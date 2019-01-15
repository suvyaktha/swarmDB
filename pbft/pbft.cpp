// Copyright (C) 2018 Bluzelle
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License, version 3,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.

#include <pbft/pbft.hpp>
#include <pbft/operations/pbft_memory_operation.hpp>
#include <pbft/operations/pbft_operation_manager.hpp>
#include <utils/make_endpoint.hpp>
#include <utils/bytes_to_debug_string.hpp>
#include <google/protobuf/text_format.h>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/format.hpp>
#include <cstdint>
#include <memory>
#include <algorithm>
#include <numeric>
#include <optional>
#include <iterator>
#include <crud/crud_base.hpp>
#include <random>
#include <chrono>

using namespace bzn;

pbft::pbft(
    std::shared_ptr<bzn::node_base> node
    , std::shared_ptr<bzn::asio::io_context_base> io_context
    , const bzn::peers_list_t& peers
    , std::shared_ptr<bzn::options_base> options
    , std::shared_ptr<pbft_service_base> service
    , std::shared_ptr<pbft_failure_detector_base> failure_detector
    , std::shared_ptr<bzn::crypto_base> crypto
    , std::shared_ptr<bzn::pbft_operation_manager> operation_manager
    )
    : node(std::move(node))
    , uuid(options->get_uuid())
    , options(options)
    , service(std::move(service))
    , failure_detector(std::move(failure_detector))
    , io_context(io_context)
    , audit_heartbeat_timer(this->io_context->make_unique_steady_timer())
    , crypto(std::move(crypto))
    , operation_manager(std::move(operation_manager))
{
    if (peers.empty())
    {
        throw std::runtime_error("No peers found!");
    }

    this->initialize_configuration(peers);

    // TODO: stable checkpoint should be read from disk first: KEP-494
    this->low_water_mark = this->stable_checkpoint.first;
    this->high_water_mark = this->stable_checkpoint.first + std::lround(CHECKPOINT_INTERVAL*HIGH_WATER_INTERVAL_IN_CHECKPOINTS);
    this->service->save_service_state_at(((this->next_issued_sequence_number / CHECKPOINT_INTERVAL) + 1) * CHECKPOINT_INTERVAL);
}

void
pbft::start()
{
    std::call_once(this->start_once,
        [this]()
        {
            this->node->register_for_message(bzn_envelope::PayloadCase::kPbft,
                std::bind(&pbft::handle_bzn_message, shared_from_this(), std::placeholders::_1, std::placeholders::_2));

            this->node->register_for_message(bzn_envelope::PayloadCase::kPbftMembership,
                std::bind(&pbft::handle_membership_message, shared_from_this(), std::placeholders::_1, std::placeholders::_2));

            this->node->register_for_message(bzn_envelope::kDatabaseMsg,
                std::bind(&pbft::handle_database_message, shared_from_this(), std::placeholders::_1, std::placeholders::_2));

            this->audit_heartbeat_timer->expires_from_now(HEARTBEAT_INTERVAL);
            this->audit_heartbeat_timer->async_wait(
                std::bind(&pbft::handle_audit_heartbeat_timeout, shared_from_this(), std::placeholders::_1));

            this->service->register_execute_handler(
                [weak_this = this->weak_from_this(), fd = this->failure_detector]
                (std::shared_ptr<pbft_operation> op)
                {
                    fd->request_executed(op->get_request_hash());

                    if (op->get_sequence() % CHECKPOINT_INTERVAL == 0)
                    {
                        auto strong_this = weak_this.lock();
                        if (strong_this)
                        {
                            // tell service to save the next checkpoint after this one
                            strong_this->service->save_service_state_at(op->get_sequence() + CHECKPOINT_INTERVAL);

                            strong_this->checkpoint_reached_locally(op->get_sequence());
                        }
                        else
                        {
                            throw std::runtime_error("pbft_service callback failed because pbft does not exist");
                        }
                    }
                }
            );

            this->failure_detector->register_failure_handler(
                [weak_this = this->weak_from_this()]()
                {
                    auto strong_this = weak_this.lock();
                    if (strong_this)
                    {
                        strong_this->handle_failure();
                    }
                }
                );

            this->join_swarm();
        });
}

void
pbft::handle_audit_heartbeat_timeout(const boost::system::error_code& ec)
{
    if (ec)
    {
        LOG(error) << "pbft audit heartbeat canceled? " << ec.message();
        return;
    }

    if (this->is_primary() && this->audit_enabled)
    {
        audit_message msg;
        msg.mutable_primary_status()->set_view(this->view);
        msg.mutable_primary_status()->set_primary(this->uuid);

        this->broadcast(this->wrap_message(msg));

    }

    this->audit_heartbeat_timer->expires_from_now(HEARTBEAT_INTERVAL);
    this->audit_heartbeat_timer->async_wait(std::bind(&pbft::handle_audit_heartbeat_timeout, shared_from_this(), std::placeholders::_1));
}

void
pbft::handle_bzn_message(const bzn_envelope& msg, std::shared_ptr<bzn::session_base> /*session*/)
{
    if (msg.payload_case() != bzn_envelope::kPbft)
    {
        LOG(error) << "Got misdirected message " << msg.DebugString().substr(0, MAX_MESSAGE_SIZE);
    }

    pbft_msg inner_msg;
    if (!inner_msg.ParseFromString(msg.pbft()))
    {
        LOG(error) << "Failed to parse payload of wrapped message " << msg.DebugString().substr(0, MAX_MESSAGE_SIZE);
        return;
    }

    this->handle_message(inner_msg, msg);
}

void
pbft::handle_membership_message(const bzn_envelope& msg, std::shared_ptr<bzn::session_base> session)
{
    LOG(debug) << "Received message: " << msg.ShortDebugString().substr(0, MAX_MESSAGE_SIZE);

    pbft_membership_msg inner_msg;
    if (!inner_msg.ParseFromString(msg.pbft_membership()))
    {
        LOG(error) << "Failed to parse payload of wrapped message " << msg.DebugString().substr(0, MAX_MESSAGE_SIZE);
        return;
    }

    const auto hash = this->crypto->hash(msg);

    switch (inner_msg.type())
    {
        case PBFT_MMSG_JOIN:
        case PBFT_MMSG_LEAVE:
            this->sessions_waiting_on_forwarded_requests[hash] = session;
            if (!this->is_primary())
            {
                this->forward_request_to_primary(msg);
                return;
            }
            this->handle_join_or_leave(inner_msg, session);
            break;
        case PBFT_MMSG_GET_STATE:
            this->handle_get_state(inner_msg, std::move(session));
            break;
        case PBFT_MMSG_SET_STATE:
            this->handle_set_state(inner_msg);
            break;
        case PBFT_MMSG_JOIN_RESPONSE:
            this->handle_join_response(inner_msg);
            break;
        default:
            LOG(error) << "Invalid membership message received "
                << inner_msg.DebugString().substr(0, MAX_MESSAGE_SIZE);
    }
}

