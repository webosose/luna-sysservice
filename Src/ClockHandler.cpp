// Copyright (c) 2013-2018 LG Electronics, Inc.
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
 *  @file ClockHandler.cpp
 */

#include <luna-service2/lunaservice.h>
#include <sys/time.h>
#include <pbnjson.hpp>
#include "Logging.h"
#include "JSONUtils.h"

#include "ClockHandler.h"
#include "TimePrefsHandler.h"

#define SCHEMA_TIMESTAMP { \
					"type": "object", \
					"properties": { \
						"source": { "type": "string" }, \
						"sec": { "type": "integer" }, \
						"nsec": { "type": "integer" } \
					}, \
					"required": [ "source", "sec", "nsec" ], \
					"additionalProperties": false \
				}

namespace {
	LSMethod s_methods[]  = {
		{ "getTime", &ClockHandler::cbGetTime, LUNA_METHOD_FLAG_DEPRECATED},
		{ "setTime", &ClockHandler::cbSetTime, LUNA_METHOD_FLAG_DEPRECATED},
		{ 0, 0 },
	};

	// schema for /clock/setTime
	pbnjson::JSchemaFragment schemaSetTime(
	JSON(
		{
			"oneOf": [
				{
					"type":"object",
					"properties": {
						"source": { "type": "string", "default": "manual" },
						"utc": { "type": "integer" },
						"available": { "type": "boolean", "default": true },
						"timestamp": SCHEMA_TIMESTAMP,
						"$activity": { "type": "object", "optional": true }
					},
					"additionalProperties": false,
					"required": ["utc"]
				},
				{
					"type":"object",
					"properties": {
						"source": { "type": "string", "default": "manual" },
						"available": { "enum": [false] },
						"$activity": { "type": "object", "optional": true }
					},
					"additionalProperties": false,
					"required": ["available"]
				}
			]
		}
	));
} // anonymous namespace

const std::string ClockHandler::manual = "manual";
const std::string ClockHandler::micom = "micom";
const std::string ClockHandler::system = "system";
const time_t ClockHandler::invalidTime = (time_t)-1;
const time_t ClockHandler::invalidOffset = (time_t)LONG_MIN;

ClockHandler::ClockHandler() :
	m_manualOverride( false )
{
	// we always have manual time-source
	// assume priority 0 (the lowest non-negative)
	setup(manual, 0);
}

bool ClockHandler::setServiceHandle(LSHandle* serviceHandle)
{
	LSError lsError;
	LSErrorInit(&lsError);
	bool result = LSRegisterCategory(serviceHandle, "/clock",
												 s_methods, NULL, NULL, &lsError );
	if (!result) {
		PmLogError( sysServiceLogContext(), "CLOCK_REGISTER_FAIL", 1,
					PMLOGKS("MESSAGE", lsError.message),
					"Failed to register clock handler methods" );
		LSErrorFree(&lsError);
		return false;
	}
	result = LSCategorySetData(serviceHandle,  "/clock", this, &lsError);
	if (!result) {
		PmLogWarning( sysServiceLogContext(), "CLOCK_SET_DATA_FAIL", 1,
					PMLOGKS("MESSAGE", lsError.message),
					"Failed to set user data for the clock category" );
		LSErrorFree(&lsError);
		return false;
	}
	return true;
}

void ClockHandler::adjust(time_t offset)
{
	for (ClocksMap::iterator it = m_clocks.begin();
		 it != m_clocks.end(); ++it)
	{
		if (it->second.systemOffset == invalidOffset) continue;
		it->second.systemOffset -= offset; // maintain absolute time presented in diff from current one
		if (it->second.lastUpdate != invalidTime)
		{
			it->second.lastUpdate += offset; // maintain same distance from current time
		}
	}
}
void ClockHandler::manualOverride(bool enabled)
{
	if (m_manualOverride == enabled)
	{
		return; // nothing to change
	}

	m_manualOverride = enabled;

	if (!enabled)
	{
		// re-send clock changes again if switched to auto
		for (ClocksMap::const_iterator it = m_clocks.begin();
			 it != m_clocks.end(); ++it)
		{
			// skip those for which there was no update was called
			// even if they have initial offset set
			if (it->second.lastUpdate == invalidTime) continue;

			const Clock &clock = it->second;

			assert( clock.lastUpdate == invalidTime ||
					clock.systemOffset != invalidOffset ); // invariant of Clock

			PmLogDebug(sysServiceLogContext(),
				"Re-sending %s with %ld offset and %ld last update mark",
				it->first.c_str(), clock.systemOffset, clock.lastUpdate
			);
			clockChanged.fire(it->first, clock.priority, clock.systemOffset, clock.lastUpdate);
		}
	}
}

