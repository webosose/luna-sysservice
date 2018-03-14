// Copyright (c) 2012-2018 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0


#ifndef NETWORKCONNECTIONLISTENER_H
#define NETWORKCONNECTIONLISTENER_H

#include <glib.h>
#include <luna-service2/lunaservice.h>

#include "SignalSlot.h"

class NetworkConnectionListener
{
public:

	static NetworkConnectionListener* instance();
	static void shutdown();

	bool isInternetConnectionAvailable() const { return m_isInternetConnectionAvailable; }

public:

	Signal<bool> signalConnectionStateChanged;

private:

	NetworkConnectionListener();
	~NetworkConnectionListener();

	void registerForConnectionManager();
	void unregisterFromConnectionManager();

	static bool connectionManagerConnectCallback(LSHandle *sh, const char *serviceName, bool connected, void *ctx);
	static bool connectionManagerGetStatusCallback(LSHandle* sh, LSMessage* message, void* ctxt);

	bool connectionManagerConnectCallback(LSHandle *sh, const char *serviceName, bool connected);
	bool connectionManagerGetStatusCallback(LSHandle *sh, LSMessage *message);

private:

	bool m_isInternetConnectionAvailable;
	void* m_cookie;
};

#endif /* NETWORKCONNECTIONLISTENER_H */