void
pbft::handle_message(const pbft_msg& msg, const bzn_envelope& original_msg)
{

    LOG(debug) << "Received message: " << msg.ShortDebugString().substr(0, MAX_MESSAGE_SIZE);

    if (!this->preliminary_filter_msg(msg))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(this->pbft_lock);

    switch (msg.type())
    {
        case PBFT_MSG_PREPREPARE :
            this->handle_preprepare(msg, original_msg);
            break;
        case PBFT_MSG_PREPARE :
            this->handle_prepare(msg, original_msg);
            break;
        case PBFT_MSG_COMMIT :
            this->handle_commit(msg, original_msg);
            break;
        case PBFT_MSG_CHECKPOINT :
            this->handle_checkpoint(msg, original_msg);
            break;
        case PBFT_MSG_VIEWCHANGE :
            this->handle_viewchange(msg, original_msg);
            break;
        case PBFT_MSG_NEWVIEW :
            this->handle_newview(msg, original_msg);
            break;

        default :
            throw std::runtime_error("Unsupported message type");
    }
}

bool
pbft::preliminary_filter_msg(const pbft_msg& msg)
{
    if (!this->is_view_valid() && !(msg.type() == PBFT_MSG_CHECKPOINT || msg.type() == PBFT_MSG_VIEWCHANGE || msg.type() == PBFT_MSG_NEWVIEW))
    {
        LOG(debug) << "Dropping message because local view is invalid";
        return false;
    }

    if (auto t = msg.type();t == PBFT_MSG_PREPREPARE || t == PBFT_MSG_PREPARE || t == PBFT_MSG_COMMIT)
    {
        if (msg.view() != this->view)
        {
            LOG(debug) << "Dropping message because it has the wrong view number";
            return false;
        }

        if (msg.sequence() <= this->low_water_mark)
        {
            LOG(debug) << "Dropping message because it has an unreasonable sequence number " << msg.sequence();
            return false;
        }

        if (msg.sequence() > this->high_water_mark)
        {
            LOG(debug) << "Dropping message because it has an unreasonable sequence number " << msg.sequence();
            return false;
        }
    }

    return true;
}

std::shared_ptr<pbft_operation>
pbft::setup_request_operation(const bzn_envelope& request_env, const bzn::hash_t& request_hash)
{
    const uint64_t request_seq = this->next_issued_sequence_number++;
    auto op = this->operation_manager->find_or_construct(this->view, request_seq, request_hash, this->current_peers_ptr());
    op->record_request(request_env);

    return op;
}

void
pbft::handle_request(const bzn_envelope& request_env, const std::shared_ptr<session_base>& session)
{
    const auto hash = this->crypto->hash(request_env);

    if (session)
    {
        if (this->sessions_waiting_on_forwarded_requests.find(hash) == this->sessions_waiting_on_forwarded_requests.end())
        {
            this->sessions_waiting_on_forwarded_requests[hash] = session;
        }
    }

    if (!this->is_primary())
    {
        this->forward_request_to_primary(request_env);
        return;
    }

    if (request_env.timestamp() < (this->now() - MAX_REQUEST_AGE_MS) || request_env.timestamp() > (this->now() + MAX_REQUEST_AGE_MS))
    {
        // TODO: send error message to client
        LOG(info) << "Rejecting request because it is outside allowable timestamp range: " << request_env.ShortDebugString();
        return;
    }

    // keep track of what requests we've seen based on timestamp and only send preprepares once
    if (this->already_seen_request(request_env, hash))
    {
        // TODO: send error message to client
        LOG(info) << "Rejecting duplicate request: " << request_env.ShortDebugString();
        return;
    }
    this->saw_request(request_env, hash);
    auto op = setup_request_operation(request_env, hash);
    this->do_preprepare(op);
}

void
pbft::forward_request_to_primary(const bzn_envelope& request_env)
{
    LOG(info) << "Forwarding request to primary";
    this->node->send_message(bzn::make_endpoint(this->get_primary()), std::make_shared<bzn_envelope>(request_env));

    const bzn::hash_t req_hash = this->crypto->hash(request_env);

    this->failure_detector->request_seen(req_hash);
}

void
pbft::maybe_record_request(const pbft_msg& msg, const std::shared_ptr<pbft_operation>& op)
{
    if (msg.has_request() && !op->has_request())
    {
        if (this->crypto->hash(msg.request()) != msg.request_hash())
        {
            LOG(info) << "Not recording request because its hash does not match";
            return;
        }

        op->record_request(msg.request());
    }
}

void
pbft::handle_preprepare(const pbft_msg& msg, const bzn_envelope& original_msg)
{
    // If we've already accepted a preprepare for this view+sequence, and it's not this one, then we should reject this one
    // Note that if we get the same preprepare more than once, we can still accept it
    const log_key_t log_key(msg.view(), msg.sequence());

    if (auto lookup = this->accepted_preprepares.find(log_key);
        lookup != this->accepted_preprepares.end()
        && std::get<2>(lookup->second) != msg.request_hash())
    {

        LOG(debug) << "Rejecting preprepare because I've already accepted a conflicting one \n";
        return;
    }
    else
    {
        auto op = this->operation_manager->find_or_construct(msg, this->current_peers_ptr());
        op->record_pbft_msg(msg, original_msg);
        this->maybe_record_request(msg, op);

        // This assignment will be redundant if we've seen this preprepare before, but that's fine
        accepted_preprepares[log_key] = op->get_operation_key();

        if (op->has_config_request())
        {
            this->handle_config_message(msg, op);
        }

        this->do_preprepared(op);
        this->maybe_advance_operation_state(op);
    }
}

void
pbft::handle_prepare(const pbft_msg& msg, const bzn_envelope& original_msg)
{
    // Prepare messages are never rejected, assuming the sanity checks passed
    auto op = this->operation_manager->find_or_construct(msg, this->current_peers_ptr());

    op->record_pbft_msg(msg, original_msg);
    this->maybe_record_request(msg, op);
    this->maybe_advance_operation_state(op);
}

void
pbft::handle_commit(const pbft_msg& msg, const bzn_envelope& original_msg)
{
    // Commit messages are never rejected, assuming  the sanity checks passed
    auto op = this->operation_manager->find_or_construct(msg, this->current_peers_ptr());

    op->record_pbft_msg(msg, original_msg);
    this->maybe_record_request(msg, op);
    this->maybe_advance_operation_state(op);
}

void
pbft::handle_join_or_leave(const pbft_membership_msg& msg, std::shared_ptr<bzn::session_base> session)
{
    if (msg.has_peer_info())
    {
        // build a peer_address_t from the message
        auto const &peer_info = msg.peer_info();
        bzn::peer_address_t peer(peer_info.host(), static_cast<uint16_t>(peer_info.port()),
            static_cast<uint16_t>(peer_info.http_port()), peer_info.name(), peer_info.uuid());

        auto config = std::make_shared<pbft_configuration>(*(this->configurations.current()));
        if (msg.type() == PBFT_MMSG_JOIN)
        {
            // see if we can add this peer
            if (!config->add_peer(peer))
            {
                LOG(debug) << "Can't add new peer due to conflict";

                if (session && session->is_open())
                {
                    pbft_membership_msg response;
                    response.set_type(PBFT_MMSG_JOIN_RESPONSE);
                    response.set_result(false);
                    auto env = this->wrap_message(response);
                    session->send_message(std::make_shared<std::string>(env.SerializeAsString()));
                }
                return;
            }
        }
        else if (msg.type() == PBFT_MMSG_LEAVE)
        {
            if (!config->remove_peer(peer))
            {
                // TODO - respond with negative result?
                LOG(debug) << "Couldn't remove requested peer";
                return;
            }
        }

        this->configurations.add(config);
        this->broadcast_new_configuration(config);
    }
    else
    {
        LOG(debug) << "Malformed join/leave message";
    }
}