void ClockHandler::setup(const std::string &clockTag, int priority, time_t offset /* = invalidOffset */)
{
	ClocksMap::iterator it = m_clocks.find(clockTag);
	if (it != m_clocks.end())
	{
		PmLogWarning( sysServiceLogContext(), "CLOCK_SETUP_OVERRIDE", 3,
					  PMLOGKS("CLOCK_TAG", clockTag.c_str()),
					  PMLOGKFV("PRIORITY", "%d", priority),
					  PMLOGKFV("OFFSET", "%ld", offset),
					  "Trying to register already existing clock (overriding old params)" );

		it->second.priority = priority;
		if (offset != invalidOffset)
		{
			it->second.systemOffset = offset;
			// That's a good question what time to set for lastUpdate.
			// Follow rule that if we specified offset than we want it to be
			// considered so set it to current time.
			it->second.lastUpdate = time(0);
		}
	}
	else
	{
		m_clocks.insert(ClocksMap::value_type(clockTag, (Clock){ priority, offset, invalidTime }));
	}

	PmLogDebug(sysServiceLogContext(), "Registered clock %s with priority %d", clockTag.c_str(), priority);
}

bool ClockHandler::compensateSuspendedTime(time_t offset, const std::string &clockTag, time_t timeStamp)
{
	PmLogInfo(sysServiceLogContext(), "COMPENSATE_SUSPENDED_TIME", 2,
		PMLOGKS("SOURCE", clockTag.c_str()),
		PMLOGKFV("SYSTEM_OFFSET", "%ld", offset),
		"ClockHandler::compensateSuspendedTime() with time-stamp %ld",
		timeStamp
	);

	ClocksMap::iterator it = m_clocks.find(clockTag);
	if (it == m_clocks.end())
	{
		PmLogWarning( sysServiceLogContext(), "WRONG_CLOCK_UPDATE", 2,
					  PMLOGKFV("OFFSET", "%ld", offset),
					  PMLOGKS("CLOCK_TAG", clockTag.c_str()),
					  "Trying to update clock that is not registered" );
		return false;
	}

	time_t prevTimeStamp = it->second.lastUpdate;
	if (timeStamp == invalidTime)
	{
		timeStamp = time(0);
	}
	else if (prevTimeStamp != invalidTime && prevTimeStamp > timeStamp)
	{
		PmLogInfo( sysServiceLogContext(), "CLOCK_UPDATE_OUTDATED", 2,
				   PMLOGKS("SOURCE", clockTag.c_str()),
				   PMLOGKFV("SYSTEM_OFFSET", "%ld", offset),
				   "ClockHandler::compensateSuspendedTime() silently ignores updates with outdated time-stamp %ld < %ld",
				   timeStamp, it->second.lastUpdate );
		return false;
	}

	Clock &clock = it->second;
	clock.lastUpdate = timeStamp;
	clock.systemOffset += offset;

	return true;
}

void ClockHandler::compensateSuspendedTimeToClocks(time_t offset, time_t timestamp)
{
	for (ClocksMap::iterator it = m_clocks.begin();
		 it != m_clocks.end(); ++it)
	{
		if(it->first.compare("manual") == 0) continue;
		if(it->first.compare("micom") == 0) continue;
		compensateSuspendedTime(offset, it->first, timestamp);
	}
}

bool ClockHandler::update(time_t offset, const std::string &clockTag /* = manual */, time_t timeStamp /* = invalidTime */)
{
	PmLogInfo(sysServiceLogContext(), "CLOCK_UPDATE", 2,
		PMLOGKS("SOURCE", clockTag.c_str()),
		PMLOGKFV("SYSTEM_OFFSET", "%ld", offset),
		"ClockHandler::update() with time-stamp %ld",
		timeStamp
	);

	ClocksMap::iterator it = m_clocks.find(clockTag);
	if (it == m_clocks.end())
	{
		PmLogWarning( sysServiceLogContext(), "WRONG_CLOCK_UPDATE", 2,
					  PMLOGKFV("OFFSET", "%ld", offset),
					  PMLOGKS("CLOCK_TAG", clockTag.c_str()),
					  "Trying to update clock that is not registered" );
		return false;
	}

	time_t prevTimeStamp = it->second.lastUpdate;
	if (timeStamp == invalidTime)
	{
		timeStamp = time(0);
	}
	else if (prevTimeStamp != invalidTime && prevTimeStamp > timeStamp)
	{
		PmLogInfo( sysServiceLogContext(), "CLOCK_UPDATE_OUTDATED", 2,
				   PMLOGKS("SOURCE", clockTag.c_str()),
				   PMLOGKFV("SYSTEM_OFFSET", "%ld", offset),
				   "ClockHandler::update() silently ignores updates with outdated time-stamp %ld < %ld",
				   timeStamp, it->second.lastUpdate );
		return true;
	}

	Clock &clock = it->second;
	clock.lastUpdate = timeStamp;
	clock.systemOffset = offset;

	clockChanged.fire( it->first, clock.priority, offset, clock.lastUpdate );

	return true;
}

