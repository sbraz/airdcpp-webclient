/*
* Copyright (C) 2011-2015 AirDC++ Project
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#ifndef DCPLUSPLUS_DCPP_SESSION_H
#define DCPLUSPLUS_DCPP_SESSION_H

#include <web-server/stdinc.h>

#include <web-server/LazyInitWrapper.h>
#include <web-server/SessionListener.h>
#include <web-server/WebUser.h>

#include <api/ApiModule.h>

#include <airdcpp/GetSet.h>
#include <airdcpp/typedefs.h>
#include <airdcpp/Speaker.h>

namespace webserver {
	// Sessions are owned by WebUserManager and WebSockets (websockets are closed when session is removed)
	class Session : public Speaker<SessionListener> {
	public:
		Session(WebUserPtr& aUser, const std::string& aToken, bool aIsSecure, WebServerManager* aServer, uint64_t maxInactivityMinutes, bool aIsUserSession);
		~Session();

		const std::string& getToken() const noexcept {
			return token;
		}

		WebUserPtr getUser() {
			return user;
		}

		bool isSecure() const {
			return secure;
		}

		ApiModule* getModule(const std::string& aApiID);

		websocketpp::http::status_code::value handleRequest(ApiRequest& aRequest);

		Session(Session&) = delete;
		Session& operator=(Session&) = delete;
		GETSET(bool, userAway, UserAway);

		void onSocketConnected(const WebSocketPtr& aSocket) noexcept;
		void onSocketDisconnected() noexcept;
		
		WebServerManager* getServer() noexcept {
			return server;
		}

		void updateActivity() noexcept;
		uint64_t getLastActivity() const noexcept {
			return lastActivity;
		}

		uint64_t getMaxInactivity() const noexcept {
			return maxInactivity;
		}

		bool isUserSession() const noexcept {
			return userSession;
		}
	private:
		typedef LazyInitWrapper<ApiModule> LazyModuleWrapper;
		std::map<std::string , LazyModuleWrapper> apiHandlers;

		const uint64_t maxInactivity;
		const time_t started;
		uint64_t lastActivity;

		const std::string  token;
		const bool secure;
		const bool userSession;

		WebUserPtr user;
		WebServerManager* server;
	};
}

#endif