void
pbft::handle_join_response(const pbft_membership_msg& msg)
{
    if (!this->in_swarm)
    {
        if (msg.result())
        {
            this->in_swarm = true;
            LOG(debug) << "Successfully joined the swarm, waiting for NEW_VIEW message...";
        }
        else
        {
            LOG(error) << "Request to join swarm rejected. Aborting...";
            throw (std::runtime_error("Request to join swarm rejected."));
        }
    }
    else
    {
        LOG(error) << "Received JOIN response when not waiting to join swarm";
    }
}

void
pbft::handle_get_state(const pbft_membership_msg& msg, std::shared_ptr<bzn::session_base> session) const
{
    LOG(debug) << boost::format("Got request for state data for checkpoint: seq: %1%, hash: %2%")
                  % msg.sequence() % msg.state_hash();

    // get stable checkpoint for request
    checkpoint_t req_cp(msg.sequence(), msg.state_hash());

    if (req_cp == this->latest_stable_checkpoint())
    {
        auto state = this->get_checkpoint_state(req_cp);
        if (!state)
        {
            LOG(debug) << boost::format("I'm missing data for checkpoint: seq: %1%, hash: %2%")
                          % msg.sequence() % msg.state_hash();
            // TODO: send error response
            return;
        }

        pbft_membership_msg reply;
        reply.set_type(PBFT_MMSG_SET_STATE);
        reply.set_sequence(req_cp.first);
        reply.set_state_hash(req_cp.second);
        reply.set_state_data(*state);
        if (this->saved_newview)
        {
            reply.set_allocated_newview_msg(new bzn_envelope(*this->saved_newview));
        }

        auto msg_ptr = std::make_shared<bzn::encoded_message>(this->wrap_message(reply).SerializeAsString());
        session->send_message(msg_ptr);
    }
    else
    {
        LOG(debug) << boost::format("Request for checkpoint that I don't have: seq: %1%, hash: %2%")
            % msg.sequence() % msg.state_hash();
        // TODO: send error response
    }
}

void
pbft::handle_set_state(const pbft_membership_msg& msg)
{
    checkpoint_t cp(msg.sequence(), msg.state_hash());

    // do we need this checkpoint state?
    // make sure we don't have this checkpoint locally, but do know of a stablized one
    // based on the messages sent by peers.
    if (this->unstable_checkpoint_proofs[cp].size() >= this->quorum_size() &&
        this->local_unstable_checkpoints.count(cp) == 0)
    {
        LOG(info) << boost::format("Adopting checkpoint %1% at seq %2%")
            % cp.second % cp.first;

        // TODO: validate the state data
        this->set_checkpoint_state(cp, msg.state_data());

        if (msg.has_newview_msg())
        {
            pbft_msg newview;
            if (newview.ParseFromString(msg.newview_msg().pbft()) && this->is_valid_newview_message(newview, msg.newview_msg()))
            {
                this->view = newview.view();
                LOG(info) << "setting view to " << this->view;
            }
        }

        this->stabilize_checkpoint(cp);
    }
    else
    {
        LOG(debug) << boost::format("Sent state for checkpoint that I don't need: seq: %1%, hash: %2%")
            % msg.sequence() % msg.state_hash();
    }
}

void
pbft::broadcast(const bzn_envelope& msg)
{
    auto msg_ptr = std::make_shared<bzn_envelope>(msg);

    for (const auto& peer : this->current_peers())
    {
        this->node->send_message(make_endpoint(peer), msg_ptr);
    }
}

void
pbft::maybe_advance_operation_state(const std::shared_ptr<pbft_operation>& op)
{
    if (op->get_stage() == pbft_operation_stage::prepare && op->is_prepared())
    {
        this->do_prepared(op);
    }

    if (op->get_stage() == pbft_operation_stage::commit && op->is_committed())
    {
        this->do_committed(op);
    }
}

pbft_msg
pbft::common_message_setup(const std::shared_ptr<pbft_operation>& op, pbft_msg_type type)
{
    pbft_msg msg;
    msg.set_view(op->get_view());
    msg.set_sequence(op->get_sequence());
    msg.set_request_hash(op->get_request_hash());
    msg.set_type(type);

    return msg;
}

void
pbft::do_preprepare(const std::shared_ptr<pbft_operation>& op)
{
    LOG(debug) << "Doing preprepare for operation " << op->get_sequence();

    pbft_msg msg = this->common_message_setup(op, PBFT_MSG_PREPREPARE);
    msg.set_allocated_request(new bzn_envelope(op->get_request()));

    this->broadcast(this->wrap_message(msg));
}

void
pbft::do_preprepared(const std::shared_ptr<pbft_operation>& op)
{
    LOG(debug) << "Entering prepare phase for operation " << op->get_sequence();

    pbft_msg msg = this->common_message_setup(op, PBFT_MSG_PREPARE);

    this->broadcast(this->wrap_message(msg));
}

void
pbft::do_prepared(const std::shared_ptr<pbft_operation>& op)
{
    // accept new configuration if applicable
    if (op->has_config_request())
    {
        pbft_configuration config;
        if (config.from_string(op->get_config_request().configuration()))
        {
            this->configurations.enable(config.get_hash());
        }
    }

    LOG(debug) << "Entering commit phase for operation " << op->get_sequence();
    op->advance_operation_stage(pbft_operation_stage::commit);

    pbft_msg msg = this->common_message_setup(op, PBFT_MSG_COMMIT);

    this->broadcast(this->wrap_message(msg));
}

void
pbft::do_committed(const std::shared_ptr<pbft_operation>& op)
{
    LOG(debug) << "Operation " << op->get_sequence() << " is committed-local";
    op->advance_operation_stage(pbft_operation_stage::execute);

    // If we have a pending session for this request, attach to the operation just before we pass off to the service.
    // If we were to do that before this moment, then maybe the pbft_operation we attached it to never gets executed and
    // it gets executed in a different view (earlier or later), so the client never gets a response. If we do it here,
    // then the only reason this pbft_operation instance wouldn't be executed eventually is if the service already has
    // a pbft_operation for this sequence, meaning we committed this before we got the request, or if we crash, which
    // kills the session anyway.

    const auto session = this->sessions_waiting_on_forwarded_requests.find(op->get_request_hash());
    if (session != this->sessions_waiting_on_forwarded_requests.end() && !op->has_session())
    {
        op->set_session(session->second);
    }

    // commit new configuration if applicable
    if (op->has_config_request())
    {
        pbft_configuration config;
        if (config.from_string(op->get_config_request().configuration()))
        {
            // get rid of all other previous configs, except for currently active one
            this->configurations.remove_prior_to(config.get_hash());
        }

        // send response to new node
        auto session = op->session();
        if (session && session->is_open())
        {
            pbft_membership_msg response;
            response.set_type(PBFT_MMSG_JOIN_RESPONSE);
            response.set_result(true);
            auto env = this->wrap_message(response);
            session->send_message(std::make_shared<std::string>(env.SerializeAsString()));

            // TODO: start timer for sending viewchange KEP-825

        }
        else
        {
            LOG(debug) << "Unable to send join response, session is not valid";
        }
    }

    if (this->audit_enabled)
    {
        audit_message msg;
        msg.mutable_pbft_commit()->set_operation(op->get_request_hash());
        msg.mutable_pbft_commit()->set_sequence_number(op->get_sequence());
        msg.mutable_pbft_commit()->set_sender_uuid(this->uuid);

        this->broadcast(this->wrap_message(msg));
    }

    // TODO: this needs to be refactored to be service-agnostic
    if (op->has_db_request())
    {
        this->io_context->post(std::bind(&pbft_service_base::apply_operation, this->service, op));
    }
    else
    {
        // the service needs sequentially sequenced operations. post a null request to fill in this hole
        auto msg = new database_msg;
        msg->set_allocated_nullmsg(new database_nullmsg);
        bzn_envelope request;
        request.set_database_msg(msg->SerializeAsString());
        auto new_op = std::make_shared<pbft_memory_operation>(op->get_view(), op->get_sequence()
            , this->crypto->hash(request), nullptr);
        new_op->record_request(request);
        this->io_context->post(std::bind(&pbft_service_base::apply_operation, this->service, new_op));
    }
}

