// Copyright (c) 2010-2024 LG Electronics, Inc.
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


#include "PrefsFactory.h"

#include <pbnjson.hpp>
#include <luna-service2++/error.hpp>

#include "NetworkConnectionListener.h"
#include "Logging.h"
#include "JSONUtils.h"

using namespace pbnjson;

static NetworkConnectionListener* s_instance = 0;

NetworkConnectionListener* NetworkConnectionListener::instance()
{
	if (!s_instance)
		s_instance = new NetworkConnectionListener;

	return s_instance;
}

NetworkConnectionListener::NetworkConnectionListener()
	: m_isInternetConnectionAvailable(false), m_cookie(nullptr)
{
	registerForConnectionManager();
}

NetworkConnectionListener::~NetworkConnectionListener()
{
	unregisterFromConnectionManager();
}

void NetworkConnectionListener::shutdown()
{
	delete NetworkConnectionListener::instance();
	s_instance = 0;
}

void NetworkConnectionListener::registerForConnectionManager()
{
	LSHandle* serviceHandle = PrefsFactory::instance()->getServiceHandle();

	LSError error;
	LSErrorInit(&error);

	bool ret = LSRegisterServerStatusEx(serviceHandle, "com.webos.service.connectionmanager", connectionManagerConnectCallback
	                                   , this, &m_cookie, &error);
	if (!ret) {
		PmLogCritical(sysServiceLogContext(), "FAILED_TO_REGISTER_SERVER", 0, "Failed to register for server status: %s", error.message);
		LSErrorFree(&error);
		return;
	}
}

void NetworkConnectionListener::unregisterFromConnectionManager()
{
	if (m_cookie) {
		LSHandle* serviceHandle = PrefsFactory::instance()->getServiceHandle();

		LSError error;
		LSErrorInit(&error);

		auto ret = LSCancelServerStatus(serviceHandle, m_cookie, &error);
		if (ret) {
			m_cookie = nullptr;
		} else {
			PmLogWarning(sysServiceLogContext(), "REGISTER_FAIL", 0, "Failed to unregister from server status: %s", error.message);
			LSErrorFree(&error);
			return;
		}
	}
}

bool NetworkConnectionListener::connectionManagerConnectCallback(LSHandle *sh
                                                                , const char *serviceName
                                                                , bool connected
                                                                , void *ctx)
{
	return NetworkConnectionListener::instance()->connectionManagerConnectCallback(sh, serviceName, connected);
}

bool NetworkConnectionListener::connectionManagerGetStatusCallback(LSHandle* sh, LSMessage* message, void* ctxt)
{
	if (LSMessageIsHubErrorMessage(message)) {  // returns false if message is NULL
		PmLogWarning(sysServiceLogContext(), "ERROR_MESSAGE", 0, "The message received is an error message from the hub");
		return true;
	}
	return NetworkConnectionListener::instance()->connectionManagerGetStatusCallback(sh, message);
}

bool NetworkConnectionListener::connectionManagerConnectCallback(LSHandle *sh, const char *serviceName, bool connected)
{
	if (connected) {
		LSHandle* serviceHandle = PrefsFactory::instance()->getServiceHandle();

		LS::Error error;
		if (!LSCall(serviceHandle, "luna://com.webos.service.connectionmanager/getstatus",
		                     "{\"subscribe\":true}",
		                     connectionManagerGetStatusCallback, NULL, NULL, error))
		{
			PmLogCritical(sysServiceLogContext(), "FAILED_TO_CALL_GETSTATUS", 0, "Failed in calling luna://com.webos.service.connectionmanager/getstatus:%s", error.what());
		}
	}

	return true;
}

bool NetworkConnectionListener::connectionManagerGetStatusCallback(LSHandle *sh, LSMessage *message)
{
	// {"returnValue": boolean, "isInternetConnectionAvailable": boolean}
	LSMessageJsonParser parser(message, RELAXED_SCHEMA(
	                                       PROPS_3(PROPERTY(returnValue, boolean),
	                                               PROPERTY(subscribed, boolean),
	                                               PROPERTY(isInternetConnectionAvailable, boolean))
	                                       REQUIRED_1(returnValue)));

	if (!parser.parse(__FUNCTION__, sh, Settings::instance()->schemaValidationOption))
		return true;

	bool isInternetConnectionAvailable = parser.get()["isInternetConnectionAvailable"].asBool();

	if (m_isInternetConnectionAvailable != isInternetConnectionAvailable) {
		m_isInternetConnectionAvailable = isInternetConnectionAvailable;
		signalConnectionStateChanged.fire(isInternetConnectionAvailable);
	}

	return true;
}
