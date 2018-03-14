// Copyright (c) 2010-2018 LG Electronics, Inc.
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

#define __STDC_FORMAT_MACROS

#include <string>
#include <glib.h>

#include <pbnjson.hpp>
#include <luna-service2++/error.hpp>

#include "TimeZoneService.h"

#include "PrefsFactory.h"
#include "TimePrefsHandler.h"
#include "TzParser.h"
#include "Logging.h"
#include "JSONUtils.h"

using namespace pbnjson;

static LSMethod s_methods[]  = {
	{ "getTimeZoneRules",  TimeZoneService::cbGetTimeZoneRules, LUNA_METHOD_FLAG_DEPRECATED},
	{ "getTimeZoneFromEasData", TimeZoneService::cbGetTimeZoneFromEasData, LUNA_METHOD_FLAG_DEPRECATED},
	{ "createTimeZoneFromEasData", TimeZoneService::cbCreateTimeZoneFromEasData, LUNA_METHOD_FLAG_DEPRECATED},
	{ 0, 0 },
};

#define ManualTimeZoneStart  2013
#define ManualTimeZonePeriod 24 // up to 2037. Careful for the year 2038 problem.

static const char* mon_names[] = {
	NULL, "Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL
};

static const char* wday_names[] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", NULL
};

static const char*	execZIC = "/usr/sbin/zic";
static const char*	usrDefinedTZPath = WEBOS_INSTALL_SYSMGR_LOCALSTATEDIR "/preferences/zoneinfo";
static const char*	usrDefinedTZFilePath = WEBOS_INSTALL_SYSMGR_LOCALSTATEDIR "/preferences/user_defined_TZ.txt";

/*! \page com_palm_systemservice_timezone Service API com.webos.service.systemservice/timezone/
 *
 * Public methods:
 * - \ref com_palm_systemservice_timezone_get_time_zone_rules
 * - \ref com_palm_systemservice_timezone_get_time_zone_from_eas_data
 */

void TimeZoneService::setServiceHandle(LSHandle* serviceHandle)
{
	LS::Error error;
	if (!LSRegisterCategory(serviceHandle, "/timezone", s_methods, nullptr, nullptr, error))
	{
		qCritical() << "Failed in registering timezone handler method:" << error.what();
	}
}

/*!
\page com_palm_systemservice_timezone
\n
\section com_palm_systemservice_timezone_get_time_zone_rules getTimeZoneRules

\e Public.

com.webos.service.systemservice/timezone/getTimeZoneRules

\subsection com_palm_systemservice_timezone_get_time_zone_rules_syntax Syntax:
\code
[
	{
		"tz": string
		"years": [int array]
	}
]
\endcode

\param tz The timezone for which to get information. Required.
\param years Array of years for which to get information. If not specified, information for the current year is returned.

\subsection com_palm_systemservice_timezone_get_time_zone_rules_returns Returns:
\code
{
	"returnValue": true,
	"results": [
		{
			"tz": string,
			"year": int,
			"hasDstChange": false,
			"utcOffset": int,
			"dstOffset": int,
			"dstStart": int,
			"dstEnd": int
		}
	]
	"errorText": string
}
\endcode

\param returnValue Indicates if the call was succesful.
\param results Object array for the results, see fields below.
\param tz The timezone.
\param year The year.
\param hasDstChange True if daylight saving time is in use in this timezone.
\param utcOffset Time difference from UTC time in seconds.
\param dstOffset Time difference from UTC time in seconds during daylight saving time. -1 if daylight saving time is not used.
\param dstStart The time when daylight saving time starts during the \c year, presented in Unix time. -1 if daylight saving time is not used.
\param dstEnd The time when daylight saving time ends during the \c year, presented in Unix time. -1 if daylight saving time is not used.
\param errorText Description of the error if call was not succesful.

\subsection com_palm_systemservice_timezone_get_time_zone_rules_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/timezone/getTimeZoneRules '[ {"tz": "Europe/Helsinki", "years": [2012, 2010]} ]'
\endcode
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/timezone/getTimeZoneRules '[ {"tz": "Europe/Moscow"} ]'
\endcode

Example responses for succesful calls:
\code
{
	"returnValue": true,
	"results": [
		{
			"tz": "Europe\/Helsinki",
			"year": 2012,
			"hasDstChange": true,
			"utcOffset": 7200,
			"dstOffset": 10800,
			"dstStart": 1332637200,
			"dstEnd": 1351386000
		},
		{
			"tz": "Europe\/Helsinki",
			"year": 2010,
			"hasDstChange": true,
			"utcOffset": 7200,
			"dstOffset": 10800,
			"dstStart": 1269738000,
			"dstEnd": 1288486800
		}
	]
}
\endcode

\code
{
	"returnValue": true,
	"results": [
		{
			"tz": "Europe\/Moscow",
			"year": 2012,
			"hasDstChange": false,
			"utcOffset": 14400,
			"dstOffset": -1,
			"dstStart": -1,
			"dstEnd": -1
		}
	]
}
\endcode

Example response for a failed call:
\code
{
	"returnValue": false,
	"errorText": "Missing tz entry"
}
\endcode
*/
bool TimeZoneService::cbGetTimeZoneRules(LSHandle* lsHandle, LSMessage *message,
										 void *)
{
	TimeZoneEntryList entries;
	JValue root;
	JValue reply;

	root = JDomParser::fromString(LSMessageGetPayload(message));
	if (!root.isArray()) {
		reply = createJsonReply(false, 0, "Cannot parse json payload. Json root needs to be an array");
		goto Done;
	}

	for (const JValue entry: root.items()) {
		TimeZoneEntry tzEntry;
		
		// Mandatory (tz)
		JValue label = entry["tz"];
		if (!label.isString()) {
			reply = createJsonReply(false, 0, "Missing tz entry or entry is not a string");
			goto Done;
		}
		tzEntry.tz = label.asString();

		// Optional (years)
		label = entry["years"];
		if (label.isValid()) {
			if (!label.isArray()) {
				reply = createJsonReply(false, 0, "years entry is not array");
				goto Done;
			}

			for (const JValue year: label.items()) {
				if (!year.isNumber()) {
					reply = createJsonReply(false, 0, "entry in years array is not integer");
					goto Done;
				}

				tzEntry.years.push_back(year.asNumber<int>());
			}
		}

		if (tzEntry.years.empty()) {
			time_t utcTime = time(NULL);
			struct tm* localTime = localtime(&utcTime);
			tzEntry.years.push_back(localTime->tm_year + 1900);
		}

		entries.push_back(tzEntry);
	}	
	
	reply = TimeZoneService::instance()->getTimeZoneRules(entries);

Done:

	LS::Error error;
	(void) LSMessageReply(lsHandle, message, reply.stringify().c_str(), error);

	return true;
}