bool
pbft::is_primary() const
{
    return this->get_primary().uuid == this->uuid;
}

const peer_address_t&
pbft::get_primary(std::optional<uint64_t> view) const
{
    return this->current_peers()[view.value_or(this->view) % this->current_peers().size()];
}

bzn_envelope
pbft::wrap_message(const pbft_msg& msg) const
{
    bzn_envelope result;
    result.set_pbft(msg.SerializeAsString());
    result.set_sender(this->uuid);
    result.set_timestamp(this->now());
    this->crypto->sign(result);

    return result;
}

bzn_envelope
pbft::wrap_message(const pbft_membership_msg& msg) const
{
    bzn_envelope result;
    result.set_pbft_membership(msg.SerializeAsString());
    result.set_sender(this->uuid);
    result.set_timestamp(this->now());
    this->crypto->sign(result);
    
    return result;
}

bzn_envelope
pbft::wrap_message(const audit_message& msg) const
{
    bzn_envelope result;
    result.set_audit(msg.SerializeAsString());
    result.set_sender(this->uuid);
    result.set_timestamp(this->now());
    this->crypto->sign(result);

    return result;
}

const bzn::uuid_t&
pbft::get_uuid() const
{
    return this->uuid;
}

void
pbft::set_audit_enabled(bool setting)
{
    this->audit_enabled = setting;
}

void
pbft::notify_audit_failure_detected()
{
    if (this->audit_enabled)
    {
        audit_message msg;
        msg.mutable_failure_detected()->set_sender_uuid(this->uuid);
        this->broadcast(this->wrap_message(msg));
    }
}

void
pbft::handle_failure()
{
    std::lock_guard<std::mutex> lock(this->pbft_lock);
    LOG (error) << "handle_failure - PBFT failure - invalidating current view and sending VIEWCHANGE to view: " << this->view + 1;
    this->notify_audit_failure_detected();
    this->view_is_valid = false;
    auto ops = this->operation_manager->prepared_operations_since(this->latest_stable_checkpoint().first);
    pbft_msg view_change{pbft::make_viewchange(this->view + 1, this->latest_stable_checkpoint().first, this->stable_checkpoint_proof, ops)};
    this->broadcast(this->wrap_message(view_change));
}

void
pbft::checkpoint_reached_locally(uint64_t sequence)
{
    std::lock_guard<std::mutex> lock(this->pbft_lock);

    LOG(info) << "Reached checkpoint " << sequence;

    auto cp = this->local_unstable_checkpoints.emplace(sequence, this->service->service_state_hash(sequence)).first;

    pbft_msg cp_msg;
    cp_msg.set_type(PBFT_MSG_CHECKPOINT);
    cp_msg.set_view(this->view);
    cp_msg.set_sequence(sequence);
    cp_msg.set_state_hash(cp->second);

    this->broadcast(this->wrap_message(cp_msg));

    this->maybe_stabilize_checkpoint(*cp);
}

void
pbft::handle_checkpoint(const pbft_msg& msg, const bzn_envelope& original_msg)
{
    if (msg.sequence() <= stable_checkpoint.first)
    {
        LOG(debug) << boost::format("Ignoring checkpoint message for seq %1% because I already have a stable checkpoint at seq %2%")
                   % msg.sequence()
                   % stable_checkpoint.first;
        return;
    }

    LOG(info) << boost::format("Received checkpoint message for seq %1% from %2%")
              % msg.sequence()
              % original_msg.sender();

    checkpoint_t cp(msg.sequence(), msg.state_hash());

    this->unstable_checkpoint_proofs[cp][original_msg.sender()] = original_msg.SerializeAsString();
    this->maybe_stabilize_checkpoint(cp);
}

bzn::checkpoint_t
pbft::latest_stable_checkpoint() const
{
    return this->stable_checkpoint;
}

bzn::checkpoint_t
pbft::latest_checkpoint() const
{
    return this->local_unstable_checkpoints.empty() ? this->stable_checkpoint : *(this->local_unstable_checkpoints.rbegin());
}

size_t
pbft::unstable_checkpoints_count() const
{
    return this->local_unstable_checkpoints.size();
}

void
pbft::maybe_stabilize_checkpoint(const checkpoint_t& cp)
{
    if (this->unstable_checkpoint_proofs[cp].size() < this->quorum_size())
    {
        return;
    }

    if (this->local_unstable_checkpoints.count(cp) != 0)
    {
        this->stabilize_checkpoint(cp);
    }
    else
    {
        // we don't have this checkpoint, so we need to catch up
        this->request_checkpoint_state(cp);
    }
}

void
pbft::stabilize_checkpoint(const checkpoint_t& cp)
{
    this->stable_checkpoint = cp;
    this->stable_checkpoint_proof = this->unstable_checkpoint_proofs[cp];

    LOG(info) << boost::format("Checkpoint %1% at seq %2% is now stable; clearing old data")
        % cp.second % cp.first;

    this->clear_local_checkpoints_until(cp);
    this->clear_checkpoint_messages_until(cp);
    this->operation_manager->delete_operations_until(cp.first);

    this->low_water_mark = std::max(this->low_water_mark, cp.first);
    this->high_water_mark = std::max(this->high_water_mark,
        cp.first + std::lround(HIGH_WATER_INTERVAL_IN_CHECKPOINTS * CHECKPOINT_INTERVAL));

    // remove seen requests older than our time threshold
    this->recent_requests.erase(this->recent_requests.begin(),
        this->recent_requests.upper_bound(this->now() - MAX_REQUEST_AGE_MS));
}

void
pbft::request_checkpoint_state(const checkpoint_t& cp)
{
    pbft_membership_msg msg;
    msg.set_type(PBFT_MMSG_GET_STATE);
    msg.set_sequence(cp.first);
    msg.set_state_hash(cp.second);

    auto selected = this->select_peer_for_checkpoint(cp);
    LOG(info) << boost::format("Requesting checkpoint state for hash %1% at seq %2% from %3%")
        % cp.second % cp.first % selected.uuid;

    // TODO: fix the race condition here where receiving node may not have had time to
    //  stabilize its checkpoint yet.
    auto msg_ptr = std::make_shared<bzn_envelope>(this->wrap_message(msg));
    this->node->send_message(make_endpoint(selected), msg_ptr);
}

