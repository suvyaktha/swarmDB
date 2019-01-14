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

// accepting an incoming connection
session::session(std::shared_ptr<bzn::asio::io_context_base> io_context, const bzn::session_id session_id, std::shared_ptr<bzn::beast::websocket_stream_base> websocket, std::shared_ptr<bzn::chaos_base> chaos, bzn::protobuf_handler handler)
    : io_context(std::move(io_context))
    , session_id(session_id)
    , ep(std::move(ep))
    , websocket(std::move(websocket))
    , chaos(std::move(chaos))
    , proto_handler(std::move(proto_handler))
{
    this->websocket->async_accept(
            [self = shared_from_this()](boost::system::error_code ec)
            {
                if (ec)
                {
                    LOG(error) << "websocket accept failed: " << ec.message();
                    return;
                }

                self->do_read();
                self->do_write();
            }
    );
}

// initiating a connection
session::session(std::shared_ptr<bzn::asio::io_context_base> io_context, const bzn::session_id session_id, boost::asio::ip::tcp::endpoint ep, std::shared_ptr<bzn::chaos_base> chaos, bzn::protobuf_handler handler)
        : io_context(std::move(io_context))
        , session_id(session_id)
        , ep(std::move(ep))
        , chaos(std::move(chaos))
        , proto_handler(std::move(proto_handler))
{
    this->open_connection();
}

void
session::open_connection()
{
    std::shared_ptr<bzn::asio::tcp_socket_base> socket = this->io_context->make_unique_tcp_socket();
    socket->async_connect(this->ep,
                          [self = shared_from_this(), socket](const boost::system::error_code& ec)
                          {
                              if (ec)
                              {
                                  LOG(error) << "failed to connect to: " << ep.address().to_string() << ":" << ep.port() << " - " << ec.message();

                                  return;
                              }

                              // we've completed the handshake...
                              self->ws = self->websocket->make_unique_websocket_stream(socket->get_tcp_socket());
                              self->ws->async_handshake(ep.address().to_string(), "/",
                                                  [self, ws](const boost::system::error_code& ec)
                                                  {
                                                      if (ec)
                                                      {
                                                          LOG(error) << "handshake failed: " << ec.message();

                                                          return;
                                                      }

                                                      self->do_read();
                                                      self->do_write();
                                                  });
                          });
}

void
session::do_read()
{
    auto buffer = std::make_shared<boost::beast::multi_buffer>();
    std::lock_guard<std::mutex> lock(this->socket_lock);

    if (this->reading || !this->is_open())
    {
        return;
    }

    this->reading = true;

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

                self->reading = false;
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
    if(this->writing || !this->is_open() || this->write_queue.empty())
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

                {
                    std::lock_guard<std::mutex> lock(this->socket_lock);
                    this->write_queue.push_front(msg);
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
        LOG(debug) << "chaos testing delaying message";
        this->chaos->reschedule_message(std::bind(static_cast<void(session::*)(std::shared_ptr<std::string>, const bool)>(&session::send_message), shared_from_this(), std::move(msg)));
        return;
    }

    if (this->chaos->is_message_dropped())
    {
        LOG(debug) << "chaos testing dropping message";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(this->socket_lock);
        this->write_queue.push_back(msg);
    }

    this->do_write();
    //TODO: trigger session opening if its not open already
}

void
session::close()
{
    // TODO: also re-open socket if we still have messages to send?

    std::lock_guard<std::mutex> lock(this->socket_lock);
    if (this->closing)
    {
        return;
    }

    this->closing = true;

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