time_t TimeZoneService::getTimeZoneBaseOffset(const std::string &tzName)
{
	// Get current Timezone offset without DST
	TimeZoneEntry tzEntry;

	tzEntry.tz = tzName;
	tzEntry.years.push_back(getCurrentYear());
	TimeZoneResultList totalResult =
		(TimeZoneService::instance())->getTimeZoneRuleOne(tzEntry);

	if (totalResult.empty()) {
		return (time_t)-1;
	}

	TimeZoneResult r = totalResult.front();
	// return as seconds
	return r.utcOffset;
}

JValue TimeZoneService::getTimeZoneRules(const TimeZoneService::TimeZoneEntryList& entries)
{
	TimeZoneResultList totalResult;
	for (TimeZoneEntryList::const_iterator it = entries.begin();
		 it != entries.end(); ++it) {
		TimeZoneResultList r = getTimeZoneRuleOne(*it);
		totalResult.splice(totalResult.end(), r);
	}

	if (totalResult.empty()) {
		return createJsonReply(false, 0, "Failed to retrieve results for specified timezones");
	}

	JObject result {{"returnValue", true}};

	JArray array;
	for (TimeZoneResultList::const_iterator it = totalResult.begin();
		 it != totalResult.end(); ++it) {
		const TimeZoneResult& r = (*it);

		array.append(JObject {{"tz", r.tz}, {"year", r.year},
							  {"hasDstChange", r.hasDstChange},
							  {"utcOffset", r.utcOffset},
							  {"dstOffset", r.dstOffset},
							  {"dstStart", r.dstStart},
							  {"dstEnd", r.dstEnd}});

		// printf("Name: %s, Year: %d, hasDstChange: %d, utcOffset: %lld, "
		// 	   "dstOffset: %lld, dstStart: %lld, dstEnd: %lld\n",
		// 	   r.tz.c_str(), r.year, r.hasDstChange,
		// 	   r.utcOffset, r.dstOffset, r.dstStart, r.dstEnd);
	}
	result.put("results", array);

	return result;
}

TimeZoneService::TimeZoneResultList TimeZoneService::getTimeZoneRuleOne(const TimeZoneEntry& entry)
{
	TimeZoneResultList results;

	TzTransitionList transitionList = parseTimeZone(entry.tz.c_str());

	for (IntList::const_iterator it = entry.years.begin();
		 it != entry.years.end(); ++it) {

		int year = (*it);

		TimeZoneResult res;
		res.tz = entry.tz;
		res.year = year;
		res.hasDstChange = false;
		res.utcOffset = -1;
		res.dstOffset = -1;
		res.dstStart  = -1;
		res.dstEnd    = -1;

		// First do a scan to check if there are entries for this year
		bool hasEntriesForYear = false;
		for (TzTransitionList::const_iterator iter = transitionList.begin();
			 iter != transitionList.end(); ++iter) {

			const TzTransition& trans = (*iter);
			if (trans.year == year && false == trans.isDst) {
				hasEntriesForYear = true;
				break;
			}
		}

		if (hasEntriesForYear) {
		
			for (TzTransitionList::const_iterator iter = transitionList.begin();
				 iter != transitionList.end(); ++iter) {

				const TzTransition& trans = (*iter);
				if (trans.year != year)
					continue;

				if (trans.isDst) {
					res.hasDstChange = true;
					res.dstOffset    = trans.utcOffset;
					res.dstStart     = trans.time;
				}
				else {
					res.utcOffset    = trans.utcOffset;
					res.dstEnd       = trans.time;
				}
			}
		}
		else {
			int64_t dstUtcOffset=-1;
			// Pick the latest year which is < the specified year
			for (TzTransitionList::const_reverse_iterator iter = transitionList.rbegin();
				 iter != transitionList.rend(); ++iter) {

				const TzTransition& trans = (*iter);
				if (trans.year > year)
					continue;

				if (trans.isDst) {
					// Keep the DST UTC offset for fail safe.
					dstUtcOffset = trans.utcOffset;
					continue;
				}

				res.hasDstChange = false;
				res.dstOffset    = -1;
				res.dstStart     = -1;
				res.dstEnd       = -1;
				res.utcOffset    = trans.utcOffset;

				break;
			}
			// If not found except DST, then use it.
			if (res.utcOffset == -1)
				res.utcOffset = dstUtcOffset;
		}

		if (res.utcOffset == -1)
			continue;
					
		if (res.dstStart == -1)
			res.dstEnd = -1;

		results.push_back(res);
	}	
	
	return results;
}

