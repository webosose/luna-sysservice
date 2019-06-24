// Copyright (c) 2013-2019 LG Electronics, Inc.
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

/**
 *  @file NTPClock.cpp
 */

#include "Logging.h"

#include "PrefsDb.h"
#include "TimePrefsHandler.h"
#include "ClockHandler.h"
#include "NTPClock.h"

#include <luna-service2++/error.hpp>

using namespace pbnjson;

void NTPClock::postNTP(time_t offset)
{
	PmLogDebug(sysServiceLogContext(), "post NTP offset %ld", offset);

	// send replies if any request waits for some
	if (!requestMessages.empty())
	{
		JObject reply = {{"subscribed", false},  //no subscriptions on this; make that explicit!
						 {"returnValue", true},
						 {"utc", static_cast<int64_t>(time(0) + offset)}};

		PmLogDebug(sysServiceLogContext(), "NTP reply: %s", reply.stringify().c_str());

		for (auto& request: requestMessages)
		{
			PmLogDebug(sysServiceLogContext(), "post response on %p", request.get());

			LS::Error error;
			if (!LSMessageRespond(request.get(), reply.stringify().c_str(), error.get()))
			{
				PmLogError(sysServiceLogContext(), "NTP_RESPOND_FAIL", 1,
						   PMLOGKS("REASON", error.what()),
						   "Failed to send response for NTP query call"
				);
			}
		}
		requestMessages.clear();
	}

	// post as a new value for "ntp"
	timePrefsHandler.deprecatedClockChange.fire(offset, "ntp", ClockHandler::invalidTime);
}

void NTPClock::postError()
{
	PmLogDebug(sysServiceLogContext(), "post NTP error");

	// nothing to do if no requests
	if (requestMessages.empty()) return;

	const char *reply = "{\"subscribed\":false,\"returnValue\":false,\"errorText\":\"Failed to get NTP time response\"}";

	for (auto& request : requestMessages)
	{
		PmLogDebug(sysServiceLogContext(), "post error response on %p", request.get());

		LS::Error error;
		if (!LSMessageRespond(request.get(), reply, error.get()))
		{
			PmLogError(sysServiceLogContext(), "NTP_ERROR_RESPOND_FAIL", 1,
				PMLOGKS("REASON", error.what()),
				"Failed to send response for NTP query call"
			);
		}
	}
}

bool NTPClock::requestNTP(LSMessage *message /* = NULL */)
{
	if (message)
	{
		// postpone for further NTP time post
		requestMessages.push_back(message);
	}

	if (sntpPid != -1)
	{
		// already requested update
		return true;
	}

	//try and retrieve the currently set NTP server to query
	std::string ntpServer = PrefsDb::instance()->getPref("NTPServer");
	if (ntpServer.empty()) {
		ntpServer = DEFAULT_NTP_SERVER;
	}

	std::string ntpServerTimeout;
	if (!PrefsDb::instance()->getPref("NTPServerTimeout", ntpServerTimeout))
	{
		ntpServerTimeout = "2"; // seconds
	}

	gchar *argv[] = {
		(gchar *)"sntp",
		(gchar *)"-t",
		(gchar *)ntpServerTimeout.c_str(),
		(gchar *)"-d",
		(gchar *)ntpServer.c_str(),
		0
	};

	PmLogDebug(sysServiceLogContext(),
		"%s: running sntp on %s (timeout %s)",
		__FUNCTION__,
		ntpServer.c_str(),
		ntpServerTimeout.c_str()
	);

	gchar **envp = g_get_environ();

	// override all locale related variables (LC_*)
	envp = g_environ_setenv(envp, "LC_ALL", "C", true);

	int fdOut;
	GError *error = nullptr;
	gboolean ret = g_spawn_async_with_pipes(
		/* workdir */ 0, argv, envp,
		GSpawnFlags (G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD),
		/* child_setup */ 0,
		this, &sntpPid,
		/* stdin */ 0, /* stdout */ &fdOut, /* stderr */ 0,
		&error
	);

	g_strfreev(envp);

	if (!ret)
	{
		PmLogError(sysServiceLogContext(), "SNTP_SPAWN_FAIL", 0,
			"Failed to spawn sntp"
		);
		postError();
		g_error_free (error);
		error = nullptr;
		return false;
	}

	g_child_watch_add( sntpPid, (GChildWatchFunc)cbChild, this);

	GIOChannel *chOut = g_io_channel_unix_new(fdOut);

	// Non Blocking Mode
	g_io_channel_set_flags(chOut, G_IO_FLAG_NONBLOCK, NULL);

	g_io_add_watch( chOut, GIOCondition (G_IO_IN | G_IO_HUP), (GIOFunc)cbStdout, this);

	return true;
}

