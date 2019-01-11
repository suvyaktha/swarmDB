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

#include <node/session.hpp>
#include <sstream>


using namespace bzn;

session::session(std::shared_ptr<bzn::asio::io_context_base> io_context, const bzn::session_id session_id, std::shared_ptr<bzn::beast::websocket_stream_base> websocket, std::shared_ptr<bzn::chaos_base> chaos)
    : io_context(std::move(io_context))
    , session_id(session_id)
    , websocket(std::move(websocket))
    , chaos(std::move(chaos))
{
}

void
session::start(bzn::message_handler handler, bzn::protobuf_handler proto_handler)
{
    this->handler = std::move(handler);
    this->proto_handler = std::move(proto_handler);

    this->started = true;
    this->do_read();
    this->do_write();
}

void
session::do_read()
{
    auto buffer = std::make_shared<boost::beast::multi_buffer>();
    std::lock_guard<std::mutex> lock(this->socket_lock);

    this->websocket->async_read(
            *buffer, [self = shared_from_this(), buffer](boost::system::error_code ec, auto /*bytes_transferred*/)
            {
                if(ec)
                {
                    // don't log close of websocket...
                    if (ec != boost::beast::websocket::error::closed && ec != boost::asio::error::eof)
                    {
                        LOG(error) << "websocket read failed: " << ec.message();
                    }
                    self->close();
                    return;
                }

                // get the message...
                std::stringstream ss;
                ss << boost::beast::buffers(buffer->data());

                bzn_envelope proto_msg;

                if (proto_msg.ParseFromIstream(&ss))
                {
                    self->io_context->post(std::bind(self->proto_handler, self));
                }
                else
                {
                    LOG(error) << "Failed to parse incoming message";
                }

                self->do_read();
            }
    );
}

void
session::do_write()
{
    // because of this mutex
    std::lock_guard<std::mutex> lock(this->socket_lock);

    // at most one concurrent invocation can pass this check
    if(this->writing || this->write_queue.empty())
    {
        return;
    }

    // and set this flag
    this->writing = true;

    auto msg = this->write_queue.front();
    this->write_queue.pop_front();

    // so there will only be one instance of this callback
    this->websocket->get_websocket().binary(true);
    this->websocket->async_write(boost::asio::buffer(*msg),
        [self = shared_from_this(), msg](boost::system::error_code ec, auto /*bytes_transferred*/)
        {
            if(ec)
            {
                // don't log close of websocket...
                if (ec != boost::beast::websocket::error::closed && ec != boost::asio::error::eof)
                {
                    LOG(error) << "websocket read failed: " << ec.message();
                }
                self->close();
                return;
            }

            // and the flag will only be reset once after each sucessful write
            self->writing = false;
            /* multiple threads may race to perform the next do_write, but we don't care which wins. If there are no
             * others then ours definitely works because we don't try until after resetting the flag. Our resetting
             * the flag can't interfere with another do_write because no such do_write can happen until we reset the
             * flag.
             */

            self->do_write();
        })
}

void
session::send_message(std::shared_ptr<bzn::encoded_message> msg)
{
    if (this->chaos->is_message_delayed())
    {
        this->chaos->reschedule_message(std::bind(static_cast<void(session::*)(std::shared_ptr<std::string>, const bool)>(&session::send_message), shared_from_this(), std::move(msg)));
        return;
    }

    if (this->chaos->is_message_dropped())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(this->socket_lock);
        this->write_queue.push_back(msg);
    }

    if (this->started)
    {
        this->do_write();
    }
}

void
session::close()
{
    std::lock_guard<std::mutex> lock(this->socket_lock);

    if (this->websocket->is_open())
    {
        this->websocket->async_close(boost::beast::websocket::close_code::normal,
            [self = shared_from_this()](auto ec)
            {
                if (ec)
                {
                    LOG(error) << "failed to close websocket: " << ec.message();
                }
            });
    }
}

bool
session::is_open() const
{
    return this->websocket->is_open();
}