time_t TimeZoneService::nextTzTransition(const std::string& zoneId) const
{
	time_t current = time(0);
	time_t next_trans = -1;

	TzTransitionList transitionList = parseTimeZone(zoneId.c_str());

	for (TzTransitionList::const_iterator iter = transitionList.begin();
			iter != transitionList.end(); ++iter)
	{
		if ( iter->time <= current )
			continue;

		PmLogInfo(sysServiceLogContext(), "TIMEZONE_TRANSITION", 5,
				PMLOGKFV("Abbr", "\"%s\"", iter->abbrName),
				PMLOGKFV("DST", "\"%s\"", iter->isDst ? "Start" : "End" ),
				PMLOGKFV("Year", "%d", iter->year),
				PMLOGKFV("Time", "%d", iter->time),
				PMLOGKFV("Offset", "%d", iter->utcOffset),
				"TimeZone offset will be changed");

		/* find next transition event from now */
		next_trans = iter->time;
		break;
	}

	return next_trans;
}

/*!
\page com_palm_systemservice_timezone
\n
\section com_palm_systemservice_timezone_get_time_zone_from_eas_data getTimeZoneFromEasData

\e Public.

com.webos.service.systemservice/timezone/getTimeZoneFromEasData

\subsection com_palm_systemservice_timezone_get_time_zone_from_eas_data_syntax Syntax:
\code
{
	"bias": integer,
	"standardDate": {   "year": integer,
						"month": integer,
						"dayOfWeek": integer,
						"day": integer,
						"hour": integer,
						"minute": integer,
						"second": integer
					},
	"standardBias": integer,
	"daylightDate": {   "year": integer,
						"month": integer,
						"dayOfWeek": integer,
						"day": integer,
						"hour": integer,
						"minute": integer,
						"second": integer
					},
	"daylightBias": integer
}
\endcode

\param bias Number of minutes that a time zone is offset from Coordinated Universal Time (UTC). Required.
\param standardDate Object containing date and time information in standard time. See fields below.
\param year Year in standard time.
\param month Month of year in standard time, 1 - 12.
\param dayOfWeek Day of the week in standard time, 0 - 6.
\param day The occurrence of the day of the week within the month in standard time, 1 - 5, where 5 indicates the final occurrence during the month if that day of the week does not occur 5 times.
\param hour Hour in standard time, 0 - 23.
\param minute Minutes in standard time 0 - 59.
\param second Seconds in standard time, 0 - 59.
\param standardBias The bias value to be used during local time translations that occur during standard time. This value is added to the value of the Bias member to form the bias used during standard time. In most time zones, the value is zero.
\param daylightDate Object containing date and time information in US daylight time. See fields below.
\param year Year in daylight time.
\param month Month of year in daylight time, 1 - 12.
\param dayOfWeek Day of the week in daylight time, 0 - 6.
\param day The occurrence of the day of the week within the month in daylight time, 1 - 5, where 5 indicates the final occurrence during the month if that day of the week does not occur 5 times.
\param hour Hour in daylight time, 0 - 23.
\param minute Minutes in daylight time 0 - 59.
\param second Seconds in daylight time, 0 - 59.
\param daylightBias The bias value to be used during local time translations that occur during daylight saving time. This value is added to the value of the Bias member to form the bias used during daylight saving time. In most time zones, the value is –60.


\subsection com_palm_systemservice_timezone_get_time_zone_from_eas_data_returns Returns:
\code
{
	"returnValue": boolean,
	"timeZone": string,
	"errorText": string
}
\endcode

\param returnValue Indicates if the call was successful.
\param timeZone The timezone matching the given parameters.
\param errorText Description of the error if call was not successful.

\subsection com_palm_systemservice_timezone_get_time_zone_from_eas_data_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/timezone/getTimeZoneFromEasData '{ "bias": -60  }'
\endcode

Example response for a successful call:
\code
{
	"returnValue": true,
	"timeZone": "Europe\/Brussels"
}
\endcode

Example response for a failed call:
\code
{
	"returnValue": false,
	"errorText": "Failed to find any timezones with specified bias value"
}
\endcode
*/
// http://msdn.microsoft.com/en-us/library/ms725481.aspx
bool TimeZoneService::cbGetTimeZoneFromEasData(LSHandle* lsHandle, LSMessage *message,
											   void *user_data)
{
	JValue reply;

	int easBias = 0;
	EasSystemTime easStandardDate;
	int easStandardBias = 0;
	EasSystemTime easDaylightDate;
	int easDaylightBias = 0;

	easStandardDate.valid = false;
	easDaylightDate.valid = false;

	// {"bias": integer, standardDate:{"year": integer, "month": integer, "dayOfWeek": integer, "day": integer, "hour": integer, "minute": integer, "second": integer}, "standardBias": integer, "daylightDate":{"year": integer, "month": integer, "dayOfWeek": integer, "day": integer, "hour": integer, "minute": integer, "second": integer}, "daylightBias": integer}
	LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_5(PROPERTY(bias, integer),
															  NAKED_OBJECT_OPTIONAL_8(standardDate, year, integer, month, integer, dayOfWeek, integer, day, integer, week, integer, hour, integer, minute, integer, second, integer),
															  PROPERTY(standardBias, integer),
															  NAKED_OBJECT_OPTIONAL_8(daylightDate, year, integer, month, integer, dayOfWeek, integer, day, integer, week, integer, hour, integer, minute, integer, second, integer),
															  PROPERTY(daylightBias, integer))
													  REQUIRED_1(bias)));

	if (!parser.parse(__FUNCTION__, lsHandle, EValidateAndErrorAlways))
		return true;

	JValue root = parser.get();

	// bias
	easBias = root["bias"].asNumber<int>();

	// standard date
	readEasDate(root["standardDate"], easStandardDate);

	// standard bias
	JValue element = root["standardBias"];
	if (element.isValid()) {
		easStandardBias = element.asNumber<int>();
	}

	// daylight date
	readEasDate(root["daylightDate"], easDaylightDate);

	// daylight bias
	element = root["daylightBias"];
	if (element.isValid()) {
		easDaylightBias = element.asNumber<int>();
	}

	// Both standard and daylight bias need to specified together,
	// otherwise both are invalid
	if (!easDaylightDate.valid)
		easStandardDate.valid = false;

	if (!easStandardDate.valid)
		easDaylightDate.valid = false;

	{
		// Get all timezones matching the current offset
		auto handler = PrefsFactory::instance()->getPrefsHandler("timeZone");
		if (!handler)
		{
			reply = createJsonReply(false, 0, "Failed to find timeZone preference");
			goto Done;
		}
		TimePrefsHandler* tzHandler = static_cast<TimePrefsHandler*>(handler.get());
		TimeZoneService* tzService = TimeZoneService::instance();

		std::list<std::string> timeZones = tzHandler->getTimeZonesForOffset(-easBias);

		if (timeZones.empty()) {
			reply = createJsonReply(false, 0, "Failed to find any timezones with specified bias value");
			goto Done;
		}

		if (!easStandardDate.valid) {
			// No additional data available for refinement. Just use the
			// first timezone entry in the list
			reply = createJsonReply();
			reply.put("timeZone", *timeZones.begin());
			goto Done;
		}
		else {
			int currentYear = getCurrentYear();

			updateEasDateDayOfMonth(easStandardDate, currentYear);
			updateEasDateDayOfMonth(easDaylightDate, currentYear);

			for (std::list<std::string>::const_iterator it = timeZones.begin();
				it != timeZones.end(); ++it) {
				TimeZoneEntry tzEntry;
				tzEntry.tz = (*it);
				tzEntry.years.push_back(currentYear);

				TimeZoneResultList tzResultList = tzService->getTimeZoneRuleOne(tzEntry);
				if (tzResultList.empty())
					continue;

				const TimeZoneResult& tzResult = tzResultList.front();
	
				printf("For timezone: %s\n", tzEntry.tz.c_str());
				printf("year: %d, utcOffset: %" PRIu64 ", dstOffset: %" PRIu64 ", dstStart: %" PRIu64 ", dstEnd: %" PRIu64 "\n",
					   tzResult.year, tzResult.utcOffset, tzResult.dstOffset,
					   tzResult.dstStart, tzResult.dstEnd);

				// Set this timezone as the current timezone, so that we can calculate when the
				// DST transitions occurs in seconds for the specified eas data
				setenv("TZ", tzEntry.tz.c_str(), 1);

				struct tm tzBrokenTime;
				tzBrokenTime.tm_sec = easStandardDate.second;
				tzBrokenTime.tm_min = easStandardDate.minute;
				tzBrokenTime.tm_hour = easStandardDate.hour;
				tzBrokenTime.tm_mday = easStandardDate.day;
				tzBrokenTime.tm_mon = easStandardDate.month - 1;
				tzBrokenTime.tm_year = currentYear - 1900;
				tzBrokenTime.tm_wday = 0;
				tzBrokenTime.tm_yday = 0;
				tzBrokenTime.tm_isdst = 1;

				time_t easStandardDateSeconds = ::mktime(&tzBrokenTime);

				tzBrokenTime.tm_sec = easDaylightDate.second;
				tzBrokenTime.tm_min = easDaylightDate.minute;
				tzBrokenTime.tm_hour = easDaylightDate.hour;
				tzBrokenTime.tm_mday = easDaylightDate.day;
				tzBrokenTime.tm_mon = easDaylightDate.month - 1;
				tzBrokenTime.tm_year = currentYear - 1900;
				tzBrokenTime.tm_wday = 0;
				tzBrokenTime.tm_yday = 0;
				tzBrokenTime.tm_isdst = 0;

				time_t easDaylightDateSeconds = ::mktime(&tzBrokenTime);

				printf("eas dstStart: %ld, dstEnd: %ld\n", easDaylightDateSeconds, easStandardDateSeconds);
				
				if (easStandardDateSeconds == tzResult.dstEnd &&
					easDaylightDateSeconds == tzResult.dstStart) {
					// We have a winner

					reply = createJsonReply();
					reply.put("timeZone", tzEntry.tz);
					goto Done;
				}
			}

			reply = createJsonReply(false, 0, "Failed to find any timezones with specified parameters");
		}
	}

