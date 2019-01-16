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

#include <include/bluzelle.hpp>
#include <node/node.hpp>
#include <node/session.hpp>

using namespace bzn;

namespace
{
    const std::string BZN_API_KEY = "bzn-api";
}


node::node(std::shared_ptr<bzn::asio::io_context_base> io_context, std::shared_ptr<bzn::beast::websocket_base> websocket, std::shared_ptr<chaos_base> chaos,
    const boost::asio::ip::tcp::endpoint& ep, std::shared_ptr<bzn::crypto_base> crypto, std::shared_ptr<bzn::options_base> options)
    : tcp_acceptor(io_context->make_unique_tcp_acceptor(ep))
    , io_context(std::move(io_context))
    , websocket(std::move(websocket))
    , chaos(std::move(chaos))
    , crypto(std::move(crypto))
    , options(std::move(options))
{
}

void
node::start()
{
    std::call_once(this->start_once, &node::do_accept, this);
}

bool
node::register_for_message(const bzn_envelope::PayloadCase type, bzn::protobuf_handler msg_handler)
{
    std::lock_guard<std::mutex> lock(this->message_map_mutex);

    // never allow!
    if (!msg_handler)
    {
        return false;
    }

    if (this->protobuf_map.find(type) != this->protobuf_map.end())
    {
        LOG(debug) << type << " message type already registered";

        return false;
    }

    this->protobuf_map[type] = std::move(msg_handler);

    return true;
}


void
node::do_accept()
{
    this->acceptor_socket = this->io_context->make_unique_tcp_socket();

    this->tcp_acceptor->async_accept(*this->acceptor_socket,
        [self = shared_from_this()](const boost::system::error_code& ec)
        {
            if (ec)
            {
                LOG(error) << "accept failed: " << ec.message();
            }
            else
            {
                auto ep = self->acceptor_socket->remote_endpoint();
                auto key = self->key_from_ep(ep);

                auto ws = self->websocket->make_unique_websocket_stream(
                    self->acceptor_socket->get_tcp_socket());

                auto session = std::make_shared<bzn::session>(self->io_context, ++self->session_id_counter, std::move(ws), self->chaos, std::bind(&node::priv_protobuf_handler, self, std::placeholders::_1, std::placeholders::_2));

                std::lock_guard<std::mutex> lock(self->session_map_mutex);
                if (self->sessions.find(key) == self->sessions.end())
                {
                    LOG(info) << "accepting new incoming connection with " << key;
                    self->sessions.insert(std::make_pair(key, session));
                }
                else if (self->sessions.at(key)->is_open())
                {
                    LOG(info) << "accepting new incoming connection with " << key << " while we already have one?";
                    // do not replace the existing, valid session
                }
                else
                {
                    LOG(info) << "accepting new incoming connection with " << key << "; replaces closed session";
                    self->sessions.insert_or_assign(key, session);
                }
            }

            self->do_accept();
        });
}

void
node::priv_protobuf_handler(const bzn_envelope& msg, std::shared_ptr<bzn::session_base> session)
{
    std::lock_guard<std::mutex> lock(this->message_map_mutex);

    if ((!msg.sender().empty()) && (!this->crypto->verify(msg)))
    {
        LOG(error) << "Dropping message with invalid signature: " << msg.ShortDebugString().substr(0, MAX_MESSAGE_SIZE);
        return;
    }

    if (auto it = this->protobuf_map.find(msg.payload_case()); it != this->protobuf_map.end())
    {
        it->second(msg, std::move(session));
    }
    else
    {
        LOG(debug) << "no handler for message type " << msg.payload_case();
    }

}

void
node::send_message_str(const boost::asio::ip::tcp::endpoint& ep, std::shared_ptr<bzn::encoded_message> msg) {
    std::shared_ptr<bzn::session_base> session;
    {
        std::lock_guard<std::mutex> lock(this->session_map_mutex);
        auto key = this->key_from_ep(ep);

        if (this->sessions.find(key) == this->sessions.end()) {
            auto session = std::make_shared<bzn::session>(this->io_context, ++this->session_id_counter, this->websocket, ep,
                                                               this->chaos, std::bind(&node::priv_protobuf_handler,
                                                                                      shared_from_this(),
                                                                                      std::placeholders::_1,
                                                                                      std::placeholders::_2));
            sessions.insert(std::make_pair(key, session));
        }

        session = this->sessions.at(key);
    }

    session->send_message(msg);
}


void
node::send_message(const boost::asio::ip::tcp::endpoint& ep, std::shared_ptr<bzn_envelope> msg)
{
    if(msg->sender().empty())
    {
        msg->set_sender(this->options->get_uuid());
    }

    if (msg->signature().empty())
    {
        this->crypto->sign(*msg);
    }

    this->send_message_str(ep, std::make_shared<std::string>(msg->SerializeAsString()));
}

std::string
node::key_from_ep(const boost::asio::ip::tcp::endpoint &ep)
{
    return ep.address().to_string() + ":" + std::to_string(ep.port());
}