const peer_address_t&
pbft::select_peer_for_checkpoint(const checkpoint_t& cp)
{
    // choose one of the peers who vouch for this checkpoint at random
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, this->unstable_checkpoint_proofs[cp].size() - 1);

    auto it = this->unstable_checkpoint_proofs[cp].begin();
    uint32_t selected = dist(gen);
    for (size_t i = 0; i < selected; i++)
    {
        it++;
    }

    return this->get_peer_by_uuid(it->first);
}

std::shared_ptr<std::string>
pbft::get_checkpoint_state(const checkpoint_t& cp) const
{
    // call service to retrieve state at this checkpoint
    return this->service->get_service_state(cp.first);
}

void
pbft::set_checkpoint_state(const checkpoint_t& cp, const std::string& data)
{
    // set the service state at the given checkpoint sequence
    // the service is expected to load the state and discard any pending operations
    // prior to the sequence number, then execute any subsequent operations sequentially
    this->service->set_service_state(cp.first, data);
}

void
pbft::clear_local_checkpoints_until(const checkpoint_t& cp)
{
    const auto local_start = this->local_unstable_checkpoints.begin();
    // Iterator to the first unstable checkpoint that's newer than this one. This logic assumes that CHECKPOINT_INTERVAL
    // is >= 2, otherwise we would have do do something awkward here
    const auto local_end = this->local_unstable_checkpoints.upper_bound(checkpoint_t(cp.first+1, ""));
    const size_t local_removed = std::distance(local_start, local_end);
    this->local_unstable_checkpoints.erase(local_start, local_end);
    LOG(debug) << boost::format("Cleared %1% unstable local checkpoints") % local_removed;
}

void
pbft::clear_checkpoint_messages_until(const checkpoint_t& cp)
{
    const auto start = this->unstable_checkpoint_proofs.begin();
    const auto end = this->unstable_checkpoint_proofs.upper_bound(checkpoint_t(cp.first+1, ""));
    const size_t to_remove = std::distance(start, end);
    this->unstable_checkpoint_proofs.erase(start, end);
    LOG(debug) << boost::format("Cleared %1% unstable checkpoint proof sets") % to_remove;
}

size_t
pbft::quorum_size() const
{
    return 1 + (2*this->max_faulty_nodes());
}

size_t
pbft::max_faulty_nodes() const
{
    return this->current_peers().size()/3;
}

void
pbft::handle_database_message(const bzn_envelope& msg, std::shared_ptr<bzn::session_base> session)
{
    // TODO: timestamp should be set by the client. setting it here breaks the signature (which is correct).
    bzn_envelope mutable_msg(msg);
    if (msg.timestamp() == 0)
    {
        mutable_msg.set_timestamp(this->now());
    }

    LOG(debug) << "got database message";

    if (!this->service->apply_operation_now(msg, session))
    {
        this->handle_request(mutable_msg, session);
    }
}

uint64_t
pbft::get_low_water_mark()
{
    return this->low_water_mark;
}

uint64_t
pbft::get_high_water_mark()
{
    return this->high_water_mark;
}

bool
pbft::is_view_valid() const
{
    return this->view_is_valid;
}

uint64_t
pbft::get_view() const
{
    return this->view;
}

bool
pbft::is_peer(const bzn::uuid_t& sender) const
{
    return std::find_if (std::begin(this->current_peers()), std::end(this->current_peers()), [&](const auto& address)
    {
        return sender== address.uuid;
    }) != this->current_peers().end();
}

std::map<bzn::checkpoint_t, std::set<bzn::uuid_t>>
pbft::validate_and_extract_checkpoint_hashes(const pbft_msg &viewchange_message) const
{
    std::map<bzn::checkpoint_t , std::set<bzn::uuid_t>> checkpoint_hashes;
    for (size_t i{0}; i < static_cast<uint64_t>(viewchange_message.checkpoint_messages_size()); ++i)
    {
        const bzn_envelope& envelope{viewchange_message.checkpoint_messages(i)};
        pbft_msg checkpoint_message;

        if (!this->crypto->verify(envelope) || !this->is_peer(envelope.sender()) || !checkpoint_message.ParseFromString(envelope.pbft()))
        {
            LOG (error) << "Checkpoint validation failure - unable to verify envelope";
            continue;
        }
        checkpoint_hashes[checkpoint_t(checkpoint_message.sequence(), checkpoint_message.state_hash())].insert(envelope.sender());
    }

    //  filter checkpoint_hashes to only those pairs (checkpoint, senders) such that |senders| >= 2f+1
    std::map<bzn::checkpoint_t , std::set<bzn::uuid_t>> retval;
    std::copy_if(checkpoint_hashes.begin(), checkpoint_hashes.end(), std::inserter(retval, retval.end())
        , [&](const auto& h) {return h.second.size() >= 2 * this->max_faulty_nodes() + 1;});

    return retval;
}