Done:

	LS::Error error;
	(void)LSMessageReply(lsHandle, message, reply.stringify().c_str(), error);

	return true;
}

bool TimeZoneService::createTimeZoneFromEasData(LSHandle* lsHandle, UserTzData* ap_userTz)
{
	bool ret=true;
	LSError lsError;

	UserTzData a_userTz;
	if (ap_userTz)
		a_userTz = *ap_userTz;

	LSErrorInit(&lsError);

	auto handler = PrefsFactory::instance()->getPrefsHandler("timeZone");
	if (!handler)
		return false;
	TimePrefsHandler* tzHandler = static_cast<TimePrefsHandler*>(handler.get());

	if (false == a_userTz.easBiasValid)
	{
		a_userTz.easBias = getTimeZoneBaseOffset(tzHandler->currentTimeZoneName());
		if (a_userTz.easBias == (time_t)-1)
			return false;
	}
	a_userTz.easBias /= 60;

	// Both standard and daylight bias need to specified together,
	// otherwise both are invalid
	if (!a_userTz.daylightDateRule.valid)
		a_userTz.standardDateRule.valid = false;

	if (!a_userTz.standardDateRule.valid)
		a_userTz.daylightDateRule.valid = false;

	if(createManualTimeZone(a_userTz))
	{
		// update new TZ date on
		tzHandler->updateTimeZoneEnv();
	} else {
		return false;
	}

	if(tzHandler->currentTimeZoneName() == MANUAL_TZ_NAME)
	{
		tzHandler->postSystemTimeChange();
		tzHandler->manualTimeZoneChanged();
		tzHandler->postBroadcastEffectiveTimeChange();
	}

	return true;
}