// callbacks
void NTPClock::cbChild(GPid pid, gint status, NTPClock *ntpClock)
{
	// move to new state
	ntpClock->sntpPid = -1;
	g_spawn_close_pid(pid);


	std::string &sntpOutput = ntpClock->sntpOutput;

        if (status != 0 || sntpOutput.empty())
	{
		PmLogDumpDataDebug(sysServiceLogContext(),
			sntpOutput.data(), sntpOutput.size(),
			kPmLogDumpFormatDefault
		);
		ntpClock->postError();
		return;
	}

        PmLogDebug(sysServiceLogContext(), "sntpOutput: %s", sntpOutput.c_str());

        // success, maybe...parse the output

        // sntp -d us.pool.ntp.org returns below offset.
        //
        // 2000-01-05 21:10:59.821023 (+0000) +546598669.858520 +/- 364399113.290766 us.pool.ntp.org 104.156.99.226 s2 no-leap

        // TO-DO: This way of parsing can be error prone.
        // Better solution is to find a different SNTP client which can give the offset data in a unique format

        std::istringstream sStream(sntpOutput);
        std::string tok;
        std::vector<std::string> sntpStrings;
        while(std::getline(sStream, tok, ' ')){ //split the output string with space as delimter
               sntpStrings.push_back(tok);
        }

        const char *startptr = NULL;
        char *endptr = 0;
        int offsetIndex = 0;

        for (std::string keyToken : sntpStrings) {
               if (keyToken[0] == '+' || keyToken[0] == '-') {
                     if (keyToken.find('.') != std::string::npos) {
                          startptr = sntpStrings[offsetIndex].c_str();
                          break;
                     }
               }
               offsetIndex++;
        }

        if (startptr == NULL) {
             //the query failed in some way
             ntpClock->postError();
             return;
        }
        PmLogDebug(sysServiceLogContext(), "offset: %s", startptr);

	time_t offsetValue = strtol(startptr, &endptr, /* base = */ 10);
	if ( endptr == startptr ||
		 (*endptr != '\0' && strchr(" \t#.", *endptr) == NULL) )
	{
		// either empty string interpreted as a number
		// or string ends with unexpedted char
		// consider that as error
		ntpClock->postError();
	}
	else
	{
		ntpClock->postNTP(offsetValue);
	}

	sntpOutput.clear();
}

gboolean NTPClock::cbStdout(GIOChannel *channel, GIOCondition cond, NTPClock *ntpClock)
{
	if (cond == G_IO_HUP)
	{
		g_io_channel_unref(channel);
		return false;
	}

	while (true)
	{
		char buf[4096];
		gsize bytesRead;
		GIOStatus status = g_io_channel_read_chars(channel, buf, sizeof(buf), &bytesRead, 0);
		if (status == G_IO_STATUS_AGAIN) break;
		else if (status == G_IO_STATUS_EOF) return false;
		else if (status == G_IO_STATUS_ERROR)
		{
			PmLogDebug(sysServiceLogContext(), "Error during read");
			return false;
		}

		ntpClock->sntpOutput.append(buf, bytesRead);
	}
	return true;
}