bool
pbft::is_valid_viewchange_message(const pbft_msg& viewchange_message, const bzn_envelope& original_msg) const
{
    if (!this->is_peer(original_msg.sender()))// TODO: Rich - If redundant, keep this check, remove the other
    {
        LOG(debug) << "is_valid_viewchange_message - message not from a peer";
        return false;
    }

    if (!(viewchange_message.view() > this->get_view()))
    {
        LOG(error) << "is_valid_viewchange_message - new view " << viewchange_message.view() << " is not greater than current view " << this->get_view();
        return false;
    }

    auto valid_checkpoint_hashes{this->validate_and_extract_checkpoint_hashes(viewchange_message)};
    if (valid_checkpoint_hashes.empty() && viewchange_message.sequence() != 0)
    {
        LOG(error) << "is_valid_viewchange_message - the checkpoint is invalid";
        return false;
    }

    // KEP-902: If we do not have a valid checkpoint hash, then we must set the valid_checkpoint_sequence value to 0.
    uint64_t valid_checkpoint_sequence = (valid_checkpoint_hashes.empty() ? 0 : valid_checkpoint_hashes.begin()->first.first);

    // Snuck into KEP-902: Isabel notes that viewchange messages should not have sequences, and this will be  refactored
    // out in the near future, but since we are using it we must make the comparison to the expected
    // valid_checkpoint_sequence
    if (viewchange_message.sequence() != valid_checkpoint_sequence)
    {
        LOG(error) << "is_valid_viewchange_message - the viewchange sequence: " << viewchange_message.sequence()
            << ", is different from the sequence that is expected: " <<  valid_checkpoint_sequence;
        return false;
    }

    // TODO: (see KEP-882) Refactor this block into a new method - Isabel: should extract a validate_prepared_proof method
    // all the the prepared proofs are valid  (contains a pre prepare and 2f+1 mathcin prepares)
    for (int i{0}; i < viewchange_message.prepared_proofs_size(); ++i)
    {
        const bzn_envelope& pre_prepare_envelope{viewchange_message.prepared_proofs(i).pre_prepare()};

        if (!this->is_peer(pre_prepare_envelope.sender()) || !this->crypto->verify(pre_prepare_envelope))
        {
            LOG(error) << "is_valid_viewchange_message - a pre prepare message has a bad envelope, or the sender is not in the peers list";
            return false;
        }

        pbft_msg preprepare_message;
        if (!preprepare_message.ParseFromString(pre_prepare_envelope.pbft()) || (preprepare_message.sequence() <= valid_checkpoint_sequence)
            || preprepare_message.type() != PBFT_MSG_PREPREPARE)
        {
            LOG(error) << "is_valid_viewchange_message - a pre prepare message has an invalid sequence number, or is malformed";
            return false;
        }

        std::set<uuid_t> senders;
        for (int j{0}; j < viewchange_message.prepared_proofs(i).prepare_size(); ++j)
        {
            bzn_envelope prepare_envelope{viewchange_message.prepared_proofs(i).prepare(j)};
            if (!this->is_peer(prepare_envelope.sender()) || !this->crypto->verify(prepare_envelope))
            {
                LOG(error) << "is_valid_viewchange_message - a prepare message has a bad envelope, "
                      "the sender may not be in the peer list, or the envelope failed cryptographic verification";
                return false;
            }

            // does the sequence number, view number and hash match those of the pre prepare
            pbft_msg prepare_msg;
            if (!prepare_msg.ParseFromString(prepare_envelope.pbft()) || prepare_msg.type() != PBFT_MSG_PREPARE)
            {
                LOG(error) << "is_valid_viewchange_message - a prepare message is invalid";
                return false;
            }

            if (preprepare_message.sequence() != prepare_msg.sequence() || preprepare_message.view() != prepare_msg.view()
                || preprepare_message.request_hash() != prepare_msg.request_hash())
            {
                LOG(error) << "is_valid_viewchange_message - a pre prepare message has mismatched sequence, view or request hash";
                return false;
            }

            senders.insert(prepare_envelope.sender());
        }

        if (senders.size() < 2 * this->max_faulty_nodes() + 1)
        {
            LOG(error) << "is_valid_viewchange_message - not enough prepares in a prepared_proof";
            return false;
        }
    }
    return true;
}

bool
pbft::get_sequences_and_request_hashes_from_proofs(
        const pbft_msg& viewchange_msg
        , std::set<std::pair<uint64_t, std::string>>& sequence_request_pairs) const
{
    for (int j{0}; j < viewchange_msg.prepared_proofs_size(); ++j)
    {
        // --- add the sequence/hash to a set
        auto const& prepared_proof = viewchange_msg.prepared_proofs(j);

        pbft_msg msg;
        if (const bzn_envelope& envelope{prepared_proof.pre_prepare()}; !this->is_peer(envelope.sender()) || !msg.ParseFromString(envelope.pbft()))
        {
            return false;
        }
        sequence_request_pairs.insert(std::make_pair(msg.sequence(), msg.request_hash()));
    }
    return true;
}

bool
pbft::is_valid_newview_message(const pbft_msg& theirs, const bzn_envelope& original_theirs) const
{
    // - does it contain 2f+1 viewchange messages
    if (static_cast<size_t>(theirs.viewchange_messages_size()) < 2 * this->max_faulty_nodes() + 1)
    {
        return false;
    }

    std::set<std::string> viewchange_senders;
    pbft_msg viewchange_msg;
    for (int i{0};i < theirs.viewchange_messages_size();++i)
    {
        const bzn_envelope& original_msg{theirs.viewchange_messages(i)};
        // - are each of those viewchange messages valid?
        if (!viewchange_msg.ParseFromString(original_msg.pbft()) || viewchange_msg.type() != PBFT_MSG_VIEWCHANGE
            || !this->is_valid_viewchange_message(viewchange_msg, original_msg))
        {
            LOG(error) << "is_valid_newview_message - new view message contains invalid viewchange message";
            return false;
        }

        if (viewchange_msg.view() != theirs.view())
        {
            LOG(error) << "is_valid_newview_message - a view change message has a different view than the new view message";
            return false;
        }
        viewchange_senders.insert(original_msg.sender());
    }

    if (viewchange_senders.size() != size_t(theirs.viewchange_messages_size()))
    {
        LOG (error) << "is_valid_newview_message - unexpected viewchange message count";
        return false;
    }

    std::map<uuid_t,bzn_envelope> viewchange_envelopes_from_senders;

    for (int i{0};i < theirs.viewchange_messages_size();++i)
    {
        auto const& viewchange_env = theirs.viewchange_messages(i);
        viewchange_envelopes_from_senders[viewchange_env.sender()] = viewchange_env;
    }

    pbft_msg ours = this->build_newview(theirs.view(), viewchange_envelopes_from_senders).first;
    if (ours.pre_prepare_messages_size() != theirs.pre_prepare_messages_size() )
    {
        LOG(error) << "is_valid_newview_message - expected " << ours.pre_prepare_messages_size()
            << " preprepares in new view, found " << theirs.pre_prepare_messages_size();
        return false;
    }

    for (int i{0};i < theirs.pre_prepare_messages_size();++i)
    {
        if (!this->crypto->verify(theirs.pre_prepare_messages(i)))
        {
            LOG(error) <<  "is_valid_newview_message - unable to verify thier pre prepare message";
            return false;
        }

        if (theirs.pre_prepare_messages(i).sender() != original_theirs.sender())
        {
            LOG(error) << "is_valid_newview_message - pre prepare messaged does not come from the correct sender";
            return false;
        }

        pbft_msg ours_pbft;
        pbft_msg theirs_pbft;

        ours_pbft.ParseFromString(ours.pre_prepare_messages(i).pbft());
        theirs_pbft.ParseFromString(theirs.pre_prepare_messages(i).pbft());

        if (ours_pbft.type() != theirs_pbft.type() || theirs_pbft.type() != PBFT_MSG_PREPREPARE || ours_pbft.view() != theirs_pbft.view()
            || ours_pbft.sequence() != theirs_pbft.sequence() || ours_pbft.request_hash() != theirs_pbft.request_hash())
        {
            LOG(error) << "is_valid_newview_message - type, view, sequence or request hash mismatch between our view change and their viewchange";
            return false;
        }
    }
    return true;
}

void
pbft::fill_in_missing_pre_prepares(uint64_t max_checkpoint_sequence, uint64_t new_view, std::map<uint64_t, bzn_envelope>& pre_prepares) const
{
    uint64_t last_sequence_number{0};
    for(const auto& pre_prepare : pre_prepares)
    {
        last_sequence_number = std::max(last_sequence_number, pre_prepare.first);
    }

    for (uint64_t i = max_checkpoint_sequence + 1; i <= last_sequence_number; ++i)
    {
        //  -- create a new preprepare for a no-op operation using this sequence number
        if (pre_prepares.find(i) == pre_prepares.end())
        {
            database_msg msg;
            msg.set_allocated_nullmsg(new database_nullmsg);

            auto request = new bzn_envelope;
            request->set_database_msg(msg.SerializeAsString());
            this->crypto->sign(*request);

            pbft_msg msg2;
            msg2.set_view(new_view);
            msg2.set_sequence(i);
            msg2.set_type(PBFT_MSG_PREPREPARE);
            msg2.set_allocated_request(request);
            msg2.set_request_hash(this->crypto->hash(*request));

            pre_prepares[i] = this->wrap_message(msg2);
        }
    }
}