bool TimeZoneService::cbCreateTimeZoneFromEasData(LSHandle* lsHandle, LSMessage *message,
											   void *user_data)
{
	TimeZoneService* thiz_class = static_cast<TimeZoneService*>(user_data);
	JValue reply;
	bool ret;
	LSError lsError;
	JValue root;
	JValue label;
	UserTzData userTz;

	LSErrorInit(&lsError);

	// {"bias": integer, standardDate:{"year": integer, "month": integer, "dayOfWeek": integer, "day": integer, "onLastDayOfWeekInMonth": boolean, "hour": integer,
	// "minute": integer, "second": integer},
	// "standardBias": integer, "
	//daylightDate":{"year": integer, "month": integer, "dayOfWeek": integer, "day": integer, "onLastDayOfWeekInMonth": boolean, "hour": integer, "minute": integer, "second": integer}, "daylightBias": integer}
	LSMessageJsonParser parser(message,
								STRICT_SCHEMA(PROPS_5(PROPERTY(bias, integer),
											NAKED_OBJECT_OPTIONAL_8(standardDate, year, integer, month, integer, dayOfWeek, integer, day, integer, week, integer, hour, integer, minute, integer, second, integer),
											PROPERTY(standardBias, integer),
											NAKED_OBJECT_OPTIONAL_8(daylightDate, year, integer, month, integer, dayOfWeek, integer, day, integer, week, integer, hour, integer, minute, integer, second, integer),
											PROPERTY(daylightBias, integer))));

	if (!parser.parse(__FUNCTION__, lsHandle, EValidateAndErrorAlways))
		return true;

	root = parser.get();
	if (!root.isObject()) {
		reply = createJsonReply(false, 0, "Cannot validate json payload");
		goto Done;
	}

	label = root["bias"];
	if (label.isError()) {
		reply = createJsonReply(false, 0, "bias value is wrong");
		goto Done;
	}

	if (label.isNumber())
	{
		userTz.easBias = label.asNumber<int>();
		userTz.easBiasValid = true;
	}

	// standard date
	label = root["standardDate"];
	if (label.isError()) {
		reply = createJsonReply(false, 0, "standardDate value missing or type mismatch");
		goto Done;
	}

	if (label.isObject())
	{
		readTimeZoneRule(label, userTz.standardDateRule);

		// standard bias
		label = root["standardBias"];
		if (label.isError())
		{
			reply = createJsonReply(false, 0, "standardBias value missing or type mismatch");
			goto Done;
		}
		if (label.isNumber())
			userTz.easStandardBias = label.asNumber<int>();
		else
			userTz.easStandardBias = 0;
	}

	// daylight date
	label = root["daylightDate"];
	if (label.isError()) {
		reply = createJsonReply(false, 0, "daylightDate value missing or type mismatch");
		goto Done;
	}
	if (label.isObject())
	{
		readTimeZoneRule(label, userTz.daylightDateRule);

		// daylight bias
		label = root["daylightBias"];
		if (label.isError())
		{
			reply = createJsonReply(false, 0, "daylightBias value missing or type mismatch");
			goto Done;
		}
		if (label.isNumber())
			userTz.easDaylightBias = label.asNumber<int>();
		else
			userTz.easDaylightBias = -60;
	}

	ret=thiz_class->createTimeZoneFromEasData(lsHandle, &userTz);
	if (true == ret)
	{
		reply = createJsonReply(true);
	} else {
		reply = createJsonReply(false, 0, "DST duration is too short");
		goto Done;
	}

Done:
	ret = LSMessageReply(lsHandle, message, reply.stringify().c_str(), &lsError);
	if (!ret)
		LSErrorFree(&lsError);

	return true;
}

