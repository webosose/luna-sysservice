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

#ifndef TIMEZONESERVICE_H
#define TIMEZONESERVICE_H

#include <list>
#include <cstdint>

#include <pbnjson.hpp>

#include "Singleton.h"

#define MANUAL_TZ_NAME "Etc/Manual"

class TimeZoneService : public Singleton<TimeZoneService>
{
	friend class Singleton<TimeZoneService>;

public:
	struct UserTzData;
	void setServiceHandle(LSHandle* serviceHandle);

	time_t nextTzTransition(const std::string& zoneId) const;

	static bool cbGetTimeZoneRules(LSHandle* lshandle, LSMessage *message,
								   void *user_data);
	static bool cbGetTimeZoneFromEasData(LSHandle* lshandle, LSMessage *message,
										 void *user_data);
	static bool cbCreateTimeZoneFromEasData(LSHandle* lshandle, LSMessage *message,
										 void *user_data);
	static time_t getTimeZoneBaseOffset(const std::string &tzName);
	bool createTimeZoneFromEasData(LSHandle* lsHandle, UserTzData* a_userTz = NULL);

	struct EasSystemTime {
		bool valid = false;
		int  year = -1;
		int  month = -1;
		int  dayOfWeek = -1;
		int  day = -1;
		int  week;
		bool onLastDayOfWeekInMonth = false;
		int  hour = -1;
		int  minute = -1;
		int  second = 0;
	};

	struct UserTzData {
		UserTzData()
			: easBias(0), easBiasValid(false), easStandardBias(0), easDaylightBias(0)
		{
			standardDateRule.valid = false;
			daylightDateRule.valid = false;
		}

		int easBias;
		bool easBiasValid;
		EasSystemTime standardDateRule;
		int easStandardBias;
		EasSystemTime daylightDateRule;
		int easDaylightBias;
	};

private:

	typedef std::list<int> IntList;

	struct TimeZoneEntry {
		std::string tz;
		IntList years;
	};

	struct TimeZoneResult {
		std::string tz;
		int year;
		bool hasDstChange;
		int64_t utcOffset;
		int64_t dstOffset;
		int64_t dstStart;
		int64_t dstEnd;
	};

	typedef std::list<TimeZoneEntry> TimeZoneEntryList;
	typedef std::list<TimeZoneResult> TimeZoneResultList;

private:
	TimeZoneService() = default;

	pbnjson::JValue getTimeZoneRules(const TimeZoneEntryList& entries);
	TimeZoneResultList getTimeZoneRuleOne(const TimeZoneEntry& entry);
	static void readEasDate(const pbnjson::JValue &obj, EasSystemTime& time);
	static void readTimeZoneRule(const pbnjson::JValue &obj, EasSystemTime& time);
	static void updateEasDateDayOfMonth(EasSystemTime& time, int year);

	static int compareEasRules(EasSystemTime& startTime,
					EasSystemTime& endTime,
					int diffBias);

	static bool createManualTimeZone(UserTzData& a_userTz);

	static void writeTimeZoneRule(FILE* fp, const char* ruleName,
					const char* duration, int bias,
					const EasSystemTime& entry, bool isDST);
	static void writeTimeZoneInfo(FILE* fp, const char* zoneName,
					const char* ruleName, const char* format,
					int bias);
	static int getCurrentYear();
	static void setOffsetToTime(int offset, char *result);
};	

#endif /* TIMEZONESERVICE_H */