bool ClockHandler::handleNotAvailableSource(std::string source)
{
	ClocksMap::iterator it = m_clocks.find(source);
	if (it == m_clocks.end())
	{
		PmLogWarning( sysServiceLogContext(), "WRONG_SOURCE", 1,
			PMLOGKS("SOURCE", source.c_str()),
			"handle not available source" );
			return false;
	}

	notAvailableSourceHandled.fire(source);
	return true;
}

// service handlers
bool ClockHandler::cbSetTime(LSHandle* lshandle, LSMessage *message, void *user_data)
{
	assert( user_data );

	LSMessageJsonParser parser( message, schemaSetTime);

	if (!parser.parse(__FUNCTION__, lshandle, EValidateAndErrorAlways))
		return true;

	std::string source;
	int64_t utcInteger;
	bool available = true;

	// rely on schema validation
	(void) parser.get("source", source);
	(void) parser.get("utc", utcInteger);
	(void) parser.get("available", available);

	pbnjson::JValue timestamp = parser.get()["timestamp"];

	if(timestamp.isObject())
	{
		timespec sourceTimeStamp;
		sourceTimeStamp.tv_sec = toInteger<time_t>(timestamp["sec"]);
		sourceTimeStamp.tv_nsec = toInteger<long>(timestamp["nsec"]);

		utcInteger += ClockHandler::evaluateDelay(sourceTimeStamp);
	}

	time_t systemOffset = (time_t)utcInteger - time(0);
	PmLogInfo(sysServiceLogContext(), "SET_TIME", 4,
		PMLOGKS("SENDER", LSMessageGetSenderServiceName(message)),
		PMLOGKS("SOURCE", source.c_str()),
		PMLOGKFV("UTC_OFFSET", "%ld", systemOffset),
		PMLOGKFV("AVAILABLE", "%d", available),
		"/clock/setTime received with %s",
		parser.getPayload()
	);

	const char *reply = "{\"returnValue\":false}";
	ClockHandler &handler = *static_cast<ClockHandler*>(user_data);

	if (!available)
	{
		if (handler.handleNotAvailableSource(source))
		{
			reply = "{\"returnValue\":true}";
		}
	}
	else
	{
		if (handler.update(systemOffset, source))
		{
			reply = "{\"returnValue\":true}";
		}
	}

	LSError lsError;
	LSErrorInit(&lsError);
	if (!LSMessageReply(lshandle, message, reply, &lsError))
	{
		PmLogError( sysServiceLogContext(), "SETTIME_REPLY_FAIL", 1,
					PMLOGKS("REASON", lsError.message),
					"Failed to send reply on /clock/setTime" );
		LSErrorFree(&lsError);
		return false;
	}

	return true;
}