void TimeZoneService::readEasDate(const JValue &obj, TimeZoneService::EasSystemTime& time)
{
	time.valid = false;

	if (!obj.isValid())
		return;

	JValue label = obj["year"];
	if (label.isError())
		return;
	if (label.isNumber())
		time.year = label.asNumber<int>();
	else
		time.year = getCurrentYear();

	label = obj["month"];
	if (!label.isNumber())
		return;
	time.month = label.asNumber<int>();

	label = obj["dayOfWeek"];
	if (!label.isNumber())
		return;
	time.dayOfWeek = label.asNumber<int>();

	label = obj["day"];
	if (!label.isNumber())
		return;
	time.day = label.asNumber<int>();

        label = obj["week"];
        if (!label.isNumber())
                return;
        time.week = label.asNumber<int>();

	label = obj["hour"];
	if (!label.isNumber())
		return;
	time.hour = label.asNumber<int>();

	label = obj["minute"];
	if (label.isError())
		return;
	if (label.isNumber())
		time.minute = label.asNumber<int>();
	else
		time.minute = 0;

	label = obj["second"];
	if (label.isError())
		return;
	if (label.isNumber())
		time.second = label.asNumber<int>();
	else
		time.second = 0;

	// Sanitize the input:
	if (time.month < 1 || time.month > 12)
		return;

	if (time.dayOfWeek < 0 || time.dayOfWeek > 6)
		return;

	if (time.day < 1 || time.day > 5)
		return;

        if (time.week < 1 || time.week > 5)
                return;

	if (time.hour < 0 || time.hour > 59)
		return;

	if (time.minute < 0 || time.minute > 59)
		return;

	if (time.second < 0 || time.second > 59)
		return;			
	
	time.valid = true;
}

void TimeZoneService::readTimeZoneRule(const JValue &obj, TimeZoneService::EasSystemTime& tzrule)
{
	tzrule.valid = false;

	if (!obj.isValid())
		return;

        JValue label = obj["year"];
        if (label.isError())
                return;
        if (label.isNumber()) {
                tzrule.year = label.asNumber<int>();
        } else {
                tzrule.year = getCurrentYear();
        }

	label = obj["month"];
	if (!label.isNumber())
		return;
	tzrule.month = label.asNumber<int>();

	label = obj["dayOfWeek"];
	if (label.isNumber())
		tzrule.dayOfWeek = label.asNumber<int>();


        label = obj["week"];
        if (!label.isNumber())
                return;
        tzrule.week = label.asNumber<int>();

	label = obj["hour"];
	if (!label.isNumber())
		return;
	tzrule.hour = label.asNumber<int>();

	label = obj["minute"];
	if (label.isNumber()) {
                tzrule.minute = label.asNumber<int>();
        } else {
                tzrule.minute = 0;
        }

	label = obj["second"];
	if (label.isNumber()){
		tzrule.second = label.asNumber<int>();
        } else {
                tzrule.second = 0;
	}

	// Sanitize the input:
	if (tzrule.month < 1 || tzrule.month > 12)
		return;


	if (tzrule.dayOfWeek < 0 || tzrule.dayOfWeek > 6)
		return;

        if (tzrule.week < 1 || tzrule.week > 5)
		return;

	if (tzrule.hour < 0 || tzrule.hour > 23)
		return;

	if (tzrule.minute < 0 || tzrule.minute > 59)
		return;

	if (tzrule.second < 0 || tzrule.second > 59)
		return;

	tzrule.valid = true;
}