pbft_msg
pbft::make_newview(
        uint64_t new_view_index
        , const std::map<uuid_t,bzn_envelope>& viewchange_envelopes_from_senders
        , const std::map<uint64_t, bzn_envelope>& pre_prepare_messages
) const
{
    pbft_msg newview;
    newview.set_type(PBFT_MSG_NEWVIEW);
    newview.set_view(new_view_index);

    // V is the set of 2f+1 view change messages
    for (const auto &sender_viewchange_envelope: viewchange_envelopes_from_senders)
    {
        *(newview.add_viewchange_messages()) = sender_viewchange_envelope.second;
    }

    // O
    for (const auto& preprepare_message : pre_prepare_messages)
    {
        *(newview.add_pre_prepare_messages()) = preprepare_message.second;
    }
    return newview;
}

std::pair<pbft_msg, uint64_t>
pbft::build_newview(uint64_t new_view, const std::map<uuid_t,bzn_envelope>& viewchange_envelopes_from_senders) const
{
    //  Computing O (set of new preprepares for new-view message)
    std::map<uint64_t, bzn_envelope> pre_prepares;

    uint64_t max_checkpoint_sequence{0};
    for (const auto& sender_viewchange_envelope : viewchange_envelopes_from_senders)
    {
        pbft_msg viewchange;
        viewchange.ParseFromString(sender_viewchange_envelope.second.pbft());
        max_checkpoint_sequence = std::max(max_checkpoint_sequence, viewchange.sequence());
    }
    uint64_t next_seq = max_checkpoint_sequence + 1;

    //  - for each of the 2f+1 viewchange messages
    for (const auto& sender_viewchange_envelope : viewchange_envelopes_from_senders)
    {
        //  -- for each operation included in that viewchange message
        pbft_msg viewchange_message;
        viewchange_message.ParseFromString(sender_viewchange_envelope.second.pbft());

        for (int i{0}; i < viewchange_message.prepared_proofs_size(); ++i)
        {
            const auto& prepared_proof = viewchange_message.prepared_proofs(i);

            //  --- if we have not already created a new preprepare for an operation with that sequence number

            //  ---- Create a new preprepare for that operation using its original sequence number
            //       and request hash, but using the new view number
            pbft_msg pre_prepare;
            pre_prepare.ParseFromString(prepared_proof.pre_prepare().pbft());

            if (pre_prepares.count(pre_prepare.sequence())>0)
            {
                continue;
            }

            pre_prepare.set_view(new_view);

            if (pre_prepare.sequence() <= max_checkpoint_sequence)
            {
                continue;
            }
            pre_prepares[pre_prepare.sequence()] = this->wrap_message(pre_prepare);
            next_seq = std::max(next_seq, pre_prepare.sequence() + 1);
        }
    }
    this->fill_in_missing_pre_prepares(max_checkpoint_sequence, new_view, pre_prepares);

    return std::make_pair(this->make_newview(new_view, viewchange_envelopes_from_senders, pre_prepares), next_seq);
}

void
pbft::save_checkpoint(const pbft_msg& msg)
{
    pbft_msg message;
    for (int i{0}; i < msg.checkpoint_messages_size(); ++i)
    {
        const bzn_envelope& original_checkpoint{msg.checkpoint_messages(i)};

        message.ParseFromString(original_checkpoint.pbft());

        if (!this->crypto->verify(original_checkpoint))
        {
            continue;
        }

        this->handle_checkpoint(message, original_checkpoint);
    }
}

void
pbft::handle_viewchange(const pbft_msg &msg, const bzn_envelope &original_msg)
{
    if (!this->is_valid_viewchange_message(msg, original_msg))
    {
        LOG(error) << "handle_viewchange - invalid viewchange message, ignoring";
        return;
    }

    this->valid_viewchange_messages_for_view[msg.view()][original_msg.sender()] = original_msg;

    this->save_checkpoint(msg);

    if (this->view_is_valid &&
        this->valid_viewchange_messages_for_view[msg.view()].size() == this->max_faulty_nodes() + 1 &&
        msg.view() > this->last_view_sent)
    {
        this->last_view_sent = msg.view();
        auto ops = this->operation_manager->prepared_operations_since(this->latest_stable_checkpoint().first);
        pbft_msg viewchange_message{make_viewchange(msg.view(), this->latest_stable_checkpoint().first, this->stable_checkpoint_proof, ops)};
        this->broadcast(this->wrap_message(viewchange_message));
        this->view_is_valid = false;
    }

    const auto viewchange = std::find_if(this->valid_viewchange_messages_for_view.begin(),
                                         this->valid_viewchange_messages_for_view.end(), [&](const auto &p)
                                         {
                                             return ((this->get_primary(p.first).uuid == this->get_uuid()) &&
                                                     (p.first > this->view) &&
                                                     (p.second.size() == 2 * this->max_faulty_nodes() + 1));
                                         });

    if (viewchange == this->valid_viewchange_messages_for_view.end())
    {
        return;
    }

    std::map<uuid_t, bzn_envelope> viewchange_envelopes_from_senders;
    for (const auto &sender_envelope : this->valid_viewchange_messages_for_view[msg.view()])
    {
        const auto &sender{sender_envelope.first};
        const auto &viewchange_envelope{sender_envelope.second};
        viewchange_envelopes_from_senders[sender] = viewchange_envelope;
    }

    auto res = this->build_newview(viewchange->first, viewchange_envelopes_from_senders);
    this->next_issued_sequence_number = res.second;
    this->broadcast(this->wrap_message(res.first));
}

void
pbft::handle_newview(const pbft_msg& msg, const bzn_envelope& original_msg)
{
    if (!this->is_valid_newview_message(msg, original_msg))
    {
        LOG (debug) << "handle_newview - ignoring invalid NEWVIEW message ";
        return;
    }

    LOG(debug) << "handle_newview - recieved valid NEWVIEW message";

    this->view = msg.view();
    this->view_is_valid = true;

    // after moving to the new view processes the preprepares
    for (size_t i{0}; i < static_cast<size_t>(msg.pre_prepare_messages_size()); ++i)
    {
        const bzn_envelope original_msg = msg.pre_prepare_messages(i);
        pbft_msg msg;
        if (msg.ParseFromString(original_msg.pbft()))
        {
            this->handle_preprepare(msg, original_msg);
        }
    }

    // save the newview message for providing with state messages
    this->saved_newview = std::make_shared<bzn_envelope>(original_msg);
}

std::string
pbft::get_name()
{
    return "pbft";
}