bool ClockHandler::cbGetTime(LSHandle* lshandle, LSMessage *message, void *user_data)
{
	assert( user_data );

	LSMessageJsonParser parser( message, STRICT_SCHEMA(
		PROPS_3(
			WITHDEFAULT(source, string, "system"),
			WITHDEFAULT(manualOverride, boolean, false),
			PROPERTY(fallback, string)
		)
	));

	if (!parser.parse(__FUNCTION__, lshandle, EValidateAndErrorAlways))
		return true;

	std::string source;
	bool manualOverride;
	// rely on schema validation
	(void) parser.get("source", source);
	(void) parser.get("manualOverride", manualOverride);

	std::string fallback;
	bool haveFallback = parser.get("fallback", fallback);

	ClockHandler &handler = *static_cast<ClockHandler*>(user_data);

	pbnjson::JValue reply;

	bool isSystem = (source == system);
	ClockHandler::ClocksMap::const_iterator it = handler.m_clocks.end();

	// override any source if manual override requested and system-wide user time selected
	if (manualOverride && handler.m_manualOverride)
	{
		it = handler.m_clocks.find(manual);
		// if manual time is registered and set to some value
		if (it != handler.m_clocks.end() && it->second.systemOffset != invalidOffset)
		{
			// override if we have user time
			source = manual;
			isSystem = false;
			haveFallback = false;
		}
		else
		{
			// override found clock for "manual"
			it = handler.m_clocks.end();
		}
	}

	if (it == handler.m_clocks.end())
	{
		// find requested clock (if not overriden)
		it = handler.m_clocks.find(source);
	}

	// fallback logic
	if ( haveFallback &&
		 (it == handler.m_clocks.end() || it->second.systemOffset == invalidOffset) &&
		 !isSystem )
	{
		// lets replace our source with fallback
		it = handler.m_clocks.find(fallback);
		source = fallback;
		isSystem = (fallback == system);
	}

	if (isSystem) // special case
	{
		reply = createJsonReply(true);
		reply.put("source", system);
		pbnjson::JValue offset = pbnjson::Object();
		offset.put("value", 0);
		offset.put("source", system);
		reply.put("offset", offset);
		reply.put("utc", (int64_t)time(0));
		reply.put("systemTimeSource", TimePrefsHandler::instance()->getSystemTimeSource());
		reply.put("timestamp", timestampJson());
	}
	else if (it == handler.m_clocks.end())
	{
		PmLogError( sysServiceLogContext(), "WRONG_CLOCK_GETTIME", 2,
					PMLOGKS("CLOCK_TAG", source.c_str()),
					PMLOGKFV("FALLBACK", "%s", haveFallback ? "true" : "false"),
					"Trying to fetch clock that is not registered" );
		reply = createJsonReply(false, 0, "Requested clock is not registered");
		reply.put("source", source);
	}
	else
	{
		if (it->second.systemOffset == invalidOffset)
		{
			reply = createJsonReply(false, 0, "No time available for that clock");
		}
		else
		{
			reply = createJsonReply(true);
			pbnjson::JValue offset = pbnjson::Object();
			offset.put("value", (int64_t)it->second.systemOffset);
			offset.put("source", system);
			reply.put("offset", offset);
			reply.put("utc", (int64_t)(time(0) + it->second.systemOffset));
			reply.put("timestamp", timestampJson());
		}
		reply.put("source", it->first);
		reply.put("priority", it->second.priority);
	}

	LSError lsError;
	LSErrorInit(&lsError);
	if (!LSMessageReply(lshandle, message, reply.stringify().c_str(), &lsError))
	{
		PmLogError( sysServiceLogContext(), "SETTIME_REPLY_FAIL", 1,
					PMLOGKS("REASON", lsError.message),
					"Failed to send reply on /clock/setTime" );
		LSErrorFree(&lsError);
		return false;
	}

	return true;
}
time_t ClockHandler::evaluateDelay(const timespec& sourceTimeStamp)
{
	if ( sourceTimeStamp.tv_sec == invalidTime )
		return 0;

	struct timespec currentTimeStamp;
	struct timeval currentTimeVal;
	struct timeval sourceTimeVal;
	struct timeval delayedTimeVal;
	struct timeval adjusted;

	if ( clock_gettime(CLOCK_MONOTONIC, &currentTimeStamp) == -1 )
		return 0;

	TIMESPEC_TO_TIMEVAL(&currentTimeVal, &currentTimeStamp);
	TIMESPEC_TO_TIMEVAL(&sourceTimeVal, &sourceTimeStamp);

	if ( timercmp(&currentTimeVal, &sourceTimeVal, <) )
	{
		PmLogInfo(sysServiceLogContext(), "TIMER_COMPARE_FAIL", 1,
					PMLOGKFV("RETURN_VALUE", "%d", 0),
					"sourceTimeVal: %ld.%ld | currentTimeVal: %ld.%ld",
					sourceTimeVal.tv_sec, sourceTimeVal.tv_usec,
					currentTimeVal.tv_sec, currentTimeVal.tv_usec);
		return 0;
	}

	timersub(&currentTimeVal, &sourceTimeVal, &delayedTimeVal);

	if ( gettimeofday(&adjusted, 0) == -1 )
		return 0;

	timeradd(&adjusted, &delayedTimeVal, &adjusted);

	/* Returns second difference if we guess realtime UTC is changed */
	time_t delay = adjusted.tv_sec - time(0);
	if ( delay ) {
		PmLogInfo(sysServiceLogContext(), "CHECK_DELAYED_TIME", 1,
						PMLOGKFV("Adjusted", "%d", delay),
						"Delay indicated: from %ld.%ld to %ld.%ld",
						sourceTimeVal.tv_sec, sourceTimeVal.tv_usec,
						currentTimeVal.tv_sec, currentTimeVal.tv_usec);
	}

	return delay;
}

pbnjson::JValue ClockHandler::timestampJson(void)
{
	pbnjson::JValue ret = pbnjson::Object();

	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	ret.put("source", "monotonic");
	ret.put("sec", toJValue(ts.tv_sec));
	ret.put("nsec", toJValue(ts.tv_nsec));

	return ret;
}
