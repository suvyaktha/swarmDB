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

#pragma once

#include <include/bluzelle.hpp>
#include <proto/bluzelle.pb.h>


namespace bzn
{
    // forward declare...
    class session_base;

    using message_handler = std::function<void(const bzn::json_message& msg, std::shared_ptr<bzn::session_base> session)>;
    using protobuf_handler = std::function<void(const bzn_envelope& msg, std::shared_ptr<bzn::session_base> session)>;

    class session_base
    {
    public:
        virtual ~session_base() = default;

        /**
         * Send a message to the connected node
         * @param msg message
         */
        virtual void send_message(std::shared_ptr<bzn::encoded_message> msg) = 0;

        /**
         * Perform an orderly shutdown of the websocket.
         */
        virtual void close() = 0;

        /**
         * Is the underlying socket open? (subject to race conditions)
         */
        virtual bool is_open() const = 0;

        /**
         * Get the id associated with this session
         * @return id
         */
        virtual bzn::session_id get_session_id() = 0;
    };

} // bzn