// This function figures out the correct day of month based o
void TimeZoneService::updateEasDateDayOfMonth(TimeZoneService::EasSystemTime& time, int year)
{	
	// Beginning of this month at 1:00AM
	struct tm tzBrokenTimeFirst;
	tzBrokenTimeFirst.tm_sec = 0;
	tzBrokenTimeFirst.tm_min = 0;
	tzBrokenTimeFirst.tm_hour = 1;
	tzBrokenTimeFirst.tm_mday = 1;
	tzBrokenTimeFirst.tm_mon = time.month - 1;
	tzBrokenTimeFirst.tm_year = year - 1900;
	tzBrokenTimeFirst.tm_wday = 0;
	tzBrokenTimeFirst.tm_yday = 0;
	tzBrokenTimeFirst.tm_isdst = 0;

	// Beginning of next month at 1:00AM
	struct tm tzBrokenTimeLast;
	tzBrokenTimeLast.tm_sec = 0;
	tzBrokenTimeLast.tm_min = 0;
	tzBrokenTimeLast.tm_hour = 1;
	tzBrokenTimeLast.tm_mday = 1;
	tzBrokenTimeLast.tm_mon = time.month;
	tzBrokenTimeLast.tm_year = year - 1900;
	tzBrokenTimeLast.tm_wday = 0;
	tzBrokenTimeLast.tm_yday = 0;
	tzBrokenTimeLast.tm_isdst = 0;

	// Overflowed into next year
	if (tzBrokenTimeLast.tm_mon == 12) {
		tzBrokenTimeLast.tm_mon = 0;
		tzBrokenTimeLast.tm_year += 1;
	}

	time_t timeFirstOfMonth = ::timegm(&tzBrokenTimeFirst);
	time_t timeLastOfMonth = ::timegm(&tzBrokenTimeLast);
	// Subtract out 2 hours from 1:00 on first of next month.
	// This will give us 11:00 PM on last of this month
	timeLastOfMonth -= 2 * 60 * 60;

	// Now breakdown the time again
	gmtime_r(&timeFirstOfMonth, &tzBrokenTimeFirst);
	gmtime_r(&timeLastOfMonth, &tzBrokenTimeLast);

	// printf("Beg: year: %d, month: %d, day: %d, hour: %d, wday: %d\n",
	// 	   tzBrokenTimeFirst.tm_year + 1900, tzBrokenTimeFirst.tm_mon,
	// 	   tzBrokenTimeFirst.tm_mday, tzBrokenTimeFirst.tm_hour,
	// 	   tzBrokenTimeFirst.tm_wday);

	// printf("End: year: %d, month: %d, day: %d, hour: %d, wday: %d\n",
	// 	   tzBrokenTimeLast.tm_year + 1900, tzBrokenTimeLast.tm_mon,
	// 	   tzBrokenTimeLast.tm_mday, tzBrokenTimeLast.tm_hour,
	// 	   tzBrokenTimeLast.tm_wday);

	char days[31+7]; // max(mday)=31, max(wday)=7
	::memset(days, -1, sizeof(days));
	for (int i = tzBrokenTimeFirst.tm_mday; i <= tzBrokenTimeLast.tm_mday; i++) {
		days[i + tzBrokenTimeFirst.tm_wday - 1] = i;
	}

	int week = CLAMP(time.week, 1, 5) - 1;
	int dayOfWeek = CLAMP(time.dayOfWeek, 0, 6);

	int weekStart = 0;
	for (weekStart = 0; weekStart < 5; weekStart++) {
		if (days[weekStart * 7 + dayOfWeek] != (char)(-1))
			break;
	}

	week = weekStart + week;
	week = CLAMP(week, 0, 4);

	time.day = days[week * 7 + dayOfWeek];
	if(time.day == (char)(-1))
	{
		if(week == 0)
			time.day = days[(week+1) * 7 + dayOfWeek];
		else
			time.day = days[(week-1) * 7 + dayOfWeek];
	}

	qDebug() << "Updated DST start week: " << week
		<< " day: " << time.day
		<< " Of Year: " << year;
}

int TimeZoneService::compareEasRules(TimeZoneService::EasSystemTime& startTime,
					TimeZoneService::EasSystemTime& endTime,
					int diffBias)
{
	if(!startTime.valid || !endTime.valid)
		return false;

	struct tm tzDSTStartTime;
	memset(&tzDSTStartTime, 0, sizeof(tzDSTStartTime));
	tzDSTStartTime.tm_sec = startTime.second;
	tzDSTStartTime.tm_min = startTime.minute;
	tzDSTStartTime.tm_hour = startTime.hour;
	tzDSTStartTime.tm_mday = startTime.day;

	tzDSTStartTime.tm_mon = startTime.month;
	tzDSTStartTime.tm_year = startTime.year - 1900;
	tzDSTStartTime.tm_wday = startTime.dayOfWeek;

	tzDSTStartTime.tm_yday = 0;
	tzDSTStartTime.tm_isdst = 0;

	struct tm tzDSTEndTime;
	memset(&tzDSTEndTime, 0, sizeof(tzDSTEndTime));
	tzDSTEndTime.tm_sec = endTime.second;
	tzDSTEndTime.tm_min = endTime.minute;
	tzDSTEndTime.tm_hour = endTime.hour;
	tzDSTEndTime.tm_mday = endTime.day;

	tzDSTEndTime.tm_mon = endTime.month;
	tzDSTEndTime.tm_year = endTime.year - 1900;
	tzDSTEndTime.tm_wday = endTime.dayOfWeek;

	tzDSTEndTime.tm_yday = 0;
	tzDSTEndTime.tm_isdst = 0;

	int duration = ::timegm(&tzDSTEndTime) - ::timegm(&tzDSTStartTime);

	qDebug() << "Comapre DST duration :" << diffBias << " : " << duration;

	// works as like no DST
	if(duration == 0)
		return 0;

	// DST region is cross year.
	if(duration < 0)
		return 1;

	// error case
	if((diffBias*60) > duration)
		return -1;

	return 1;
}