bzn::json_message
pbft::get_status()
{
    bzn::json_message status;

    std::lock_guard<std::mutex> lock(this->pbft_lock);

    status["outstanding_operations_count"] = uint64_t(this->operation_manager->held_operations_count());
    status["is_primary"] = this->is_primary();

    auto primary = this->get_primary();
    status["primary"]["host"] = primary.host;
    status["primary"]["host_port"] = primary.port;
    status["primary"]["http_port"] = primary.http_port;
    status["primary"]["name"] = primary.name;
    status["primary"]["uuid"] = primary.uuid;

    status["latest_stable_checkpoint"]["sequence_number"] = this->latest_stable_checkpoint().first;
    status["latest_stable_checkpoint"]["hash"] = this->latest_stable_checkpoint().second;
    status["latest_checkpoint"]["sequence_number"] = this->latest_checkpoint().first;
    status["latest_checkpoint"]["hash"] = this->latest_checkpoint().second;

    status["unstable_checkpoints_count"] = uint64_t(this->unstable_checkpoints_count());
    status["next_issued_sequence_number"] = this->next_issued_sequence_number;
    status["view"] = this->view;

    status["peer_index"] = bzn::json_message();
    for (const auto& p : this->current_peers())
    {
        bzn::json_message peer;
        peer["host"] = p.host;
        peer["port"] = p.port;
        peer["http_port"] = p.http_port;
        peer["name"] = p.name;
        peer["uuid"] = p.uuid;
        status["peer_index"].append(peer);
    }

    return status;
}

bool
pbft::initialize_configuration(const bzn::peers_list_t& peers)
{
    auto config = std::make_shared<pbft_configuration>();
    bool config_good = true;
    for (auto& p : peers)
    {
        config_good &= config->add_peer(p);
    }

    if (!config_good)
    {
        LOG(warning) << "One or more peers could not be added to configuration";
        LOG(warning) << config->get_peers()->size() << " peers were added";
    }

    this->configurations.add(config);
    this->configurations.enable(config->get_hash());
    this->configurations.set_current(config->get_hash());

    return config_good;
}

std::shared_ptr<const std::vector<bzn::peer_address_t>>
pbft::current_peers_ptr() const
{
    auto config = this->configurations.current();
    if (config)
    {
        return config->get_peers();
    }

    throw std::runtime_error("No current configuration!");
}

const std::vector<bzn::peer_address_t>&
pbft::current_peers() const
{
    return *(this->current_peers_ptr());
}

const peer_address_t&
pbft::get_peer_by_uuid(const std::string& uuid) const
{
    for (auto const& peer : this->current_peers())
    {
        if (peer.uuid == uuid)
        {
            return peer;
        }
    }

    // something went wrong. this uuid should exist
    throw std::runtime_error("peer missing from peers list");
}

void
pbft::broadcast_new_configuration(pbft_configuration::shared_const_ptr config)
{
    auto cfg_msg = new pbft_config_msg;
    cfg_msg->set_configuration(config->to_string());
    bzn_envelope req;
    req.set_pbft_internal_request(cfg_msg->SerializeAsString());

    auto op = this->setup_request_operation(req, this->crypto->hash(req));
    this->do_preprepare(op);
}

bool
pbft::is_configuration_acceptable_in_new_view(hash_t config_hash)
{
    return this->configurations.is_enabled(config_hash);
}

void
pbft::handle_config_message(const pbft_msg& msg, const std::shared_ptr<pbft_operation>& op)
{
    assert(op->has_config_request());
    auto config = std::make_shared<pbft_configuration>();
    if (msg.type() == PBFT_MSG_PREPREPARE && config->from_string(op->get_config_request().configuration()))
    {
        if (this->proposed_config_is_acceptable(config))
        {
            // store this configuration
            this->configurations.add(config);
        }
    }
}

bool
pbft::move_to_new_configuration(hash_t config_hash)
{
    if (this->configurations.is_enabled(config_hash))
    {
        this->configurations.set_current(config_hash);
        this->configurations.remove_prior_to(config_hash);
        return true;
    }

    return false;
}

bool
pbft::proposed_config_is_acceptable(std::shared_ptr<pbft_configuration> /* config */)
{
    return true;
}

timestamp_t
pbft::now() const
{
    return static_cast<timestamp_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void
pbft::saw_request(const bzn_envelope& req, const request_hash_t& hash)
{
    this->recent_requests.insert(std::make_pair(req.timestamp(),
        std::make_pair(req.sender(), hash)));
}

bool
pbft::already_seen_request(const bzn_envelope& req, const request_hash_t& hash) const
{
    auto range = this->recent_requests.equal_range(req.timestamp());
    for (auto r = range.first; r != range.second; r++)
    {
        if (r->second.first == req.sender() && r->second.second == hash)
        {
            return true;
        }
    }

    return false;
}

pbft_msg
pbft::make_viewchange(
        uint64_t new_view
        , uint64_t base_sequence_number
        , const std::unordered_map<bzn::uuid_t, std::string>& stable_checkpoint_proof
        , const std::map<uint64_t, std::shared_ptr<bzn::pbft_operation>>& prepared_operations)
{
    pbft_msg viewchange;

    viewchange.set_type(PBFT_MSG_VIEWCHANGE);
    viewchange.set_view(new_view);
    viewchange.set_sequence(base_sequence_number);  // base_sequence_number = n = sequence # of last valid checkpoint

    // C = a set of local 2*f + 1 valid checkpoint messages
    for (const auto& msg : stable_checkpoint_proof)
    {
        bzn_envelope envelope;
        envelope.ParseFromString(msg.second);
        *(viewchange.add_checkpoint_messages()) = envelope;
    }

    // P = a set (of client requests) containing a set P_m  for each request m that prepared at i with a sequence # higher than n
    // P_m = the pre prepare and the 2 f + 1 prepares
    //            get the set of operations, frome each operation get the messages..
    for (const auto& operation : prepared_operations)
    {
        prepared_proof* prepared_proof = viewchange.add_prepared_proofs();
        prepared_proof->set_allocated_pre_prepare(new bzn_envelope(operation.second->get_preprepare()));
        for(const auto& sender_envelope : operation.second->get_prepares())
        {
            *(prepared_proof->add_prepare()) = sender_envelope.second;
        }
    }

    return viewchange;
}

size_t
pbft::faulty_nodes_bound(size_t swarm_size)
{
    return (swarm_size - 1)/3;
}

size_t
pbft::honest_member_size(size_t swarm_size)
{
    return pbft::faulty_nodes_bound(swarm_size) + 1;
}

size_t
pbft::honest_majority_size(size_t swarm_size)
{
    return pbft::faulty_nodes_bound(swarm_size) * 2 + 1;
}

void
pbft::join_swarm()
{
    // are we already in the peers list?
    if (this->is_peer(this->uuid))
    {
        return;
    }

#if 0 // disabling dynamic peering for now
    auto info = new pbft_peer_info;
    info->set_host(this->options->get_listener().address().to_string());
    info->set_port(this->options->get_listener().port());
    info->set_http_port(options->get_http_port());
    info->set_uuid(this->uuid);

    pbft_membership_msg join_msg;
    join_msg.set_type(PBFT_MMSG_JOIN);
    join_msg.set_allocated_peer_info(info);

    // choose one of the peers at random
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, this->current_peers().size() - 1);
    uint32_t selected = dist(gen);

    LOG(info) << "Sending request to join swarm to node " << this->current_peers()[selected].uuid;
    auto msg_ptr = std::make_shared<bzn_envelope>(this->wrap_message(join_msg));
    this->node->send_message(make_endpoint(this->current_peers()[selected]), msg_ptr);

    // TODO: set timer and retry with different peer if we don't get a response
#else
    LOG(fatal) << "This node is not a member of the swarm, exiting.";
    throw std::runtime_error("This node is not a member of the swarm, exiting.");
#endif
}