bool TimeZoneService::createManualTimeZone(UserTzData& a_userTz)
{
	FILE * fpZone = NULL;

	// Set to None
	std::string tzRule = "-";

	fpZone = fopen(usrDefinedTZFilePath, "wb");

	if(a_userTz.standardDateRule.valid)
	{
		int targetYear = ManualTimeZoneStart;

		for(unsigned int i=0; i<= ManualTimeZonePeriod; i++)
		{
			updateEasDateDayOfMonth(a_userTz.daylightDateRule, targetYear);
			a_userTz.daylightDateRule.year = targetYear;
			updateEasDateDayOfMonth(a_userTz.standardDateRule, targetYear);
			a_userTz.standardDateRule.year = targetYear;

			if(i == 0)
			{
				int ret;
				ret = compareEasRules(a_userTz.daylightDateRule, a_userTz.standardDateRule,
						(a_userTz.easStandardBias - a_userTz.easDaylightBias));

				if(ret == 0)
				{
					break;
				}
				else if(ret < 0)
				{
					fclose(fpZone);
					return false;
				}

				tzRule = "UDT";
			}

			// create Rule entry
			writeTimeZoneRule(fpZone, tzRule.c_str(),
				(i == ManualTimeZonePeriod) ? "max":"only",
				a_userTz.easDaylightBias, a_userTz.daylightDateRule, true);

			writeTimeZoneRule(fpZone, tzRule.c_str(),
				(i == ManualTimeZonePeriod) ? "max":"only",
				a_userTz.easStandardBias, a_userTz.standardDateRule, false);
			targetYear += 1;
		}
	}

	// create Zone entry
	writeTimeZoneInfo(fpZone, MANUAL_TZ_NAME, tzRule.c_str(), "USR", a_userTz.easBias);

	fclose(fpZone);
	fpZone = NULL;

	const char *exec_args[] = {execZIC, "-d", usrDefinedTZPath, usrDefinedTZFilePath};
	std::string command = std::string(execZIC);

	for(unsigned int i=1; i< sizeof(exec_args)/sizeof(char*); i++)
	{
		command += " ";
		command += std::string(exec_args[i]);
	}

	if(g_mkdir_with_parents(usrDefinedTZPath, 0755) != 0)
	{
		return false;
	}

	::system(command.c_str());

	return true;
}

void TimeZoneService::writeTimeZoneRule(FILE* fp, const char* ruleName,
					const char* duration, int bias,
					const EasSystemTime& entry, bool isDST)
{
	const size_t onDaySize = 8;
	const size_t dstBiasSize = 8;
	char onDay[onDaySize];
	char dstBias[dstBiasSize];
	if(bias)
	{
		bias *= -1;
		setOffsetToTime(bias, dstBias);
	}
	else
	{
		dstBias[0] = '0';
		dstBias[1] = '\0';
	}

        if(entry.week == 5)
        {
                sprintf(onDay, "last%s", wday_names[entry.dayOfWeek]);
        }
        else
        {
                sprintf(onDay, "%s%s%d", wday_names[entry.dayOfWeek],
                                                ">=",
                                                entry.day);
        }

	//# Rule  NAME    FROM    TO      TYPE    IN      ON      AT      SAVE    LETTER
	//Rule    US      2007    max     -       Nov     Sun>=1  2:00    0       S
	fprintf(fp, "%s\t%s\t%d\t%s\t-\t%s\t%s\t%d:%02d\t%s\t%s\n",
		"Rule", ruleName,
		entry.year, duration,
		mon_names[entry.month],
		onDay,
		entry.hour, entry.minute,
		dstBias,
		isDST ? "D":"S");

}

void TimeZoneService::writeTimeZoneInfo(FILE* fp, const char* zoneName,
					const char* ruleName, const char* format,
					int bias)
{
	//# Zone  NAME            GMTOFF  RULES   FORMAT  [UNTIL]
	//Zone    EST              -5:00  -       EST
	char time_bias[8];
	setOffsetToTime(bias, time_bias);

	fprintf(fp, "%s\t%s\t%s\t%s\t%s\n",
		"Zone", zoneName,
		time_bias, ruleName, format);

}

int TimeZoneService::getCurrentYear()
{
	time_t utcTime = time(NULL);
	struct tm* localTime = localtime(&utcTime);
	return (localTime->tm_year + 1900);
}

void TimeZoneService::setOffsetToTime(int offset, char *result)
{
	int hour = offset/60;
	int minute = offset%60;

	if(minute <0) minute *= -1;

	snprintf(result, 6, "%d:%02d", hour, minute);
}

