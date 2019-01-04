// Copyright (c) 2010-2019 LG Electronics, Inc.
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


#include "TimePrefsHandler.h"

#include <glib.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <memory.h>
#include <set>
#include <sys/sysinfo.h>

#if defined(HAVE_LUNA_PREFS)
#include <lunaprefs.h>
#endif

#include <pbnjson.hpp>
#include <luna-service2++/error.hpp>
#include <webosi18n.h>

#include "Mainloop.h"
#include "NetworkConnectionListener.h"
#include "PrefsDb.h"
#include "PrefsFactory.h"
#include "ClockHandler.h"
#include "Logging.h"
#include "Utils.h"
#include "JSONUtils.h"
#include "TimeZoneService.h"

using namespace pbnjson;

static const char*        s_tzFile = WEBOS_INSTALL_WEBOS_PREFIX "/ext-timezones.json";
static const char*        s_tzFilePath = WEBOS_INSTALL_SYSMGR_LOCALSTATEDIR "/preferences/localtime";
static const char*        s_zoneInfoFolder = "/usr/share/zoneinfo/";
static const int          s_sysTimeNotificationThreshold = 3000; // 5 mins
static const char*        s_logChannel = "TimePrefsHandler";
static const char*        s_factoryTimeSource = "factory";
static std::string        s_localeStr = "en-US";
static const std::string  s_file = "cppstrings.json";
static const std::string  s_resources_path = "/usr/share/localization/luna-sysservice";

#define					ORIGIN_NITZ			"nitz"
#define					HOURFORMAT_12		"HH12"
#define					HOURFORMAT_24		"HH24"

#define					NITZVALIDITY_STATE_NITZVALID					"NITZVALID"
#define					NITZVALIDITY_STATE_NITZINVALIDUSERNOTSET		"NITZINVALID_USERNOTSET"
#define					NITZVALIDITY_STATE_NITZINVALIDUSERSET			"NITZINVALID_USERSET"

#define DEBUG_TIMEPREFS 1

#define ABSV(x) ((x) < 0 ? (x*-1) : (x))
#define DIFFTIME(x,y) ((x) > (y) ? ((x)-(y)) : ((y)-(x)))

#undef LSREPORT
//#define LSREPORT(lse) g_critical( "in %s: %s => %s", __func__, (lse).func, (lse).message )
#define LSREPORT(lse) qCritical( "in %s: %s => %s", __func__, (lse).func, (lse).message )

#define TIMEOUT_INTERVAL_SEC	5

namespace {
	// when no time source used for system time
	const int lowestTimeSourcePriority = INT_MIN; // mark for overriding
} // anonymous namespace

JValue TimePrefsHandler::s_timeZonesJson {};
TimePrefsHandler * TimePrefsHandler::s_inst = NULL;

extern char *strptime (__const char *__restrict __s,
			   __const char *__restrict __fmt, struct tm *__tp);

namespace {
	bool convert(const JValue &value, time_t &timeValue)
	{
		if (!value.isNumber()) return false;

		// this check will be compiled-out due to static condition
		if (sizeof(time_t) <= sizeof(int32_t))
		{
			timeValue = value.asNumber<int32_t>();
		}
		else
		{
			timeValue = value.asNumber<int64_t>();
		}
		return true;
	}

	bool convertUnique(const char *function, const char *value, std::vector<std::string> &unique)
	{
		JsonMessageParser parser( value,
			"{\"type\":\"array\",\"items\": {\"type\":\"string\"},\"uniqueItems\":true}"
		);

		if (!parser.parse(function)) return false;

		JValue array = parser.get();

		unique.clear();
		unique.reserve(array.arraySize());
		for (ssize_t i = 0; i < array.arraySize(); ++i)
		{
			unique.push_back(array[i].asString());
		}

		return true;
	}

	// return moment of bootStart in current system time (i.e. always valid
	// even if time were changed since boot)
	time_t bootStart()
	{
		timespec ts_epoch, ts_boot;
		bool haveTimes = (clock_gettime(CLOCK_REALTIME, &ts_epoch) == 0) &&
						 (clock_gettime(CLOCK_BOOTTIME, &ts_boot) == 0);
		// Note: we use beginning of epoch as a fake boot time if no way to get
		//       one of required clocks
		return haveTimes ? (ts_epoch.tv_sec - ts_boot.tv_sec) : (time_t)0;
	}
} // anonymous namespace

static void
print_tzvars (void)
{
	qDebug ("tzname[0]='%s' tzname[1]='%s' daylight='%d' timezone='%ld'", tzname[0], tzname[1], daylight, timezone);
}


static void
set_tz(const char * tz) {
	static gchar * env = NULL;
	g_free(env);
	env = g_strdup_printf("TZ=%s", tz);
	putenv(env);
	tzset();

	qDebug("%s: tz set to %s", __func__, tz);

	print_tzvars();
}

static bool
tz_exists(const char* tz_name) {
#define ZONEINFO_PATH_PREFIX "/usr/share/zoneinfo/"
	g_return_val_if_fail(tz_name != NULL, false);
	g_return_val_if_fail(strlen(tz_name) > 1, false);
	g_return_val_if_fail(tz_name[0] != '/', false);
	g_return_val_if_fail(tz_name[0] != '.', false);
	g_return_val_if_fail(strstr(tz_name, "..") == NULL, false);

	char *path = g_build_filename(ZONEINFO_PATH_PREFIX, tz_name, NULL);
	bool ret = g_file_test(path, G_FILE_TEST_IS_REGULAR);
	g_free(path);
	return ret;
}

struct TimeZoneInfo
{
	bool operator==(const struct TimeZoneInfo& c) const {
		return (name == c.name) && (city== c.city);
	}
	std::string name;
	std::string city;
	std::string description;
	std::string country;
	std::string countryCode;
	std::string jsonStringValue;
	int   	dstSupported;
	int   	offsetToUTC;
	bool 	preferred;					//if set to true, then pick this TZ is searching by offset vs any others
	int		howManyZonesForCountry;		//how many offsets (incl. this one) does this country (based on countryCode) span? e.g. USA = 9
};

namespace {
	TimeZoneInfo buildFailsafeDefaultZone()
	{
		TimeZoneInfo tz;
		tz.name = "Etc/GMT-0";
		tz.countryCode = "";
		tz.jsonStringValue =  "{"
			"\"Country\":\"\",\"CountryCode\":\"\","
			"\"ZoneID\":\"Etc/GMT-0\",\"City\":\"\","
			"\"Description\":\"GMT\",\"offsetFromUTC\": 0,"
			"\"supportsDST\":0"
			"}";
		tz.dstSupported = 0;
		tz.offsetToUTC = 0;
		tz.preferred = false;
                tz.howManyZonesForCountry = 0;
		return tz;
	}
} // anonymous namespace

const TimeZoneInfo TimePrefsHandler::s_failsafeDefaultZone = buildFailsafeDefaultZone();

struct TZJsonHelper
{
public:
	static bool extract(const JValue& root, TimeZoneInfo* timeZoneInfo)
	{
		if (!root.isObject()) {
			return false;
		}

		reset(timeZoneInfo);

		JValue label = root["Description"];
		if (label.isString()) {
			timeZoneInfo->description = label.asString();
		}

		label = root["City"];
		if (label.isString()) {
			timeZoneInfo->city = label.asString();
		}

		label = root["Country"];
		if (label.isString()) {
			timeZoneInfo->country = label.asString();
		}

		label = root["supportDST"];
		if (label.isNumber()) {
			timeZoneInfo->dstSupported = label.asNumber<int>();
		}

		label = root["offsetFromUTC"];
		if (label.isNumber()) {
			timeZoneInfo->offsetToUTC = label.asNumber<int>();
		}

		label = root["ZoneID"];
		if (label.isString()) {
			timeZoneInfo->name = label.asString();
		}

		label = root["CountryCode"];
		if (label.isString()) {
			timeZoneInfo->countryCode = label.asString();
		}

		label = root["preferred"];
		if (label.isBoolean()) {
			timeZoneInfo->preferred = label.asBool();
		}

		return true;
	}

	static JValue pack(const TimeZoneInfo* timeZoneInfo)
	{
		JValue timeZoneObj = pbnjson::Object();

		if(!timeZoneInfo->description.empty()) {
			timeZoneObj.put("Description", timeZoneInfo->description);
		}

		if(!timeZoneInfo->city.empty()) {
			timeZoneObj.put("City", timeZoneInfo->city);
		}

		if(!timeZoneInfo->country.empty()) {
			timeZoneObj.put("Country", timeZoneInfo->country);
		}

		timeZoneObj.put("supportDST", timeZoneInfo->dstSupported);
		timeZoneObj.put("offsetFromUTC", timeZoneInfo->offsetToUTC);

		if (!timeZoneInfo->name.empty()) {
			timeZoneObj.put("ZoneID", timeZoneInfo->name);
		}

		if (!timeZoneInfo->countryCode.empty()) {
			timeZoneObj.put("CountryCode", timeZoneInfo->countryCode);
		}

		if (timeZoneInfo->preferred)
		{
			timeZoneObj.put("preferred", timeZoneInfo->preferred);
		}

		return timeZoneObj;
	}

private:
	static void reset(TimeZoneInfo* timeZoneInfo)
	{
		timeZoneInfo->name = "";
		timeZoneInfo->city = "";
		timeZoneInfo->description = "";
		timeZoneInfo->country = "";
		timeZoneInfo->countryCode = "";
		timeZoneInfo->jsonStringValue = "";
		timeZoneInfo->dstSupported = 0;
		timeZoneInfo->offsetToUTC = 0;
		timeZoneInfo->preferred = false;
		timeZoneInfo->howManyZonesForCountry = 0;
	}
};

///just a simple container
struct PreferredZones
{
	PreferredZones() : dstPref(NULL), nonDstPref(NULL), dstFallback(NULL), nonDstFallback(NULL) {}

	/* PreferredZones(const PreferredZones& c) = default; */

	/* PreferredZones& operator=(const PreferredZones& c) = default; */

	int 		   offset;
	TimeZoneInfo * dstPref;
	TimeZoneInfo * nonDstPref;
	TimeZoneInfo * dstFallback;
	TimeZoneInfo * nonDstFallback;
};

NitzParameters::NitzParameters()
: _offset(-1000)
	, _dst(0)
	, _mcc(0)
	, _mnc(0)
	, _timevalid(false)
	, _tzvalid(false)
	, _dstvalid(false)
	, _localtimeStamp(0)
{
	memset(&_timeStruct,0,sizeof(_timeStruct));
}

NitzParameters::NitzParameters(struct tm& timeStruct,int offset,int dst,int mcc,int mnc,
		bool timevalid,bool tzvalid,bool dstvalid,uint32_t remotetimeStamp)
: _offset(offset)
	, _dst(dst)
	, _mcc(mcc)
	, _mnc(mnc)
	, _timevalid(timevalid)
	, _tzvalid(tzvalid)
	, _dstvalid(dstvalid)
	, _localtimeStamp(remotetimeStamp)
{
	memcpy(&_timeStruct,&timeStruct,sizeof(timeStruct));
	_localtimeStamp = time(NULL);
}

void NitzParameters::stampTime()
{
	_localtimeStamp = time(NULL);
}

bool NitzParameters::valid(uint32_t threshold)
{
	/// not using timestamps anymore since the TIL sets the time directly

//	uint32_t lt = time(NULL);
//	uint32_t difft = (uint32_t)(DIFFTIME(_localtimeStamp,(uint32_t)time(NULL)));
//	qDebug("%s: object(%u) ? (%u) current time = diff: %u , threshold: %u",__FUNCTION__,_localtimeStamp,lt,difft,threshold);
//	if (difft > threshold)
//		return false;
	return true;
}

static JValue valuesFor_useNetworkTime( TimePrefsHandler * pTimePrefsHandler);
static bool validateFor_useNetworkTime( TimePrefsHandler * pTimePrefsHandler, const JValue &pValue);

static JValue valuesFor_useNetworkTimeZone( TimePrefsHandler * pTimePrefsHandler);
static bool validateFor_useNetworkTimeZone( TimePrefsHandler * pTimePrefsHandler, const JValue &pValue);

static JValue valuesFor_timeZone( TimePrefsHandler * pTimePrefsHandler);
static bool validateFor_timeZone( TimePrefsHandler * pTimePrefsHandler, const JValue &pValue);

static JValue valuesFor_timeFormat( TimePrefsHandler * pTimePrefsHandler);
static bool validateFor_timeFormat( TimePrefsHandler * pTimePrefsHandler, const JValue &pValue);

static JValue valuesFor_timeChangeLaunch( TimePrefsHandler * pTimePrefsHandler);
static bool validateFor_timeChangeLaunch( TimePrefsHandler * pTimePrefsHandler, const JValue &pValue);

static bool validateFor_timeDriftPeriodHr(TimePrefsHandler * pTimePrefsHandler, const JValue &pValue);

static void tzsetWorkaround(const char * newTZ) __attribute__((unused));

/*!
 * \page com_palm_systemservice_time Service API com.webos.service.systemservice/time/
 *  Public methods:
 *   - \ref com_palm_systemservice_time_set_system_time
 *   - \ref com_palm_systemservice_time_set_system_network_time
 *   - \ref com_palm_systemservice_time_get_system_time
 *   - \ref com_palm_systemservice_time_get_system_timezone_file
 *   - \ref com_palm_systemservice_time_set_time_change_launch
 *   - \ref com_palm_systemservice_time_launch_time_change_apps
 *   - \ref com_palm_systemservice_time_get_ntp_time
 *   - \ref com_palm_systemservice_time_set_time_with_ntp
 *   - \ref com_palm_systemservice_time_convert_date
 */
static LSMethod s_methods[]  = {
	{ "getSystemTime",     TimePrefsHandler::cbGetSystemTime, LUNA_METHOD_FLAG_DEPRECATED},
	{ "getSystemTimezoneFile", TimePrefsHandler::cbGetSystemTimezoneFile, LUNA_METHOD_FLAG_DEPRECATED},
	{ "getBroadcastTime", TimePrefsHandler::cbGetBroadcastTime, LUNA_METHOD_FLAG_DEPRECATED},
	{ "getEffectiveBroadcastTime", TimePrefsHandler::cbGetEffectiveBroadcastTime, LUNA_METHOD_FLAG_DEPRECATED},
	{ "setTimeChangeLaunch",	TimePrefsHandler::cbSetTimeChangeLaunch, LUNA_METHOD_FLAG_DEPRECATED},
	{ "launchTimeChangeApps", TimePrefsHandler::cbLaunchTimeChangeApps, LUNA_METHOD_FLAG_DEPRECATED},
	{ "getNTPTime",			TimePrefsHandler::cbGetNTPTime, LUNA_METHOD_FLAG_DEPRECATED},
	{ "convertDate",		TimePrefsHandler::cbConvertDate, LUNA_METHOD_FLAG_DEPRECATED},
	{ "getSystemUptime",	TimePrefsHandler::getSystemUptime, LUNA_METHOD_FLAG_DEPRECATED},
	{ "getCurrentTimeZoneByLocale",		TimePrefsHandler::cbTimeZoneByLocale, LUNA_METHOD_FLAG_DEPRECATED},
	{ "micomSynchronized",    TimePrefsHandler::cbMicomSynchronized, LUNA_METHOD_FLAG_DEPRECATED},
	{ "setSystemTime",        TimePrefsHandler::cbSetSystemTime, LUNA_METHOD_FLAG_DEPRECATED},
	{ "setSystemNetworkTime", TimePrefsHandler::cbSetSystemNetworkTime, LUNA_METHOD_FLAG_DEPRECATED},
	{ "setBroadcastTime",     TimePrefsHandler::cbSetBroadcastTime, LUNA_METHOD_FLAG_DEPRECATED},
	{ "setTimeWithNTP",       TimePrefsHandler::cbSetTimeWithNTP, LUNA_METHOD_FLAG_DEPRECATED},
	{ 0, 0 },
};

typedef JValue (*valuesForKeyFnPtr)(TimePrefsHandler * pTimePrefsHandler);
typedef bool (*validateForKeyFnPtr)(TimePrefsHandler * pTimePrefsHandler, const pbnjson::JValue & pValue);

typedef struct timePrefKey_s {
	const char * keyName;
	valuesForKeyFnPtr valuesFn;
	validateForKeyFnPtr validateFn;
} TimePrefKey;

static const TimePrefKey timePrefKeys[] = {
	{"useNetworkTime" , valuesFor_useNetworkTime , validateFor_useNetworkTime},
	{"useNetworkTimeZone" , valuesFor_useNetworkTimeZone , validateFor_useNetworkTimeZone},
	{"timeZone", valuesFor_timeZone , validateFor_timeZone},
	{"timeFormat", valuesFor_timeFormat , validateFor_timeFormat},
	{"timeChangeLaunch", valuesFor_timeChangeLaunch, validateFor_timeChangeLaunch},
	{"timeDriftPeriodHr", NULL, validateFor_timeDriftPeriodHr},
	{"nitzValidity",NULL,NULL}
};

const time_t TimePrefsHandler::m_driftPeriodDefault = (4*60*60);
const time_t TimePrefsHandler::m_driftPeriodDisabled = -1;

static inline bool isSpaceOrNull(char v) {
	return (v == 0 || isspace(v));
}

void convertString(const char* str, std::string& convertedStr)
{
	if(str)
        {
                if(Settings::instance()->useLocalizedTZ) {
			ResBundle* resBundle = NULL;
                        resBundle = new ResBundle(s_localeStr, s_file, s_resources_path);
			if (resBundle)
        		{
				convertedStr = resBundle->getLocString(str);
                		delete resBundle;
                		resBundle = NULL;
        		}
                } else {
                        convertedStr = std::string(str);
                }
        }
}

time_t TimePrefsHandler::currentStamp()
{
#if !defined(DESKTOP)
	// FIXME: CLOCK_UPTIME doesn't work
	struct timespec currTime;
	::clock_gettime(CLOCK_MONOTONIC, &currTime);
	return currTime.tv_sec;
#else
	struct timespec currTime;
	::clock_gettime(CLOCK_MONOTONIC, &currTime);
	return currTime.tv_sec;
#endif
}


TimePrefsHandler::TimePrefsHandler(LSHandle* serviceHandle)
	: PrefsHandler(serviceHandle)
	, m_cpCurrentTimeZone(nullptr)
	, m_pDefaultTimeZone(nullptr)
	, m_pManualTimeZone(nullptr)
	, m_nitzSetting(TimePrefsHandler::NITZ_TimeEnable | TimePrefsHandler::NITZ_TZEnable)
	, m_lastNitzValidity(TimePrefsHandler::NITZ_Unknown)
	, m_immNitzTimeValid(false)
	, m_immNitzZoneValid(false)
	, m_p_lastNitzParameter(nullptr)
	, m_lastNitzFlags(0)
	, m_gsource_periodic(nullptr)
	, m_gsource_periodic_id(0)
	, m_timeoutCycleCount(0)
	, m_sendWakeupSetToAlarmD(true)
	, m_lastNtpUpdate(0)
	, m_nitzTimeZoneAvailable(true)
	, m_currentTimeSourcePriority(lowestTimeSourcePriority)
	, m_nextSyncTime(0)
	, m_systemTimeSourceTag(s_factoryTimeSource)
	, m_micomTimeStamp((time_t)-1)
	, m_ntpClock(*this)
	, m_driftPeriod(m_driftPeriodDefault)
	, m_gsource_tzTrans(nullptr)
	, m_gsource_tzTrans_id(0)
	, m_nextTzTrans(-1)
	, m_micomAvailable(true)
	, m_altFactorySrcPriority(0)
	, m_altFactorySrcLastUpdate(0)
	, m_altFactorySrcSystemOffset(0)
	, m_altFactorySrcValid(false)
{
	if (!s_inst)
		s_inst=this;

	init();
}

TimePrefsHandler::~TimePrefsHandler()
{
	NetworkConnectionListener::instance()->shutdown();

	delete m_p_lastNitzParameter;
        m_p_lastNitzParameter = nullptr;
	delete m_pManualTimeZone;
        m_pManualTimeZone = nullptr;
	delete m_pDefaultTimeZone;
        m_pDefaultTimeZone = nullptr;

	TimeZoneInfoList::iterator it = m_zoneList.begin();
	for (; it != m_zoneList.end();++it)
	{
		delete *it;
                *it = nullptr;
	}
        m_zoneList.clear();
	it = m_syszoneList.begin();
	for (; it != m_syszoneList.end();++it)
	{
		delete *it;
                *it = nullptr;
	}
        m_syszoneList.clear();
}

std::list<std::string> TimePrefsHandler::keys() const
{
	return m_keyList;
}

bool TimePrefsHandler::validate(const std::string& key, const pbnjson::JValue &value)
{
	if (!value.isValid())
		return false;

	for (size_t i=0;i<sizeof(timePrefKeys)/sizeof(TimePrefKey);i++) {
		if (key == timePrefKeys[i].keyName) {
			if (timePrefKeys[i].validateFn != NULL)
				return ((*(timePrefKeys[i].validateFn))(this, value));
			else
				return false;
		}
	}

	return false;
}

void TimePrefsHandler::updateTimeZoneInfo()
{
	if((m_p_lastNitzParameter != NULL) && (true == m_p_lastNitzParameter->_tzvalid)) {
		const TimeZoneInfo* nitzTz = timeZone_ZoneFromOffset(m_p_lastNitzParameter->_offset, m_p_lastNitzParameter->_dst, m_p_lastNitzParameter->_mcc);
		bool valid_tz = isValidTimeZoneName(nitzTz->name);
		if(valid_tz)
		{
			JValue root = JDomParser::fromString(nitzTz->jsonStringValue);
			TimeZoneInfo tzInfo;
			if (TZJsonHelper::extract(root, &tzInfo)) {
                                if(Settings::instance()->useLocalizedTZ) {
				        std::unique_ptr<ResBundle> resBundle = std::unique_ptr<ResBundle>(new ResBundle(s_localeStr, s_file, s_resources_path));
				        tzInfo.city = resBundle->getLocString(tzInfo.city);
				        tzInfo.description = resBundle->getLocString(tzInfo.description);
				        tzInfo.country = resBundle->getLocString(tzInfo.country);
                                }

				JValue tzInfoJValue = pbnjson::Object();
				tzInfoJValue.put("timeZone", TZJsonHelper::pack(&tzInfo));

				std::string reply = tzInfoJValue.stringify();

				LSError error;
				LSErrorInit(&error);
				if (!(LSCall(getServiceHandle(),"luna://com.webos.service.systemservice/setPreferences", reply.c_str(), nullptr, this, nullptr, &error)))
				{
					LSErrorFree(&error);
				}
				else {
					PmLogDebug(sysServiceLogContext(), "set Network TimeZone successfull");
				}
			}
		}
	}
}

void TimePrefsHandler::switchTimeZone(bool b_recover)
{
	if (b_recover)
	{
		JValue jArgs = pbnjson::JObject();

		std::string  lastTzName =
			PrefsDb::instance()->getPref("lastTimeZone");

		if (lastTzName == "") {
			lastTzName = s_failsafeDefaultZone.name;
			PrefsDb::instance()->setPref("lastTimeZone",lastTzName.c_str());
		}

		qDebug("set TimeZone to [%s]",lastTzName.c_str());
		jArgs.put("ZoneID", lastTzName);
		valueChanged("timeZone", jArgs);
	}
	else
	{
		PrefsDb::instance()->setPref("lastTimeZone",
				m_cpCurrentTimeZone->name);
		qDebug("set TimeZone to [%s]",m_cpCurrentTimeZone->name.c_str());

		TimeZoneService::instance()->createTimeZoneFromEasData(getServiceHandle());

		JValue jArgs = JObject();
		jArgs.put("ZoneID", MANUAL_TZ_NAME);
		valueChanged("timeZone", jArgs);
	}
        // while respond if timeZone preference is set using setPreferencs directly
        // (@see. cbSetPreferences & postPrefChangeValueIsCompleteString),
        // when useNetworkTime is changed, does not notify timeZone change(manual<->auto).
        // so, it need to notify timezone change to subscribers.
        std::string timeZone = PrefsDb::instance()->getPref("timeZone");
        PrefsFactory::instance()->postPrefChange("timeZone", timeZone);
}

void TimePrefsHandler::valueChanged(const std::string& key, const JValue &value)
{
	bool bval;
	std::string strval;

	if (key == "useNetworkTime") {
		if (value.isBoolean()) {
			bval = value.asBool();
		}
		else {
			bval = true;
		}

		if(isManualTimeUsed() == !bval)
		{
			qWarning("value userNetworkTime isn't changed (ignoring)");
			return;
		}

                if(enableNetworkTimeSync(bval) == -1)
                        qWarning("valueChanged: enableNetworkTimeSync failed");

		setNITZTimeEnable(bval);

		if(Settings::instance()->switchTimezoneOnManualTime)
		{
			// manual timezone will be also set
			switchTimeZone(bval);
		}

		postBroadcastEffectiveTimeChange();

		if (bval)
		{
			//kick off an update cycle right now (* see the function for restrictions; It won't do anything in some cases)
			startBootstrapCycle(3);
		}
		else
		{
			// on switching from auto-time update manual time right away as if
			// user set it
			systemSetTime(0, ClockHandler::manual);
		}
	}
	else if (key == "useNetworkTimeZone") {
		if (value.isBoolean()) {
			bval = value.asBool();
		}
		else {
			bval = true;
		}

		if(bval){
			updateTimeZoneInfo();
		}
		setNITZTZEnable(bval);
	}
	else if (key == "timeZone") {
		//TODO: change tz
		if (value) {
			strval = TimePrefsHandler::tzNameFromJsonValue(value);
			std::string substrval = TimePrefsHandler::tzCityNameFromJsonValue(value);
			__qMessage("attempted change of timeZone to [%s:%s]",
					strval.c_str(), substrval.c_str());

			const TimeZoneInfo * new_mcTZ = timeZone_ZoneFromName(strval, substrval);
			if (new_mcTZ) {
				if (*new_mcTZ == *m_cpCurrentTimeZone) {
					qDebug("new and old timezones are the same...skipping the rest of the change procedure");
					return;
				}
				m_cpCurrentTimeZone = new_mcTZ;
			}

			if (m_cpCurrentTimeZone) {
				qDebug("%s: successfully mapped to zone [%s]", __func__, m_cpCurrentTimeZone->name.c_str());
				setTimeZone(m_cpCurrentTimeZone);
			}
			else {
				int currOffsetToUTC = offsetToUtcSecs()/60;
				//last chance to get a valid timezone given the offset

				m_cpCurrentTimeZone = timeZone_ZoneFromOffset(currOffsetToUTC);
				if (m_cpCurrentTimeZone == NULL)
				{
					qWarning() << "Couldn't pick timezone from offset" << currOffsetToUTC << "... picking a generic zone based on offset";
					//STILL NULL! pick a generic zone
					m_cpCurrentTimeZone = timeZone_GenericZoneFromOffset(currOffsetToUTC);
					if (m_cpCurrentTimeZone == NULL)
					{
						qWarning() << "Couldn't pick GENERIC timezone from offset" << currOffsetToUTC << "... last resort: go to default zone";
						//This should never happen unless the syszone list is corrupt. But if it is, pick the failsafe default
						m_cpCurrentTimeZone = &s_failsafeDefaultZone;
					}
				}
				setTimeZone(m_cpCurrentTimeZone);
			}

			transitionNITZValidState((this->getLastNITZValidity() == TimePrefsHandler::NITZ_Valid),true);

			// TODO: consider moving to systemSetTimeZone
			postSystemTimeChange();
			postBroadcastEffectiveTimeChange();
			//launch any apps that wanted to be launched when the time/zone changed
			launchAppsOnTimeChange();
			tzTransTimer();
		}
		else {
			qWarning("attempted change of timeZone but no value provided");
		}
	}
	else if (key == "timeFormat") {
		//TODO: change tformat
		if (value.isString()) {
			strval = value.asString();
			__qMessage("attempted change of timeFormat to [%s]",strval.c_str());
		}
		else {
			qWarning() << "attempted change of timeFormat but no string value provided";
		}
	}
	else if (key == "timeDriftPeriodHr") {
		if (value.isString()) {
			strval = value.asString();
			__qMessage("attempted change of timeDriftpPeriodHr to [%s]",strval.c_str());
			updateDriftPeriod(strval);
		}
		else {
			qWarning() << "attempted change of timeDriftPeriodHr but no string value provided";
		}
	}

	qWarning("valueChanged: useNetworkTime is [%s] , useNetworkTimeZone is [%s]",
			(this->isNITZTimeEnabled() ? "true" : "false"),(this->isNITZTZEnabled() ? "true" : "false"));
}

JValue TimePrefsHandler::valuesForKey(const std::string& key)
{
	JValue result;
	for (size_t i=0;i<sizeof(timePrefKeys)/sizeof(TimePrefKey);i++) {
		if ((key == timePrefKeys[i].keyName) && (timePrefKeys[i].valuesFn != NULL)) {
			result = ((*(timePrefKeys[i].valuesFn))(this));
			break;
		}
		else if ((key == timePrefKeys[i].keyName) && (timePrefKeys[i].valuesFn == NULL))
			break;
	}

	if (result.isValid()) {
		return result;
	}

	//else, a default return object is returned
	return JObject();
}

bool TimePrefsHandler::cbLocaleHandler(LSHandle* sh, LSMessage* message, void* data)
{
	do {
		const char* str = LSMessageGetPayload(message);
		if(!str) break;

		JValue root = JDomParser::fromString(str);
		if (!root.isObject()) break;

		JValue settings = root["settings"];
		if (!settings.isObject()) break;

		JValue localeInfo = settings["localeInfo"];
		if (!localeInfo.isObject()) break;

		JValue locales = localeInfo["locales"];
		if (!locales.isObject()) break;

		JValue UI = locales["UI"];
		if (!UI.isString()) break;
		s_localeStr = UI.asString();

		return true;
	} while(false);

	return false;
}

JValue TimePrefsHandler::timeZoneListAsJson()
{
	return TimePrefsHandler::s_timeZonesJson; //"copy" it!
}

JValue TimePrefsHandler::timeZoneListAsJson(const std::string& countryCode, const std::string& locale)
{
	do {
		JValue timeZones = TimePrefsHandler::s_timeZonesJson["timeZone"];
		if (!timeZones.isArray()) {
			qWarning() << "Failed to parse timeZone details";
			break;
		}

		JValue sysZones = TimePrefsHandler::s_timeZonesJson["syszones"];
		if (!sysZones.isArray()) {
			qWarning() << "Failed to parse syszones details";
			break;
		}

		JValue mmcInfoObj = TimePrefsHandler::s_timeZonesJson["mmcInfo"];
		if (!mmcInfoObj.isObject()) {
			qWarning() << "Failed to parse mmcInfo details";
			break;
		}

		std::unique_ptr<ResBundle> resBundle(nullptr);
		if (!locale.empty())
		{
			resBundle.reset(new ResBundle(locale, s_file, s_resources_path));
		}
		else
		{
			resBundle.reset(new ResBundle(s_localeStr, s_file, s_resources_path));
		}

		std::string locCountryCode("");
		JValue label;
		JValue timeZoneArray = pbnjson::Array();
		TimeZoneInfo tzInfo;
		for (const JValue key: timeZones.items()) {

			label = key["CountryCode"];
			if (!label.isString())	continue;
			locCountryCode = label.asString();

			if (countryCode.empty() || (countryCode == locCountryCode)) {
				if (TZJsonHelper::extract(key, &tzInfo)) {
                                                if(Settings::instance()->useLocalizedTZ) {
						        tzInfo.description = resBundle->getLocString(tzInfo.description);
						        tzInfo.city = resBundle->getLocString(tzInfo.city);
						        tzInfo.country = resBundle->getLocString(tzInfo.country);
                                                }
						timeZoneArray.append(TZJsonHelper::pack(&tzInfo));
				}
			}
		}

		JValue timeZonesListObj = pbnjson::Object();
		timeZonesListObj.put("timeZone", timeZoneArray);
		if (countryCode.empty()) {
			timeZonesListObj.put("syszones", sysZones);
			timeZonesListObj.put("mmcInfo", mmcInfoObj);
		}

		if (!timeZonesListObj.isNull()) {
			return timeZonesListObj;
		}
	} while(false);

	return TimePrefsHandler::s_timeZonesJson;
}

bool TimePrefsHandler::isValidTimeZoneName(const std::string& tzName)
{
	if (!TimePrefsHandler::s_timeZonesJson.isValid())
		return false;

	if(!tzName.compare(MANUAL_TZ_NAME)) return true;

	JValue label = TimePrefsHandler::s_timeZonesJson["timeZone"];
	if (!label.isArray()) {
		return false;
	}

	for (const JValue& timezone: label.items()) {

		if (!timezone.isObject()) continue;

		//parse out the TZ name
		JValue zone_id = timezone["ZoneID"];
		if (!zone_id.isString()) {
			continue;
		}

		if (tzName == zone_id.asString())
			return true;
	}

	label = TimePrefsHandler::s_timeZonesJson["syszones"];
	if (!label.isArray()) {
		return false;
	}

	for (const JValue timezone: label.items()) {

		if (!timezone.isObject()) continue;

		//parse out the TZ name
		JValue zone_id = timezone["ZoneID"];
		if (!zone_id.isString()) {
			continue;
		}

		if (tzName == zone_id.asString())
			return true;
	}

	return false;

}

static JValue valuesFor_useNetworkTime(TimePrefsHandler *)
{
	return JObject {{"useNetworkTime", JArray {true, false}}};
}

static bool validateFor_useNetworkTime(TimePrefsHandler *, const JValue &pValue)
{
	return pValue.isBoolean();
}

static JValue valuesFor_useNetworkTimeZone(TimePrefsHandler *)
{
	return JObject {{"useNetworkTimeZone", JArray {true, false}}};
}

static bool validateFor_useNetworkTimeZone(TimePrefsHandler * pTimePrefsHandler, const JValue &pValue)
{
	return pValue.isBoolean();
}

static JValue valuesFor_timeZone(TimePrefsHandler * pTimePrefsHandler)
{
	if (pTimePrefsHandler == NULL)
		return JObject();

	JValue json = pTimePrefsHandler->timeZoneListAsJson();
	if (!json.isValid())
		return JObject();

	return json;
}

static bool validateFor_timeZone(TimePrefsHandler * pTimePrefsHandler, const JValue &pValue)
{
	if (pTimePrefsHandler == NULL)
		return false;

	if (!pValue.isObject())
		return false;

	JValue label = pValue["ZoneID"];
	if (!label.isString()) {
		return false;
	}

	bool rv = pTimePrefsHandler->isValidTimeZoneName(label.asString());
	return rv; //broken out for debugging ease
}

/*
 * Scans the json timezone array (built in init()) for "default" on an entry and returns that json object as
 * a string
 */
std::string TimePrefsHandler::getDefaultTZFromJson(TimeZoneInfo * r_pZoneInfo)
{
	if (!s_timeZonesJson.isValid())
	{
		if (r_pZoneInfo)
			*r_pZoneInfo = s_failsafeDefaultZone;
		return s_failsafeDefaultZone.jsonStringValue;
	}

	JValue label = s_timeZonesJson["timeZone"];
	if (!label.isArray()) {
		qWarning() << "error on json object: it doesn't contain a timezones array";
		if (r_pZoneInfo)
			*r_pZoneInfo = s_failsafeDefaultZone;
		return s_failsafeDefaultZone.jsonStringValue;
	}

	for (const JValue timezone: label.items()) {

		//look for "default" boolean
		if (!timezone["default"].isValid())
			continue;

		//found it - I actually don't care if it's true or false...its mere existence is enough to
		//consider this a default.

		if (r_pZoneInfo)
		{
			if (TimePrefsHandler::jsonUtil_ZoneFromJson(timezone, *r_pZoneInfo) == false)
			{
				*r_pZoneInfo = s_failsafeDefaultZone;
				return (s_failsafeDefaultZone.jsonStringValue);
			}
			else
				return (r_pZoneInfo->jsonStringValue);
		}
	}

	if (r_pZoneInfo)
		*r_pZoneInfo = s_failsafeDefaultZone;
	return s_failsafeDefaultZone.jsonStringValue;
}

//static
/*
 * reads the nitzValidity key from the db and decides what the next state will be, writes that back to the db
 * returns the previous state
 *
 */
std::string TimePrefsHandler::transitionNITZValidState(bool nitzValid,bool userSetTime)
{
	std::string currentState = PrefsDb::instance()->getPref("nitzValidity");
	std::string nextState;
	if ((currentState == NITZVALIDITY_STATE_NITZVALID) || (currentState.size() == 0)) {
		if (nitzValid == false)
			nextState = NITZVALIDITY_STATE_NITZINVALIDUSERNOTSET;
		else
			nextState = NITZVALIDITY_STATE_NITZVALID;
	}
	else if (currentState == NITZVALIDITY_STATE_NITZINVALIDUSERNOTSET) {
		if (userSetTime)
			nextState = NITZVALIDITY_STATE_NITZINVALIDUSERSET;
		else if (nitzValid)
			nextState = NITZVALIDITY_STATE_NITZVALID;
		else
			nextState = NITZVALIDITY_STATE_NITZINVALIDUSERNOTSET;
	}
	else if (currentState == NITZVALIDITY_STATE_NITZINVALIDUSERSET) {
		if (nitzValid)
			nextState = NITZVALIDITY_STATE_NITZVALID;
		else
			nextState = NITZVALIDITY_STATE_NITZINVALIDUSERSET;
	}
	else {
		//confused? weird state...default to NITZVALID
		nextState = NITZVALIDITY_STATE_NITZVALID;
	}

	PrefsDb::instance()->setPref("nitzValidity",nextState);
	qDebug("transitioning [%s] -> [%s]",currentState.c_str(),nextState.c_str());

	return currentState;
}

static JValue valuesFor_timeFormat(TimePrefsHandler *)
{
	return JObject {{"timeFormat", JArray {"HH12", "HH24"}}};
}

static bool validateFor_timeFormat(TimePrefsHandler *, const JValue &pValue)
{
	if (!pValue.isString())
		return false;

	std::string val = pValue.asString();
	return ((val == "HH12") || (val == "HH24"));
}

static bool validateFor_timeDriftPeriodHr(TimePrefsHandler *, const JValue &pValue)
{
	return pValue.isNumber();
}

static JValue valuesFor_timeChangeLaunch(TimePrefsHandler *)
{
	return JObject();
}

static bool	validateFor_timeChangeLaunch(TimePrefsHandler * pTimePrefsHandler, const JValue &pValue)
{
	return false; //this is a special key which can only be set via a setTimeChangeLaunch message
}

void TimePrefsHandler::init()
{
	bool result;
	LSError lsError;
	LSErrorInit(&lsError);

	//(these will also set defaults in the db if there was nothing stored yet (e.g. first use))
	readCurrentNITZSettings();
	readCurrentTimeSettings();

   //init the keylist
	for (size_t i=0;i<sizeof(timePrefKeys)/sizeof(TimePrefKey);i++) {
		m_keyList.push_back(std::string(timePrefKeys[i].keyName));
	}

	result = LSRegisterCategory(m_serviceHandle, "/time", s_methods,
										   NULL, NULL, &lsError);
	if (!result) {
		qCritical("Failed in registering time handler method: %s", lsError.message);
		LSErrorFree(&lsError);
		return;
	}

	result = LSCategorySetData(m_serviceHandle, "/time", this, &lsError);
	if (!result) {
		qCritical() << "Failed in LSCategorySetData:" << lsError.message;
		LSErrorFree(&lsError);
		return;
	}

	s_timeZonesJson = JDomParser::fromFile(s_tzFile);
	if (s_timeZonesJson.isValid())
	{
		JValue ja = s_timeZonesJson["timeZone"];
		if (ja.isArray()) {
			qDebug("%zd timezones loaded from [%s]", ja.arraySize(), s_tzFile);
		}
		JValue jsa = s_timeZonesJson["syszones"];
		if (jsa.isArray()) {
			qDebug("%zd sys timezones loaded from [%s]", jsa.arraySize(), s_tzFile);
		}
	}
	else {
		qWarning("Can't parse timezones from the file: %s", s_tzFile);
	}

	//load the default
	m_pDefaultTimeZone = new TimeZoneInfo();
	(void)getDefaultTZFromJson(m_pDefaultTimeZone);

	m_pManualTimeZone = new TimeZoneInfo();
	setManualTimeZoneInfo();

        std::string useNetworkTime = PrefsDb::instance()->getPref("useNetworkTime");
        std::istringstream iStream(useNetworkTime);
        bool bval =  true;
        iStream >> std::boolalpha >> bval;

        if(enableNetworkTimeSync(bval) == -1)
                qWarning("init: enableNetworkTimeSync failed");


	std::string nitzValidityState = PrefsDb::instance()->getPref("nitzValidity");
	if (nitzValidityState == "") {
		PrefsDb::instance()->setPref("nitzValidity",NITZVALIDITY_STATE_NITZVALID);
		qDebug("nitzValidity default set to [%s]",NITZVALIDITY_STATE_NITZVALID);
	}

	std::string currentlySetTimeZoneJsonString= PrefsDb::instance()->getPref("timeZone");
	if (currentlySetTimeZoneJsonString == "") {
                if(nullptr != m_pDefaultTimeZone){
                        currentlySetTimeZoneJsonString = m_pDefaultTimeZone->jsonStringValue;
                }
		//set a default
		PrefsDb::instance()->setPref("timeZone",currentlySetTimeZoneJsonString);
		qDebug("timezone default set to [%s]",currentlySetTimeZoneJsonString.c_str());
	}
	qDebug("timezone default set to [%s]",currentlySetTimeZoneJsonString.c_str());

	std::string currentlySetTimeZoneName = tzNameFromJsonString(currentlySetTimeZoneJsonString);
	qDebug("timezone default set to [%s]",currentlySetTimeZoneName.c_str());

	scanTimeZoneJson();

	m_cpCurrentTimeZone = timeZone_ZoneFromName(currentlySetTimeZoneName);

	if (m_cpCurrentTimeZone) {
		qDebug("%s: successfully mapped to zone [%s]", __func__, m_cpCurrentTimeZone->name.c_str());
		setTimeZone(m_cpCurrentTimeZone);
	}
	else {
		int currOffsetToUTC = offsetToUtcSecs()/60;
		//last chance to get a valid timezone given the offset
		m_cpCurrentTimeZone = this->timeZone_ZoneFromOffset(currOffsetToUTC);
		if (m_cpCurrentTimeZone == NULL)
		{
			qWarning() << " Couldn't pick timezone from offset" << currOffsetToUTC << "... picking a generic zone based on offset";
			//STILL NULL! pick a generic zone
			m_cpCurrentTimeZone = timeZone_GenericZoneFromOffset(currOffsetToUTC);
			if (m_cpCurrentTimeZone == NULL)
			{
				qWarning() << "Couldn't pick GENERIC timezone from offset" << currOffsetToUTC << "... last resort: go to default zone";
				//This should never happen unless the syszone list is corrupt. But if it is, pick the failsafe default
				m_cpCurrentTimeZone = &s_failsafeDefaultZone;
			}
		}

		//there is no way to get here w/o m_cpCurrentTimeZone being non-NULL
		setTimeZone(m_cpCurrentTimeZone);
	}

	if (LSCall(m_serviceHandle,"luna://com.webos.service.bus/signal/registerServerStatus",
			"{\"serviceName\":\"com.webos.service.alarm\", \"subscribe\":true}",
			cbServiceStateTracker, this, NULL, &lsError) == false)
	{
		LSErrorFree(&lsError);
	}

	if (LSCall(m_serviceHandle, "luna://com.webos.service.bus/signal/registerServerStatus",
			   "{\"serviceName\":\"com.webos.service.telephony\", \"subscribe\":true}",
			   cbServiceStateTracker, this, NULL, &lsError) == false)
	{
		LSErrorFree(&lsError);
	}

	NetworkConnectionListener::instance()->signalConnectionStateChanged.
		connect(this, &TimePrefsHandler::slotNetworkConnectionStateChanged);

	//kick off an initial timeout for time setting, for cases where TIL/modem won't be there
	startBootstrapCycle();
}

/*
 * it will look for specific pref values. These were nicely set by a more general and flexible
 * implementation with valuesFor_ functions. So if those change, so must this function.
 * TODO: extend the TimePrefKey struct to include setter/validator function ptrs
 */
void TimePrefsHandler::readCurrentNITZSettings()
{
	std::string settingJsonStr = PrefsDb::instance()->getPref("useNetworkTime");
	bool val;
	qDebug("string1 is [%s]",settingJsonStr.c_str());
	JValue json = JDomParser::fromString(settingJsonStr);
	if (json.isBoolean()) {
		val = json.asBool();
	}
	else {
		//set a default
		PrefsDb::instance()->setPref("useNetworkTime","true");
		val = true;
	}

	setNITZTimeEnable(val);

	settingJsonStr = PrefsDb::instance()->getPref("useNetworkTimeZone");
	qDebug("string2 is [%s]",settingJsonStr.c_str());
	json = JDomParser::fromString(settingJsonStr);
	if (json.isBoolean()) {
		val = json.asBool();
	}
	else {
		//set a default
		PrefsDb::instance()->setPref("useNetworkTimeZone","true");
		val = true;
	}

	setNITZTZEnable(val);

}

void TimePrefsHandler::readCurrentTimeSettings()
{
	std::string settingJsonStr = PrefsDb::instance()->getPref("timeFormat");
	qDebug("string1 is [%s]",settingJsonStr.c_str());
	if (settingJsonStr == "") {
		PrefsDb::instance()->setPref("timeFormat","\"HH12\"");		//must store as a json string, or else baaaad stuff
		//TODO: fix that ...it's not very robust
	}

	std::string timeSourcesJson;
	if (!PrefsDb::instance()->getPref("timeSources", timeSourcesJson))
	{
		// default hard-coded value
		// we should get proper value from luna-init defaultPreferences.txt
		timeSourcesJson = "[\"ntp\",\"sdp\",\"nitz\",\"broadcast-adjusted\",\"broadcast\"]";
		PrefsDb::instance()->setPref("timeSources", timeSourcesJson);
		PmLogError(
			sysServiceLogContext(), "MISSING_PREF_TIMESOURCES", 1,
			PMLOGKS("HARDCODED", timeSourcesJson.c_str()),
			"No timeSources preference defined falling back to hard-coded"
		);
	}

	if (!convertUnique(__FUNCTION__, timeSourcesJson.c_str(), m_timeSources))
	{
		// converUnique will log error
		static const std::string fallback[] = { "ntp", "sdp", "nitz", "broadcast-adjusted", "broadcast" };
		m_timeSources.assign(fallback+0, fallback+(sizeof(fallback)/sizeof(fallback[0])));
	}
	else
	{
		PmLogDebug(sysServiceLogContext(), "Using next time sources order: %s",
				   timeSourcesJson.c_str());
	}

	std::string timeSyncPeriodHr;
	if ( PrefsDb::instance()->getPref("timeDriftPeriodHr", timeSyncPeriodHr) )
	{
		updateDriftPeriod(timeSyncPeriodHr);
	}
	else
	{
		PmLogDebug(sysServiceLogContext(), "Using default Sync. Period : %ld sec",
				   static_cast<long int>(getDriftPeriod()));
	}
}

std::string TimePrefsHandler::tzNameFromJsonValue(const JValue &pValue)
{
	if (!pValue.isObject())
		return std::string();

	JValue label = pValue["ZoneID"];
	if (!label.isString()) {
		return std::string();
	}

	return label.asString();
}

std::string TimePrefsHandler::tzCityNameFromJsonValue(const JValue &pValue)
{
	do {
		if (!pValue.isObject()) break;

		JValue lable = pValue["City"];

		if (!lable.isString()) break;

		return lable.asString();
	} while(false);

	return std::string();
}

std::string TimePrefsHandler::tzNameFromJsonString(const std::string& TZJson)
{
	JValue root = JDomParser::fromString(TZJson);
	if (!root.isObject()) {
		qWarning() << " Couldn't parse timezone string";
		return std::string("");
	}

	std::string zoneId;

	JValue label = root["ZoneID"];
	if (label.isString()) {
		zoneId = label.asString();
		qDebug() << "Extracted ZoneID" << zoneId.c_str();
	}

	return zoneId;
}

/**
 * Scans s_timeZonesJson for ZoneID == tzName. Returns that json object as a string, or "" if none found
 *
 *
 */

std::string TimePrefsHandler::getQualifiedTZIdFromName(const std::string& tzName)
{
	if (tzName.length() == 0)
		return std::string();

	JValue label = s_timeZonesJson["timeZone"];
	if (!label.isArray()) {
		qWarning() << "error on json object: it doesn't contain a timezones array";
		return std::string();
	}

	for (const JValue timezone: label.items()) {

		JValue zone_id = timezone["ZoneID"];
		if (!zone_id.isString())
			continue;

		if (tzName == zone_id.asString())
			return timezone.asString();
	}

	//try the sys zones
	label = s_timeZonesJson["syszones"];
	if (!label.isArray()) {
		qWarning() << "error on json object: it doesn't contain a syszones array";
		return std::string();
	}

	for (const JValue timezone: label.items()) {

		JValue zone_id = timezone["ZoneID"];
		if (!zone_id.isString())
			continue;

		if (tzName == zone_id.asString())
			return timezone.asString();
	}

	return std::string();
}

std::string TimePrefsHandler::getQualifiedTZIdFromJson(const std::string& jsonTz)
{
	if (jsonTz.length() == 0)
		return std::string();

	std::string tzName;

	JValue root = JDomParser::fromString(jsonTz);
	if (!root.isObject()) {
		return std::string();
	}

	JValue label = root["ZoneID"];
	if (label.isString()) {
		tzName = label.asString();
	}
	else {
		qWarning() << "error on json object: it doesn't contain a ZoneID key";
		return std::string();
	}

	return getQualifiedTZIdFromName(tzName);
}

//a replacement for the scanTimeZoneFile so that I only need to deal with 1 file...see init() for where the json obj is created
void TimePrefsHandler::scanTimeZoneJson()
{
	std::map<int,PreferredZones> tmpPrefZoneMap;
	std::map<int,PreferredZones>::iterator tmpPrefZoneMapIter;
	std::map<std::string,std::set<int> > tmpCountryZoneCounterMap;

	if (!s_timeZonesJson.isValid()) {
		qWarning () << "no json loaded";
		return;
	}

	JValue timezones = TimePrefsHandler::s_timeZonesJson["timeZone"];
	if (!timezones.isArray()) {
		qWarning() << "invalid json; missing timeZone array";
		return;
	}

	TimeZoneInfo tzInfo;
	// cannot work with const JValue because of stringify
	for (JValue timezone: timezones.items()) {
		TZJsonHelper::extract(timezone, &tzInfo);

		//update "counter map"
		tmpCountryZoneCounterMap[tzInfo.countryCode].insert(tzInfo.offsetToUTC);

		TimeZoneInfo* tz = new TimeZoneInfo;
		tz->offsetToUTC = tzInfo.offsetToUTC;
		tz->preferred = tzInfo.preferred;
		tz->dstSupported = tzInfo.dstSupported;
		tz->name = tzInfo.name;
		tz->city = tzInfo.city;
		tz->countryCode = tzInfo.countryCode;
		tz->jsonStringValue = timezone.stringify();

		tmpPrefZoneMapIter = tmpPrefZoneMap.find(tz->offsetToUTC);
		if (tmpPrefZoneMapIter == tmpPrefZoneMap.end()) {
			PreferredZones pz;
			pz.offset = tzInfo.offsetToUTC;
			if ((tzInfo.preferred) && (tzInfo.dstSupported))
				pz.dstPref = tz;
			else if ((tzInfo.preferred) && (!tzInfo.dstSupported))
				pz.nonDstPref = tz;
			else if (tzInfo.dstSupported)
				pz.dstFallback = tz;
			else
				pz.nonDstFallback = tz;

			tmpPrefZoneMap[tzInfo.offsetToUTC] = pz;
		}
		else {
			if ((tzInfo.preferred) && (tzInfo.dstSupported))
				(*tmpPrefZoneMapIter).second.dstPref = tz;
			else if ((tzInfo.preferred) && (!tzInfo.dstSupported))
				(*tmpPrefZoneMapIter).second.nonDstPref = tz;
			else if ((tzInfo.dstSupported) && (((*tmpPrefZoneMapIter).second.dstFallback)==NULL))
				(*tmpPrefZoneMapIter).second.dstFallback = tz;
			else if ( (!tzInfo.dstSupported) && ((*tmpPrefZoneMapIter).second.nonDstFallback)==NULL)
				(*tmpPrefZoneMapIter).second.nonDstFallback = tz;
		}

		m_zoneList.push_back(tz);

		m_offsetZoneMultiMap.insert(TimeZonePair(tzInfo.offsetToUTC, tz));

	}

	//go through the whole zone list and assign offset-per-country counter values
	for (TimeZoneInfoList::iterator it = m_zoneList.begin(); it != m_zoneList.end();++it)
	{
		(*it)->howManyZonesForCountry = tmpCountryZoneCounterMap[(*it)->countryCode].size();
	}

	//go through the temp map and assign values to the final dst and non-dst maps
	for (tmpPrefZoneMapIter = tmpPrefZoneMap.begin();tmpPrefZoneMapIter != tmpPrefZoneMap.end();++tmpPrefZoneMapIter) {
		int off_key = (*tmpPrefZoneMapIter).second.offset;

		//if there is only a dstPref, then use that for both dst and non-dst
		if ( ((*tmpPrefZoneMapIter).second.dstPref) && ((*tmpPrefZoneMapIter).second.nonDstPref == NULL))
		{
			m_preferredTimeZoneMapDST[off_key] = (*tmpPrefZoneMapIter).second.dstPref;
			m_preferredTimeZoneMapNoDST[off_key] = (*tmpPrefZoneMapIter).second.dstPref;
			continue;
		}

		if ((*tmpPrefZoneMapIter).second.dstPref)
			m_preferredTimeZoneMapDST[off_key] = (*tmpPrefZoneMapIter).second.dstPref;
		else if ((*tmpPrefZoneMapIter).second.dstFallback)
			m_preferredTimeZoneMapDST[off_key] = (*tmpPrefZoneMapIter).second.dstFallback;
		else if ((*tmpPrefZoneMapIter).second.nonDstPref)
			m_preferredTimeZoneMapDST[off_key] = (*tmpPrefZoneMapIter).second.nonDstPref;
		else
			m_preferredTimeZoneMapDST[off_key] = (*tmpPrefZoneMapIter).second.nonDstFallback;

		if ((*tmpPrefZoneMapIter).second.nonDstPref)
			m_preferredTimeZoneMapNoDST[off_key] = (*tmpPrefZoneMapIter).second.nonDstPref;
		else if ((*tmpPrefZoneMapIter).second.nonDstFallback)
			m_preferredTimeZoneMapNoDST[off_key] = (*tmpPrefZoneMapIter).second.nonDstFallback;
		else if ((*tmpPrefZoneMapIter).second.dstPref)
			m_preferredTimeZoneMapNoDST[off_key] = (*tmpPrefZoneMapIter).second.dstPref;
		else
			m_preferredTimeZoneMapNoDST[off_key] = (*tmpPrefZoneMapIter).second.dstFallback;
	}

	qDebug("found %zu timezones",m_zoneList.size());

	for (TimeZoneMapIterator it = m_preferredTimeZoneMapDST.begin();it != m_preferredTimeZoneMapDST.end();it++) {
	PMLOG_TRACE("DST-MAP: preferred zone found: [%s] , offset = %d , dstSupport = %s",
				it->second->name.c_str(),it->second->offsetToUTC,(it->second->dstSupported != 0 ? "TRUE" : "FALSE"));
	}
	for (TimeZoneMapIterator it = m_preferredTimeZoneMapNoDST.begin();it != m_preferredTimeZoneMapNoDST.end();it++) {
	PMLOG_TRACE("NON-DST-MAP: preferred zone found: [%s] , offset = %d , dstSupport = %s",
						it->second->name.c_str(),it->second->offsetToUTC,(it->second->dstSupported != 0 ? "TRUE" : "FALSE"));
	}

	//now grab the "syszones"...these are the default, generic, timezones that get set in case NITZ supplies "dstinvalid"

	timezones = s_timeZonesJson["syszones"];
	if (!timezones.isArray()) {
		qWarning() << "invalid json; missing syszones array";
		return;
	}

	for (JValue timezone: timezones.items()) {

		if (!timezone.isObject())
			continue;

		JValue label = timezone["ZoneID"];
		if (!label.isString()) {
			continue;
		}
		std::string name = label.asString();

		label = timezone["offsetFromUTC"];
		if (!label.isNumber()) {
			continue;
		}
		int offset = label.asNumber<int>();

		TimeZoneInfo* tz = new TimeZoneInfo;
		tz->offsetToUTC = offset;
		tz->preferred = false;
		tz->dstSupported = 0;
		//setTZIName(tz,name.c_str());
		tz->name = name;
		tz->jsonStringValue = timezone.stringify();

		m_syszoneList.push_back(tz);
	}

	//now grab the time zone info for known MCCs...
	// This is used to correct problems in many networks' NITZ data

	timezones = s_timeZonesJson["mccInfo"];
	if (!timezones.isArray()) {
		qWarning() << "invalid json; missing mccInfo array";
		return;
	}

	for (JValue timezone: timezones.items()) {
		if (!timezone.isObject())
			continue;

		std::string name;
		JValue label = timezone["ZoneID"];
		if (label.isString()) {
			name = label.asString();
		}

		std::string countryCode;
		label = timezone["CountryCode"];
		if (label.isString()) {
			countryCode = label.asString();
		}

		label = timezone["offsetFromUTC"];
		if (!label.isNumber()) {
			continue;
		}
		int offset = label.asNumber<int>();

		label = timezone["supportsDST"];
		if (!label.isNumber()) {
			continue;
		}
		int supportsDst = label.asNumber<int>();

		label = timezone["mcc"];
		if (!label.isNumber()) {
			continue;
		}
		int mcc = label.asNumber<int>();

		TimeZoneInfo* tz = new TimeZoneInfo;
		tz->offsetToUTC = offset;
		tz->preferred = false;
		tz->dstSupported = supportsDst;
		tz->countryCode = countryCode;
		if (name.size()) {
			//setTZIName(tz,name.c_str());
			tz->name = name;
			tz->jsonStringValue = timezone.stringify();
		}
		else {
			tz->name = "";
			tz->jsonStringValue = "";
		}
		m_mccZoneInfoMap[mcc] = tz;

	}

}

void TimePrefsHandler::setManualTimeZoneInfo()
{
        if( nullptr != m_pManualTimeZone )
        {
                m_pManualTimeZone->name = MANUAL_TZ_NAME;
                m_pManualTimeZone->countryCode = "";

                m_pManualTimeZone->jsonStringValue =  "{"
                        "\"Country\":\"\",\"CountryCode\":\"\","
                        "\"ZoneID\":\"";
                m_pManualTimeZone->jsonStringValue += MANUAL_TZ_NAME;
                m_pManualTimeZone->jsonStringValue += "\",\"City\":\"\","
                        "\"Description\":\"Manual Time Zone\",\"offsetFromUTC\":\"NA\","
                        "\"supportsDST\":\"NA\""
                        "}";

                m_pManualTimeZone->dstSupported = 0;
                m_pManualTimeZone->offsetToUTC = 0;
                m_pManualTimeZone->preferred = false;
        }

}

void TimePrefsHandler::setTimeZone(const TimeZoneInfo * pZoneInfo)
{
	if (pZoneInfo == NULL)
	{
		//failsafe default!
		pZoneInfo = &s_failsafeDefaultZone;
		qWarning() << "passed in NULL for the zone. Failsafe activated! setting failsafe-default zone: [" << pZoneInfo->name.c_str() << "]";
	}

	std::string tzFileActual = s_zoneInfoFolder + pZoneInfo->name;
	qWarning() << "Checking timezone data from " << tzFileActual.c_str() << "].";
	if (access(tzFileActual.c_str(), F_OK))
	{
		qWarning() << "Missing timezone data for [" << pZoneInfo->name.c_str() << "]."
			" Failsafe activated! setting failsafe-default zone: [" << s_failsafeDefaultZone.name.c_str() << "]";
		pZoneInfo = &s_failsafeDefaultZone;
		tzFileActual = s_zoneInfoFolder + pZoneInfo->name;
	}

	m_cpCurrentTimeZone = pZoneInfo;
	PrefsDb::instance()->setPref("timeZone",pZoneInfo->jsonStringValue);
	systemSetTimeZone(tzFileActual, *pZoneInfo);
}

void TimePrefsHandler::systemSetTimeZone(const std::string &tzFileActual, const TimeZoneInfo &zoneInfo)
{
	// Do we have a timezone file in place? remove if yes
	(void) unlink(s_tzFilePath);

	// Note that /etc/localtime should point to this file
	// s_tzFilePath ( /var/luna/preferences/localtime )
	// which is symlink to current time-zone
	// This allows to have read-only /etc/localtime
	if (symlink(tzFileActual.c_str(), s_tzFilePath))
	{
		PmLogError(sysServiceLogContext(), "CHANGETZ_FAILURE", 2,
				   PMLOGKS("TZFILE_TARGET", tzFileActual.c_str()),
				   PMLOGKS("TZFILE_LINK", s_tzFilePath),
				   "Failed to change system time-zone through making symlink");
		return;
	}
	PmLogInfo(sysServiceLogContext(), "UpdateTimeZone", 0,
			"Update Env values");
	updateTimeZoneEnv();
}

bool TimePrefsHandler::systemSetTime(time_t deltaTime, const std::string &source)
{
	struct timeval timeVal;
	timeVal.tv_sec = time(0) + deltaTime;
	timeVal.tv_usec = 0;
	qDebug("%s: settimeofday: %u",__FUNCTION__,(unsigned int)timeVal.tv_sec);

	int rc = deltaTime == 0 ? 0 : settimeofday(&timeVal, 0);
	qDebug("settimeofday %s", ( rc == 0 ? "succeeded" : "failed"));
	if (rc == 0)
	{
		if ( m_systemTimeSourceTag != source ) {
			// remember last synchronized with time
			m_systemTimeSourceTag = source;
			PrefsDb::instance()->setPref("lastSystemTimeSource", m_systemTimeSourceTag);
			// next time "micom" will come we'll use this clock tag instead
		}

		// TODO: drop direct broadcastTime adjust in favor of signal and clocks
		m_broadcastTime.adjust(deltaTime);

		systemTimeChanged.fire(deltaTime);

		// adjust micom timestamp if we have one
		if (m_micomTimeStamp != (time_t)-1)
		{
			m_micomTimeStamp += deltaTime;
		}

		postSystemTimeChange();
		if (isSystemTimeBroadcastEffective()) postBroadcastEffectiveTimeChange();
		launchAppsOnTimeChange();
		tzTransTimer();
	}

	// if we had valid NTP in our system-time we destroy it here
	m_lastNtpUpdate = 0;
	return (rc == 0);
}

void TimePrefsHandler::updateSystemTime()
{
	// right now this method is a start point for active requests to different
	// time-souces like NTP servers etc.
	if (isManualTimeUsed())
	{
		qWarning("updateSystemTime() should never be called when using manual time (ignored)");
		return;
	}

	bool isAnyRequestSent = false;
	if (isNTPAllowed())
	{
		(void) m_ntpClock.requestNTP( 0 );
		isAnyRequestSent = true;
	}
	else
	{
		PmLogDebug(sysServiceLogContext(), "Automatic NTP requests are prohibited");
	}

	if (!isAnyRequestSent)
	{
		PmLogDebug(sysServiceLogContext(),
			"No time source were requested for system time update in response to updateSystemTime()"
		);
	}
}


//static
bool TimePrefsHandler::jsonUtil_ZoneFromJson(const JValue &json, TimeZoneInfo& r_zoneInfo)
{
	if (!json.isValid())
		return false;

	JValue label = json["ZoneID"];
	if (!label.isString()) {
		return false;
	}
	std::string name = label.asString();

	label = json["offsetFromUTC"];
	if (!label.isNumber()) {
		return false;
	}
	int offset = label.asNumber<int>();

	label = json["supportsDST"];
	if (!label.isNumber()) {
		return false;
	}
	int supportsDst = label.asNumber<int>();

	bool pref = false;
	label = json["preferred"];
	if (label.isBoolean()) {
		pref = label.asBool();
	}

	std::string countryCode;
	label = json["countryCode"];
	if (label.isString())
		countryCode = label.asString();

	r_zoneInfo.offsetToUTC = offset;
	r_zoneInfo.preferred = pref;
	r_zoneInfo.dstSupported = supportsDst;
	r_zoneInfo.name = name;
	r_zoneInfo.countryCode = countryCode;
	r_zoneInfo.jsonStringValue = JValue(json).stringify(); // libpbnjson design flaw

	return true;
}

void TimePrefsHandler::postSystemTimeChange()
{
	if (!m_cpCurrentTimeZone)
		return;

	JObject json;
	attachSystemTime(json);
	json.put("timestamp", ClockHandler::timestampJson());

	//the new "sub"keys for nitz validity...
	//the new "sub"keys for nitz validity...
	if (isNITZTimeEnabled())
		json.put("NITZValidTime", m_immNitzTimeValid);
	if (isNITZTZEnabled())
		json.put("NITZValidZone", m_immNitzZoneValid);

	PrefsFactory::instance()->postPrefChangeValueIsCompleteString("getSystemTime", json.stringify());
}

void TimePrefsHandler::attachSystemTime(JValue &json)
{
	time_t utctime = time(NULL);
	struct tm localTm;

	// tzset() already called on initialization
	struct tm * pLocalTm = localtime_r(&utctime, &localTm);
	assert( pLocalTm == &localTm );
	(void) pLocalTm; // unused variable (in release)

	json.put("utc", static_cast<int64_t>(utctime));
	json.put("localtime", JObject {{"year", localTm.tm_year + 1900},
								   {"month", localTm.tm_mon + 1},
								   {"day", localTm.tm_mday},
								   {"hour", localTm.tm_hour},
								   {"minute", localTm.tm_min},
								   {"second", localTm.tm_sec}});
	json.put("offset", static_cast<int64_t>(offsetToUtcSecs() / 60));
	if (localTm.tm_isdst == 0) {
		json.put("isDST", false);
	} else if (localTm.tm_isdst > 0) {
		json.put("isDST", true);
	}

	if (currentTimeZone()) {
		json.put("timezone", currentTimeZone()->name);
		//get current time zone abbreviation
		char tzoneabbr_cstr[16];
		strftime(tzoneabbr_cstr, 16,"%Z", &localTm);
		json.put("TZ", tzoneabbr_cstr);
	}
	else {
		//default to something
		json.put("timezone", "UTC");
		json.put("TZ", "UTC");
	}

	json.put("timeZoneFile", s_tzFilePath);
	json.put("systemTimeSource", getSystemTimeSource());

	std::string nitzValidity = PrefsDb::instance()->getPref("nitzValidity");

	if (nitzValidity == NITZVALIDITY_STATE_NITZVALID)
		json.put("NITZValid", true);
	else if (nitzValidity == NITZVALIDITY_STATE_NITZINVALIDUSERNOTSET)
		json.put("NITZValid", false);
}


void TimePrefsHandler::postNitzValidityStatus()
{
	if (!m_cpCurrentTimeZone)
		return;

	std::string nitzValidity = PrefsDb::instance()->getPref("nitzValidity");

	JObject json;
	if (nitzValidity == NITZVALIDITY_STATE_NITZVALID)
		json.put("NITZValid", true);
	else if (nitzValidity == NITZVALIDITY_STATE_NITZINVALIDUSERNOTSET)
		json.put("NITZValid", false);

	//the new "sub"keys for nitz validity...
	if (isNITZTimeEnabled())
		json.put("NITZValidTime", m_immNitzTimeValid);
	if (isNITZTZEnabled())
		json.put("NITZValidZone", m_immNitzZoneValid);

	PrefsFactory::instance()->postPrefChangeValueIsCompleteString("getSystemTime", json.stringify());
}


void TimePrefsHandler::launchAppsOnTimeChange()
{
	//grab the pref and parse out the json
	std::string rawCurrentPref = PrefsDb::instance()->getPref("timeChangeLaunch");

	JValue storedJson = JDomParser::fromString(rawCurrentPref);
	if (!storedJson.isObject()) {
		//nothing to do
		return;
	}

	//get the launchList array object out of it
	JValue storedJson_listArray = storedJson["launchList"];
	if (!storedJson_listArray.isArray()) {
		//nothing to do
		return;
	}

	for (const JValue key: storedJson_listArray.items())
	{
		JValue label = key["appId"];
		if (!label.isString()) {
			continue; //something really bad happened; something was stored in the list w/o an appId!
		}
		std::string appId = label.asString();

		label = key["parameters"];
		std::string launchStr;
		if (label.isValid()) {
			launchStr = "{ \"id\":\"" + appId + "\", \"params\":" + label.stringify().c_str() + " }";
		} else {
			launchStr = "{ \"id\":\"" + appId + "\", \"params\":{} }";
		}

		LS::Error error;
		(void) LSCall(getServiceHandle(),
					  "luna://com.webos.service.applicationManager/launch",
					  launchStr.c_str(),
					  NULL,NULL,NULL, error);
	}
}

std::string TimePrefsHandler::currentTimeZoneName() const
{
	return m_cpCurrentTimeZone->name;
}

time_t TimePrefsHandler::offsetToUtcSecs() const
{
	// We retrieve current offset to UTC separately because Daylight Savings may be in
	// effect and the offset will be different than the standard one
	time_t currTime;
	struct tm lt;

	// UTC time
	currTime = time(NULL);

	// Local time
	localtime_r(&currTime, &lt);

	// Back to UTC
	time_t ltSecs = timegm(&lt);

	PmLogDebug(sysServiceLogContext(), "LOCAL %ld - UTC %ld = OFFSET %ld",
					ltSecs, currTime, (ltSecs-currTime));

	return (ltSecs - currTime);
}

void TimePrefsHandler::manualTimeZoneChanged()
{
	PmLogDebug(sysServiceLogContext(), "[%s] is called", __FUNCTION__);
	tzTransTimer();
}

bool TimePrefsHandler::setNITZTimeEnable(bool time_en) {	//returns old value

	bool rv = (m_nitzSetting & TimePrefsHandler::NITZ_TimeEnable);

#if defined(HAVE_LUNA_PREFS)
	LPAppHandle lpHandle = 0;
	if (LPAppGetHandle("com.webos.service.systemservice", &lpHandle) == LP_ERR_NONE)
	{
	qDebug("Writing networkTimeEnabled = %d", (int)time_en);
		LPAppSetValueInt(lpHandle, "networkTimeEnabled", (int)time_en);
		LPAppFreeHandle(lpHandle, true);
	}
#endif

	if (time_en) {
		m_nitzSetting |= TimePrefsHandler::NITZ_TimeEnable;
		//schedule a periodic NTP event
		setPeriodicTimeSetWakeup();
	}
	else {
		//clear the flag
		m_nitzSetting &= (~TimePrefsHandler::NITZ_TimeEnable);

		// assume that NTP is no more stored in system-time useful to force
		// update from NTP server through turning off/on useNetworkTime
		m_lastNtpUpdate = 0;
	}

	// assume that current time isn't automatically synchronized and should be
	// overriden with next clockChange after entering back to auto mode
	m_currentTimeSourcePriority = lowestTimeSourcePriority;

	// notify after we've changed our internal flag
	isManualTimeChanged.fire(!time_en);

	return rv;
}

bool TimePrefsHandler::setNITZTZEnable(bool tz_en) //returns old value
{
	bool rv = (m_nitzSetting & TimePrefsHandler::NITZ_TZEnable);

	if (tz_en) {
		m_nitzSetting |= TimePrefsHandler::NITZ_TZEnable;
	}
	else {
		//clear the flag
		m_nitzSetting &= (~TimePrefsHandler::NITZ_TZEnable);
	}

	return rv;
}

const TimeZoneInfo* TimePrefsHandler::timeZone_ZoneFromOffset(int offset,int dstValue, int mcc) const
{
	if (mcc != 0) {

		const TimeZoneInfo* tzMcc = timeZone_ZoneFromMCC(mcc, 0);
		if (tzMcc && !tzMcc->countryCode.empty()) {

			qDebug("MCC code: %d, Offset: %d, DstValue: %d, TZ Entry: %s", mcc, offset, dstValue,
					  tzMcc->jsonStringValue.c_str());

			std::string countryCode = tzMcc->countryCode;

			// All timezones wih matching offset
			std::pair<TimeZoneMultiMapConstIterator, TimeZoneMultiMapConstIterator> iterPair
				= m_offsetZoneMultiMap.equal_range(offset);

			// narrow down list to those matching the MCC code
			TimeZoneInfoList mccMatchingTzList;
			for (TimeZoneMultiMapConstIterator iter = iterPair.first; iter != iterPair.second; ++iter) {
				if (iter->second->countryCode == countryCode) {
					mccMatchingTzList.push_back(iter->second);
				}
			}

			if (!mccMatchingTzList.empty()) {

//				if (dstValue == 1) {

					// First iteration: preferred and DST enabled
					for (TimeZoneInfoListConstIterator iter = mccMatchingTzList.begin();
						 iter != mccMatchingTzList.end(); ++iter) {

						TimeZoneInfo* z = (*iter);
//						if (z->preferred && z->dstSupported == 1) {
						if (z->preferred && z->dstSupported == dstValue) {
						PMLOG_TRACE("Found match in first iteration: %s", z->jsonStringValue.c_str());
							return z;
						}
					}

					// Second iteration: DST enabled
					for (TimeZoneInfoListConstIterator iter = mccMatchingTzList.begin();
						 iter != mccMatchingTzList.end(); ++iter) {

						TimeZoneInfo* z = (*iter);
						if (z->dstSupported == 1) {
						PMLOG_TRACE("Found match in second iteration: %s", z->jsonStringValue.c_str());
							return z;
						}
					}
//				}

				// Third iteration: just preferred
				for (TimeZoneInfoListConstIterator iter = mccMatchingTzList.begin();
					 iter != mccMatchingTzList.end(); ++iter) {

					TimeZoneInfo* z = (*iter);
					if (z->preferred) {
					PMLOG_TRACE("Found match in third iteration: %s", z->jsonStringValue.c_str());
						return z;
					}
				}

				//  Fourth iteration: just matching DST
				for (TimeZoneInfoListConstIterator iter = mccMatchingTzList.begin();
					 iter != mccMatchingTzList.end(); ++iter) {

					TimeZoneInfo* z = (*iter);
					if (z->dstSupported == dstValue) {
					PMLOG_TRACE("Found match in fourth iteration: %s", z->jsonStringValue.c_str());
						return z;
					}
				}

				// Finally: just the first in the list
				TimeZoneInfo* z = mccMatchingTzList.front();
				if (z) {
					qDebug("Found match in fifth iteration: %s", z->jsonStringValue.c_str());
					return z;
				}
			}
		}
	}

	TimeZoneInfo * z = NULL;
	TimeZoneMapConstIterator it;
	if (dstValue == 0) {
		it = m_preferredTimeZoneMapNoDST.find(offset);
		if (it != m_preferredTimeZoneMapNoDST.end())
			z = it->second;
	}
	else {
		it = m_preferredTimeZoneMapDST.find(offset);
		if (it != m_preferredTimeZoneMapDST.end())
			z = it->second;
	}

	return z;
}

const TimeZoneInfo* TimePrefsHandler::timeZone_GenericZoneFromOffset(int offset) const
{

	//scan the sys zones list
	for (TimeZoneInfoListConstIterator 	it = m_syszoneList.begin();
									it != m_syszoneList.end();
									it++)
	{
		TimeZoneInfo * zc = *it;
		if (zc->offsetToUTC == offset)
			return zc;
	}
	return NULL;
}

const TimeZoneInfo* TimePrefsHandler::timeZone_ZoneFromMCC(int mcc,int mnc) const
{
	TimeZoneMapConstIterator it = m_mccZoneInfoMap.find(mcc);
	if (it == m_mccZoneInfoMap.end())
		return NULL;
	return it->second;
}

const TimeZoneInfo* TimePrefsHandler::timeZone_ZoneFromName(const std::string& name, const std::string& city) const
{
	if (name.empty())
		return 0;

	std::string cityString;
	if(name.compare(MANUAL_TZ_NAME) == 0)
	{
		return m_pManualTimeZone;
	}
	for (TimeZoneInfoList::const_iterator it = m_zoneList.begin(); it != m_zoneList.end(); ++it)
	{
		TimeZoneInfo* z = (TimeZoneInfo*) (*it);

		if (z->name == name)
		{
			qDebug("%s: successfully mapped to zone [%s]", __func__, name.c_str());
			convertString(city.c_str(), cityString);
			qDebug("Received [city: [%s], After Translation city: [%s]", city.c_str(), cityString.c_str());
			if( city.empty() || z->city == cityString)
			{
				qDebug("Found city : %s", z->city.c_str());
				return z;
			}
		}
	}

	for (TimeZoneInfoList::const_iterator it = m_syszoneList.begin();
	it != m_syszoneList.end(); ++it) {
		TimeZoneInfo* z = (TimeZoneInfo*) (*it);

		if (z->name == name)
			return z;
	}

	return 0;
}

const TimeZoneInfo* TimePrefsHandler::timeZone_GetDefaultZoneFailsafe()
{
	//No matter what, return *a* zone...never null.
	//order:  try the m_pDefaultTimeZone , then default zone from json, then finally the failsafe default hardcoded

	const TimeZoneInfo * tz = NULL;

	if (m_pDefaultTimeZone)
	{
		tz = timeZone_ZoneFromName(m_pDefaultTimeZone->name);
		return tz;
	}
	else
	{
		tz= timeZone_ZoneFromName(tzNameFromJsonString(getDefaultTZFromJson()));
	}

	if (tz == NULL)
		tz = &s_failsafeDefaultZone;

	return tz;
}

bool TimePrefsHandler::isCountryAcrossMultipleTimeZones(const TimeZoneInfo& tzinfo) const
{
	//placeholder fn in case this logic needs to get more complex
	return ((tzinfo.howManyZonesForCountry) > 1);
}

/*!
\page com_palm_systemservice_time
\n
\section com_palm_systemservice_time_micom_synchronized micomSynchronized

\e Public.

com.webos.service.systemservice/time/micomSynchronized

Notify luna-sysservice about the fact that micom were synchronized with latest
published system time changed.
Note that micom is service that provides functionality similar real-time-clock

\subsection com_palm_systemservice_time_micom_synchronized_syntax Syntax:
\code
{ }
\endcode

\subsection com_palm_systemservice_time_micom_synchronized_returns Returns:
\code
{
	"returnValue": boolean,
	"errorText": string,
	"errorCode": string
}
\endcode

\param returnValue Indicates if the call was succesful.
\param errorText Description of the error if call was not succesful.
\param errorCode Integer number that allows to distinguish some errors that may
				 require different ways of handling. This field is optional.

\subsection com_palm_systemservice_time_micom_synchronized_examples Examples:

\code
luna-send -n 1 -f luna://com.webos.service.systemservice/time/micomSynchronized '{}'
\endcode

Example response for a succesful call:
\code
{ "returnValue": true }
\endcode

Example response for a failed call:
\code
{
	"returnValue": false,
	"errorText": "Not a valid json message",
	"errorCode": 1
}
\endcode
*/
//static
bool TimePrefsHandler::cbMicomSynchronized(LSHandle* lsHandle, LSMessage *message,
										   void *user_data)
{
	EMPTY_SCHEMA_RETURN(lsHandle, message);

	const char *replySuccess = "{\"returnValue\":true}";

	TimePrefsHandler* th = (TimePrefsHandler*) user_data;

	th->m_micomTimeStamp = time(0);

	LSError lsError;
	LSErrorInit(&lsError);
	if (!LSMessageReply(lsHandle, message, replySuccess, &lsError))
	{
		LSErrorFree (&lsError);
	}

	return true;
}

/*!
\page com_palm_systemservice_time
\n
\section com_palm_systemservice_time_set_system_time setSystemTime

\e Public.

com.webos.service.systemservice/time/setSystemTime

Set system time.

\subsection com_palm_systemservice_time_set_system_time_syntax Syntax:
\code
{
	"utc": integer
}
\endcode

\param utc The number of milliseconds since Epoch (midnight of January 1, 1970 UTC), aka - Unix time. Required.

\subsection com_palm_systemservice_time_set_system_time_returns Returns:
\code
{
	"returnValue": boolean,
	"errorText": string,
	"errorCode": string
}
\endcode

\param returnValue Indicates if the call was successful.
\param errorText Description of the error if call was not successful.
\param errorCode Description of the error if call was not successful.

\subsection com_palm_systemservice_time_set_system_time_examples Examples:

\code
luna-send -n 1 -f luna://com.webos.service.systemservice/time/setSystemTime '{"utc": 1346149624 }'
\endcode

Example response for a successful call:
\code
{
	"returnValue": true
}
\endcode

Example response for a failed call:
\code
{
	"returnValue": false,
	"errorText": "malformed json",
	"errorCode": "FAIL"
}
\endcode
*/
//static
bool TimePrefsHandler::cbSetSystemTime(LSHandle* lshandle, LSMessage *message,
							void *user_data)
{
	pbnjson::JSchemaFragment schemaSetSystemTime(JSON(
		{
			"type": "object",
			"properties": {
				"utc": {
					"type": [ "integer" ]
				},
				"timestamp": SCHEMA_TIMESTAMP
			},
			"required": ["utc"],
			"additionalProperties": false
		}
	));
	LSMessageJsonParser parser(message, schemaSetSystemTime);
	ESchemaErrorOptions schErrOption = static_cast<ESchemaErrorOptions>(Settings::instance()->schemaValidationOption);

	if (!parser.parse(__FUNCTION__, lshandle, schErrOption))
		return true;

	LSError lserror;
	LSErrorInit(&lserror);

	if( !parser.getPayload() )
		return false;

	time_t utcTimeInSecs = 0;
	time_t currentTime;
	std::string errorText;
	pbnjson::JValue timestamp;
        std::string lastClockTag;

	TimePrefsHandler* th = (TimePrefsHandler*) user_data;

	PmLogInfo(sysServiceLogContext(), "SET_SYSTEM_TIME", 2,
		PMLOGKS("SENDER", LSMessageGetSenderServiceName(message)),
		PMLOGKFV("MANUAL", "%s", th->isManualTimeUsed() ? "true" : "false"),
		"/time/setSystemTime received with %s",
		parser.getPayload()
	);

	if (!convert(parser.get()["utc"], utcTimeInSecs))
	{
		errorText = "accessing utc integer value failed";
		goto Done_cbSetSystemTime;
	}

	timestamp = parser.get()["timestamp"];
	if ( timestamp.isObject() && timestamp["sec"].isNumber() && timestamp["nsec"].isNumber() )
	{
		timespec sourceTimeStamp;
		sourceTimeStamp.tv_sec = toInteger<time_t>(timestamp["sec"]);
		sourceTimeStamp.tv_nsec = toInteger<long>(timestamp["nsec"]);

		utcTimeInSecs += ClockHandler::evaluateDelay(sourceTimeStamp);
	}

	if (!th->isManualTimeUsed() &&
		(!PrefsDb::instance()->getPref("lastSystemTimeSource", lastClockTag) ||
			lastClockTag == s_factoryTimeSource))
	{
                errorText = "factory time source is set. Ignoring micom time source";
		goto Done_cbSetSystemTime;
	}

	//a new time was specified
	g_warning("%s: settimeofday: %u",__FUNCTION__,(unsigned int)utcTimeInSecs);

	// XXX: Old behaviour would be to set time regardless of "useNetworkTime"
	//      and presence of information from time-source with higher priority.
	//
	// So we should keep that for a while until major version will be changed.
	// To ensure that manual clock source wouldn't be opted-out we'll mark
	// currently set system time with a lowest priority we can.
	// P.S. We are single-threaded so no races should appear here.
	th->m_currentTimeSourcePriority = lowestTimeSourcePriority;

	// We know that /time/setSystemTime was used both for setting time from
	// settings (manual time) and for setting time form different services on
	// boot or whatever.
	// But right now we can't distinguish them, so assume that this is manual
	// set time.
	currentTime = time(0);
	th->deprecatedClockChange.fire(
		utcTimeInSecs - currentTime,
		th->isManualTimeUsed() ? ClockHandler::manual : ClockHandler::micom,
		currentTime
	);

	// TODO: consider moving this code to systemSetTime
	TimePrefsHandler::transitionNITZValidState((th->getLastNITZValidity() & TimePrefsHandler::NITZ_Valid),true);

Done_cbSetSystemTime:

	JObject reply;
	if (errorText.empty())
	{
		//success case
		reply.put("returnValue", true);
	}
	else
	{
		//failure case
		reply.put("returnValue", false);
		reply.put("errorText", errorText);
		reply.put("errorCode", "FAIL");
	}

	LS::Error error;
	(void) LSMessageReply(lshandle, message, reply.stringify().c_str(), error);

	return true;
}

//static
bool TimePrefsHandler::cbAlarmDActivityStatus(LSHandle* lshandle, LSMessage *message,
							void *user_data)
{
	EMPTY_SCHEMA_RETURN(lshandle, message);

	//just log what's happening
	const char* str = LSMessageGetPayload(message);
	if( !str )
		str = "[NO PAYLOAD IN LSMessage!]";
	qDebug("reported status: %s",str);
	return true;
}

/*!
\page com_palm_systemservice_time
\n
\section com_palm_systemservice_time_set_system_network_time setSystemNetworkTime

\e Public.

com.webos.service.systemservice/time/setSystemNetworkTime

Used to send NITZ messages to Luna System Service.

Current date and time can be checked with: <tt>date && ls -l /var/luna/preferences/localtime</tt>

Location of supported timezones (on device): <tt>/usr/palm/ext-timezones.json</tt>

While Airplanmode is off, if Device's timezone setting is off and user manually select the timezone, then when NITZ message arrives, it will use the timezone specified by the device to calculate for times.  Time is always the unix time in seconds.

If Airplane mode is off and both device's Networktimezone & NetworkTime are off, then NITZ message is ignored.

If Airplane mode is off and NetworkTime is off and NetworkTimezone is on then time is calculated based on current device time offset by current NetworkTimezone.  (i.e device is 3pm pacific time with NetworkTime off, then device is traveled to NewYork while in Airplanemode; once arrive in Newyork, turnoff Airplanemode, time is calculated by taking device current time offset by NewYork timezone (it ignores networktime)).

\note Prior using this service to send a fake nitz message to LunaSysService, device must be in AirplaneMode and NetworkTime and Network TimeZone must be turned on.

\subsection com_palm_systemservice_time_set_system_time_syntax Syntax:
\code
{
	"sec": string,
	"min": string,
	"hour": string,
	"mday": string,
	"mon": string,
	"year": string,
	"offset": string,
	"mcc": string,
	"mnc": string,
	"tzvalid": boolean,
	"timevalid": boolean,
	"dstvalid": boolean,
	"dst": integer,
	"timestamp": string,
	"tilIgnore": boolean
}
\endcode

\param sec GMT sec.
\param min GMT min.
\param hour GMT hour.
\param mday Day of the month.
\param mon Month of the year, 0 - 11.
\param year Year calculated from 1900, for example 2012 - 1900 = 112.
\param offset Offset from GMT time in minutes.
\param mcc Country code.
\param mnc Network code assign to carrier within a country
\param tzvalid Is timezone valid. If false, \c mcc and \c offset are used to calculate for time.
\param timevalid Is time valid.
\param dstvalid Is daylight saving time in use.
\param dst If this is 1, month needs to set within the timeframe of DaylightSavingTime (~April - ~Septermber). If this is 0, months needs to be specified outside of DTS( ~november - Feb).
\param timestamp Timestamp.
\param tilIgnore Set as true if you wish to test this service with a fake NITZ message.

\subsection com_palm_systemservice_time_set_system_time_returns Returns:
\code
{
	"returnValue": boolean,
	"errorText": string
}
\endcode

\param returnValue Indicates if the call was succesful.
\param errorText Description of the error if call was not succesful.

\subsection com_palm_systemservice_time_set_system_time_examples Examples:
\code
luna-send -f -n 1 luna://com.webos.service.systemservice/time/setSystemNetworkTime '{"sec":"30","min":"15","offset":"-480","hour":"2","dst":"1","tzvalid":true,"dstvalid":true,"mon":"6","year":"111","timevalid":true,"mday":"1","mcc":"310","mnc":"26", "tilIgnore":true}'
\endcode

Example response for a succesful call:
\code
{
	"returnValue": true
}
\endcode
Example response for a failed call:
\code
{
	"returnValue": false,
	"errorText": "unable to parse json"
}
\endcode
*/
//static
bool TimePrefsHandler::cbSetSystemNetworkTime(LSHandle * lshandle, LSMessage *message, void * user_data)
{
	/*
		{
			"sec": string,
			"min": string,
			"hour": string,
			"mday": string,
			"mon": string,
			"year": string,
			"offset": string,
			"mcc": string,
			"mnc": string,
			"tzvalid": boolean,
			"timevalid": boolean,
			"dstvalid": boolean,
			"dst": integer,
			"timestamp": string,
			"tilIgnore": boolean
		}
	*/
	const char* pSchema = RELAXED_SCHEMA(
							  PROPS_15(PROPERTY(sec, string), PROPERTY(min, string), PROPERTY(hour, string),
									   PROPERTY(mday, string), PROPERTY(mon, string), PROPERTY(year, string),
									   PROPERTY(offset, string), PROPERTY(mcc, string), PROPERTY(mnc, string),
									   PROPERTY(tzvalid, boolean), PROPERTY(timevalid, boolean), PROPERTY(dstvalid, boolean),
									   PROPERTY(dst, integer), PROPERTY(timestamp, string), PROPERTY(tilIgnore, boolean))
							  REQUIRED_15(sec, min, hour, mday, mon, year, offset, mcc, mnc,
										  tzvalid, timevalid, dstvalid, dst, timestamp, tilIgnore));

	VALIDATE_SCHEMA_AND_RETURN_OPTION(lshandle, message, pSchema, EValidateAndErrorAlways);

	PmLogInfo(sysServiceLogContext(), "SET_SYSTEM_NET_TIME", 1,
		PMLOGKS("SENDER", LSMessageGetSenderServiceName(message)),
		"/time/setSystemNetworkTime received with %s",
		LSMessageGetPayload(message)
	);

	LSError lserror;
	std::string errorText;

	int rc=0;
	int utcOffset=-1;
	struct tm timeStruct;
	bool dstValid = false;
	bool tzValid = false;
	bool timeValid = false;
	int dst = 0;
	int mcc = 0;
	int mnc = 0;
	time_t remotetimeStamp = 0;
	NitzParameters nitzParam;
	int nitzFlags = 0;
	std::string nitzFnMsg;
	JValue label;

	TimePrefsHandler* th = (TimePrefsHandler*)user_data;

	const char* str = LSMessageGetPayload(message);
	if( !str )
		return false;

	JValue root = JDomParser::fromString(str);
	if (!root.isObject()) {
		errorText = std::string("unable to parse json");
		goto Done_cbSetSystemNetworkTime;
	}

	memset(&timeStruct,0,sizeof(struct tm));
	qDebug("NITZ message received from Telephony Service: %s",str);

	label = root["sec"];
	if (label.isString())
		timeStruct.tm_sec = strtol(label.asString().c_str(), 0, 10);
	else
		++rc;

	label = root["min"];
	if (label.isString())
		timeStruct.tm_min = strtol(label.asString().c_str(), 0, 10);
	else
		++rc;

	label = root["hour"];
	if (label.isString())
		timeStruct.tm_hour = strtol(label.asString().c_str(), 0, 10);
	else
		++rc;

	label = root["mday"];
	if (label.isString())
		timeStruct.tm_mday = strtol(label.asString().c_str(), 0, 10);
	else
		++rc;

	label = root["mon"];
	if (label.isString())
		timeStruct.tm_mon = strtol(label.asString().c_str(), 0, 10);
	else
		++rc;

	label = root["year"];
	if (label.isString())
		timeStruct.tm_year = strtol(label.asString().c_str(), 0, 10);
	else
		++rc;

	label = root["offset"];
	if (label.isString())
		utcOffset = strtol(label.asString().c_str(), 0, 10);
	else
		utcOffset = -1000;					// this is an invalid value so it can be detected later on

	label = root["mcc"];
	if (label.isString())
		mcc = strtol(label.asString().c_str(), 0, 10);
	else
		mcc = 0;

	label = root["mnc"];
	if (label.isString())
		mnc = strtol(label.asString().c_str(), 0, 10);
	else
		mnc = 0;

	label = root["tzvalid"];
	if (label.isBoolean())
		tzValid = label.asBool();

	dbg_time_tzvalidOverride(tzValid);

	label = root["timevalid"];
	if (label.isBoolean())
		timeValid = label.asBool();

	dbg_time_timevalidOverride(timeValid);

	label = root["dstvalid"];
	if (label.isBoolean())
		dstValid = label.asBool();

	dbg_time_dstvalidOverride(dstValid);

	label = root["dst"];
	if (label.isNumber())
		dst = label.asNumber<int>();

	//additional param checks
	if (utcOffset == -1000)
		tzValid = false;

	//check to see if there is a valid timestamp
	label = root["timestamp"];
	if (label.isString())
		remotetimeStamp = strtoul(label.asString().c_str(), 0, 10);
	else
		remotetimeStamp = 0;			//...I suppose this can be valid in some cases...like for "threshold" seconds when the time() clock rolls over [not a big deal]

	label = root["tilIgnore"];
	if (label.isBoolean())
	{
		if (label.asBool())
			nitzFlags |= NITZHANDLER_FLAGBIT_IGNORE_TIL_SET;
	}

	nitzParam = NitzParameters(timeStruct, utcOffset, dst, mcc, mnc, timeValid,
							   tzValid, dstValid, remotetimeStamp); //wasteful copy but this fn isn't called much

	//run the nitz chain
	if (th->nitzHandlerEntry(nitzParam,nitzFlags,nitzFnMsg) != NITZHANDLER_RETURN_SUCCESS)
	{
		errorText = "nitz message failed entry: "+nitzFnMsg;
		goto Done_cbSetSystemNetworkTime;
	}
	if (th->nitzHandlerTimeValue(nitzParam,nitzFlags,nitzFnMsg) != NITZHANDLER_RETURN_SUCCESS)
	{
		errorText = "nitz message failed in time-value handler: "+nitzFnMsg;
		goto Done_cbSetSystemNetworkTime;
	}
	if (th->nitzHandlerOffsetValue(nitzParam,nitzFlags,nitzFnMsg) != NITZHANDLER_RETURN_SUCCESS)
	{
		errorText = "nitz message failed in timeoffset-value handler: "+nitzFnMsg;
		goto Done_cbSetSystemNetworkTime;
	}
	if (th->nitzHandlerDstValue(nitzParam,nitzFlags,nitzFnMsg) != NITZHANDLER_RETURN_SUCCESS)
	{
		errorText = "nitz message failed in timedst-value handler: "+nitzFnMsg;
		goto Done_cbSetSystemNetworkTime;
	}
	if (th->nitzHandlerExit(nitzParam,nitzFlags,nitzFnMsg) != NITZHANDLER_RETURN_SUCCESS)
	{
		errorText = "nitz message failed exit: "+nitzFnMsg;
		goto Done_cbSetSystemNetworkTime;
	}

	//if successfully completed, then reset the last nitz parameter member and flags
	if (th->m_p_lastNitzParameter == NULL)
		th->m_p_lastNitzParameter = new NitzParameters(nitzParam);
	else
		*(th->m_p_lastNitzParameter) = nitzParam;

	th->m_lastNitzFlags = nitzFlags;

Done_cbSetSystemNetworkTime:

	//start the timeout cycle for completing NITZ processing later
	th->startTimeoutCycle();


	JObject reply;
	if (errorText.empty())
	{
		//success
		reply.put("returnValue", true);
	}
	else
	{
		reply.put("returnValue", false);
		reply.put("errorText", errorText);
		qWarning() << errorText.c_str();
	}

	LS::Error error;
	(void) LSMessageReply(lshandle, message, reply.stringify().c_str(), error);

	return true;
}

int  TimePrefsHandler::nitzHandlerEntry(NitzParameters& nitz,int& flags,std::string& r_statusMsg)
{
	//check the validity of the received nitz message
	if (nitz.valid() == false)
	{
		r_statusMsg = "timestamps are too far apart";
		return NITZHANDLER_RETURN_ERROR;
	}
	//set up the flags
	if (PrefsDb::instance()->getPref("timeZonesUseGenericExclusively") == "true")
		flags |= NITZHANDLER_FLAGBIT_GZONEFORCE;
	if (PrefsDb::instance()->getPref("AllowGenericTimezones") == "true")
		flags |= NITZHANDLER_FLAGBIT_GZONEALLOW;
	if (PrefsDb::instance()->getPref("AllowMCCAssistedTimezones") == "true")
		flags |= NITZHANDLER_FLAGBIT_MCCALLOW;
	if (PrefsDb::instance()->getPref("AllowNTPTime") == "true")
		flags |= NITZHANDLER_FLAGBIT_NTPALLOW;

	return NITZHANDLER_RETURN_SUCCESS;
}

int  TimePrefsHandler::nitzHandlerTimeValue(NitzParameters& nitz, int& flags, std::string& r_statusMsg)
{

	if (isNITZTimeEnabled() == false)
		return NITZHANDLER_RETURN_SUCCESS;			//automatic time adjustments are not allowed

	if (flags & NITZHANDLER_FLAGBIT_IGNORE_TIL_SET)
	{
		time_t utc = timegm(&(nitz._timeStruct));
		if (utc == (time_t)-1) // timegm error
		{
			nitz._timevalid = false;
		}
		else
		{
			// route to proper handler
			time_t currentTime = time(0);
			deprecatedClockChange.fire(utc - currentTime, "nitz", currentTime);
			nitz._timevalid = true;
		}
	}

	if (nitz._timevalid)
	{
		signalReceivedNITZUpdate(true,false);
		return NITZHANDLER_RETURN_SUCCESS;			//the time was already set by the TIL...nothing to do, so exit
	}

	//check to see if NTP time is allowed.
	if ((flags & NITZHANDLER_FLAGBIT_NTPALLOW) == 0)
		return NITZHANDLER_RETURN_SUCCESS;			//no NTP allowed...nothing left to do

	updateSystemTime();

	return NITZHANDLER_RETURN_SUCCESS;

}

int	 TimePrefsHandler::nitzHandlerOffsetValue(NitzParameters& nitz,int& flags,std::string& r_statusMsg)
{
	if (isNITZTZEnabled() == false)
		return NITZHANDLER_RETURN_SUCCESS;

	nitzHandlerSpecialCaseOffsetValue(nitz,flags,r_statusMsg);

	if (nitz._tzvalid == false)
		return NITZHANDLER_RETURN_SUCCESS;			///this is not a message with a tz offset value...return (not an error)

	//try and set the timezone
	const TimeZoneInfo * selectedZone = NULL;

	//check to see if I've been told to use generic timezones exclusively
	if ( flags & NITZHANDLER_FLAGBIT_GZONEFORCE )
	{
		//pick a generic zone
		selectedZone = timeZone_GenericZoneFromOffset(nitz._offset);
		setTimeZone(selectedZone);					///setTimeZone() has a failsafe against NULLs being passed in so this is safe
		signalReceivedNITZUpdate(false,true);
		return NITZHANDLER_RETURN_SUCCESS;
	}

	int effectiveDstValue = nitz._dst;
	//try and pick a zone based on offset and dst passed in. In the case of dstvalid = false, assume dst=1. This will be corrected when an updated message comes in (IF it comes in...else, 1 it is)
	if (nitz._dstvalid)
	{
		flags |= NITZHANDLER_FLAGBIT_SKIP_DST_SELECT;
	}
	else
		effectiveDstValue = 0;

	selectedZone = timeZone_ZoneFromOffset(nitz._offset,effectiveDstValue,nitz._mcc);
	if (selectedZone == NULL)
	{
		//couldn't get one with this combination...if generic zones are allowed, pick one of those
		if ( flags & NITZHANDLER_FLAGBIT_GZONEALLOW )
		{
			//pick a generic zone
			selectedZone = timeZone_GenericZoneFromOffset(nitz._offset);
		}
	}

	setTimeZone(selectedZone);					///setTimeZone() has a failsafe against NULLs being passed in so this is safe
	signalReceivedNITZUpdate(false,true);
	return NITZHANDLER_RETURN_SUCCESS;
}

int  TimePrefsHandler::nitzHandlerDstValue(NitzParameters& nitz,int& flags,std::string& r_statusMsg)
{

	// enforcing rules for dst according to some test cases that need to be passed with explicit assumptions on dstvalid <-> dst=x implications. Therefore
	// 					 	will handle everything in the nitzHandlerOffsetValue() fn

//	if (isNITZTZEnabled() == false)
//		return NITZHANDLER_RETURN_SUCCESS;
//
//	if (nitz._dstvalid == false)
//		return NITZHANDLER_RETURN_SUCCESS;		//this is not a message with a dst value...return (not an error)
//
//	if (flags & NITZHANDLER_FLAGBIT_SKIP_DST_SELECT)
//		return NITZHANDLER_RETURN_SUCCESS;		//something up the chain already figured dst into the mix...skip all this to avoid reselecting a TZ
//
//	//take the currently set timezone's offset, and this NitzParameter's dst value, and run the timezone selection sequence
//
//
//	const TimeZoneInfo * selectedZone = NULL;
//	//check to see if I've been told to use generic timezones exclusively
//	if( flags & NITZHANDLER_FLAGBIT_GZONEFORCE )
//	{
//		//pick a generic zone
//		selectedZone = timeZone_GenericZoneFromOffset(m_cpCurrentTimeZone->offsetToUTC);
//		setTimeZone(selectedZone);					///setTimeZone() has a failsafe against NULLs being passed in so this is safe
//		return NITZHANDLER_RETURN_SUCCESS;
//	}
//
//	selectedZone = timeZone_ZoneFromOffset(m_cpCurrentTimeZone->offsetToUTC,nitz._dst);
//	if (selectedZone == NULL)
//	{
//		//couldn't get one with this combination...if generic zones are allowed, pick one of those
//		if ( flags & NITZHANDLER_FLAGBIT_GZONEALLOW )
//		{
//			//pick a generic zone
//			selectedZone = timeZone_GenericZoneFromOffset(m_cpCurrentTimeZone->offsetToUTC);
//		}
//	}
//	setTimeZone(selectedZone);					///setTimeZone() has a failsafe against NULLs being passed in so this is safe
	return NITZHANDLER_RETURN_SUCCESS;
}

int  TimePrefsHandler::nitzHandlerExit(NitzParameters& nitz,int& flags,std::string& r_statusMsg)
{
	//nothing special to do here at this time...just a hook to allow future post-process
	return NITZHANDLER_RETURN_SUCCESS;
}

void  TimePrefsHandler::nitzHandlerSpecialCaseOffsetValue(NitzParameters& nitz,int& flags,std::string& r_statusMsg)
{
	//Special Case #1:  If the MCC is France (208), and the offset value is 120, then flip that to offset 60, tzvalid=true, dst=1, dstvalid=true
	if ((nitz._mcc == 208) && (nitz._offset == 120))
	{
		nitz._tzvalid = true;
		nitz._offset = 60;
		nitz._dst = 1;
		nitz._dstvalid = true;
		qWarning() << "Special Case 1 applied! MCC 208 offset 120 -> offset 60, dst=1";
		return;
	}

	//Special Case #2:  If the MCC is Spain (214), and the offset value is 120, then flip that to offset 60, tzvalid=true, dst=1, dstvalid=true
	if ((nitz._mcc == 214) && (nitz._offset == 120))
	{
		nitz._tzvalid = true;
		nitz._offset = 60;
		nitz._dst = 1;
		nitz._dstvalid = true;
		qWarning() << "Special Case 2 applied! MCC 214 offset 120 -> offset 60, dst=1";
		return;
	}
}

int TimePrefsHandler::timeoutFunc()
{
	if (m_timeoutCycleCount > 0)
	{
		//the timeout has been extended..decrement count and return signaling that cycle should repeat
		--m_timeoutCycleCount;
		qDebug("Resetting the timeout cycle, count is now %d", m_timeoutCycleCount);
		return TIMEOUTFN_RESETCYCLE;
	}

	//else, timeout needs to do work
	//run the nitz chain
	int nitzFlags = 0;
	std::string errorText,nitzFnMsg;
	NitzParameters nitzParam;		//this will be the "working copy" that the handlers will modify

	qDebug("Running the NITZ chain...");
	if (timeoutNitzHandlerEntry(nitzParam,nitzFlags,nitzFnMsg) != NITZHANDLER_RETURN_SUCCESS)
	{
		errorText = "timeout-nitz message failed entry: "+nitzFnMsg;
		goto Done_timeoutFunc;
	}
	if (timeoutNitzHandlerTimeValue(nitzParam,nitzFlags,nitzFnMsg) != NITZHANDLER_RETURN_SUCCESS)
	{
		errorText = "timeout-nitz message failed in time-value handler: "+nitzFnMsg;
		goto Done_timeoutFunc;
	}
	if (timeoutNitzHandlerOffsetValue(nitzParam,nitzFlags,nitzFnMsg) != NITZHANDLER_RETURN_SUCCESS)
	{
		errorText = "timeout-nitz message failed in timeoffset-value handler: "+nitzFnMsg;
		goto Done_timeoutFunc;
	}
	if (timeoutNitzHandlerDstValue(nitzParam,nitzFlags,nitzFnMsg) != NITZHANDLER_RETURN_SUCCESS)
	{
		errorText = "timeout-nitz message failed in timedst-value handler: "+nitzFnMsg;
		goto Done_timeoutFunc;
	}
	if (timeoutNitzHandlerExit(nitzParam,nitzFlags,nitzFnMsg) != NITZHANDLER_RETURN_SUCCESS)
	{
		errorText = "timeout-nitz message failed exit: "+nitzFnMsg;
		goto Done_timeoutFunc;
	}

	//if successfully completed, then reset the last nitz parameter member and flags
	if (m_p_lastNitzParameter == NULL)
		m_p_lastNitzParameter = new NitzParameters(nitzParam);
	else
		*m_p_lastNitzParameter = nitzParam;

	m_lastNitzFlags = nitzFlags;

Done_timeoutFunc:

	if (errorText.size()) qWarning() << "NITZ chain completed:" << errorText.c_str();
	else qDebug ("NITZ chain completed OK");

	//if neither automatic time or automatic zone were turned on, then skip advertising the system time or nitz valid status
	/*
	 * The whole chain run could have just been avoided at the cb__ function level if "manual" mode was on....
	 * The chains were still run despite "manual" mode being on, because it may be good to have a structure in place that can
	 * track and possibly remember nitz messages even though they are not getting applied. That type of thing isn't being used here
	 * (yet) but if a switch to that is needed, it's already mostly in place. Having this run always isn't that costly anyways so
	 * there is no real drawback.
	 *
	 */
	if ((isNITZTimeEnabled() == false) && (isNITZTZEnabled() == false))
	{
		qDebug ("Manual mode was on...not changing any NITZ variables/state");
		//finish, indicating I'd like the periodic source to go away
		return TIMEOUTFN_ENDCYCLE;
	}

	//figure out if everything was set ok
	if ((nitzParam._timevalid == false) && (nitzParam._tzvalid == false) && (nitzParam._dstvalid == false))
	{
		m_immNitzTimeValid = false;
		m_immNitzZoneValid = false;
		qWarning() << "Special-NITZ FAIL scenario detected - UI prompt to follow";
		//no...set the overall validity flags (for tracking UI) to invalid, and post the inability to set the time
		(void)TimePrefsHandler::transitionNITZValidState(false,false);
		markLastNITZInvalid();
		postNitzValidityStatus();
	}
	else
	{
		bool totallyGoodNitz = (nitzParam._timevalid) && (nitzParam._tzvalid) && (nitzParam._dstvalid);
		time_t dbg_time_outp = time(NULL);
		qDebug("NITZ FINAL: At least something was ok (timevalid = %s,tzvalid = %s,dstvalid = %s), time is now %s",
			(nitzParam._timevalid ? "true" : "false"),
				(nitzParam._tzvalid ? "true" : "false"),
				(nitzParam._dstvalid ? "true" : "false"),
				(ctime(&dbg_time_outp)));

		//yes...at least something was ok
		//set the right overall validity flags (for tracking UI), post the timechange, and launch apps
		(void)TimePrefsHandler::transitionNITZValidState(totallyGoodNitz,false);
		if (totallyGoodNitz)
			markLastNITZValid();
		else
			markLastNITZInvalid();
		// also set the new sub-values for validity (redundant, but not disturbing the old nitz state machine for now
		//should be phased out slowly though from here on in)
		m_immNitzTimeValid = nitzParam._timevalid;
		m_immNitzZoneValid = (nitzParam._tzvalid && nitzParam._dstvalid);

		// notify about time-zone update if we had some
		if (m_immNitzZoneValid)
		{
			// TODO: consider moving to systemSetTimeZone
			postSystemTimeChange();
			postBroadcastEffectiveTimeChange();
			launchAppsOnTimeChange();
			tzTransTimer();
		}
	}

	//then finish, indicating I'd like the periodic source to go away
	return TIMEOUTFN_ENDCYCLE;
}

void TimePrefsHandler::tzTransTimer(time_t timeout)
{
	if ( m_gsource_tzTrans ) {
		/* if registered other timeout source which is not fired */
		g_source_destroy(m_gsource_tzTrans);
		m_gsource_tzTrans = NULL;
	}

	if ( !m_cpCurrentTimeZone )
		return;

	time_t remainSec;
	if ( timeout > 0 ) {
		m_nextTzTrans = time(0) + timeout;
		remainSec = timeout;
	} else {
		m_nextTzTrans = TimeZoneService::instance()->nextTzTransition(m_cpCurrentTimeZone->name);
		if ( m_nextTzTrans == -1 )
			return;
		remainSec = m_nextTzTrans - time(0);
	}

	if ( remainSec < 1 ) {
		PmLogInfo(sysServiceLogContext(), "TIMEZONE_TRANSITION", 2,
				PMLOGKFV("Next", "%d", m_nextTzTrans),
				PMLOGKFV("UTC", "%d", time(0)),
				"Incorrect tzTrans information");

		return;
	}

	m_gsource_tzTrans = g_timeout_source_new_seconds(remainSec);
	if ( m_gsource_tzTrans == NULL ) {
		PmLogInfo(sysServiceLogContext(), "TIMEZONE_TRANSITION", 0,
				"Fail to create new timeout source");

		return;
	}

	g_source_set_callback(m_gsource_tzTrans,
			TimePrefsHandler::tzTrans, m_gsource_tzTrans,
			TimePrefsHandler::tzTransCancel);

	GMainContext *context = g_main_loop_get_context(g_mainloop.get());
	m_gsource_tzTrans_id = g_source_attach(m_gsource_tzTrans, context);
	if (m_gsource_tzTrans_id == 0) {
		PmLogInfo(sysServiceLogContext(), "TIMEZONE_TRANSITION", 0,
				"Timer source attachment error");

		g_source_unref(m_gsource_tzTrans);
		m_gsource_tzTrans = NULL;
	} else {
		PmLogInfo(sysServiceLogContext(), "TIMEZONE_TRANSITION", 1,
				PMLOGKFV("Next", "%d", m_nextTzTrans),
				"TimeZone transition after %d seconds", remainSec);

		//it's owned now by the context
		g_source_unref(m_gsource_tzTrans);
	}

	return;
}

inline void TimePrefsHandler::tzTransTimerAnew(time_t timeout)
{
	/* Reset values to re-init timer in tzTransTimer.
	 * Current m_source_tzTrans should be destroyed by other */
	m_gsource_tzTrans = NULL;
	m_gsource_tzTrans_id = 0;
	m_nextTzTrans = -1;
	tzTransTimer(timeout);
}

gboolean TimePrefsHandler::tzTrans(gpointer userData)
{
	TimePrefsHandler* inst = TimePrefsHandler::instance();

	time_t wakeupErr = inst->m_nextTzTrans - time(0);
	if ( wakeupErr > 0 ) {
		/* Timeout handler wake up early sometimes because
		 * g_source_timeout_new_second is not precise. */
		inst->tzTransTimerAnew(wakeupErr);
		return FALSE;
	}

	if ( inst->m_cpCurrentTimeZone ) {
		PmLogInfo(sysServiceLogContext(), "TIMEZONE_TRANSITION", 3,
				PMLOGKFV("ZoneId", "\"%s\"", inst->m_cpCurrentTimeZone->name.c_str()),
				PMLOGKFV("Offset", "%d", inst->m_cpCurrentTimeZone->offsetToUTC),
				PMLOGKFV("DST", "%s", inst->m_cpCurrentTimeZone->dstSupported ? "true" : "false"),
				"TimeZone offset is changed");
	} else {
		PmLogInfo(sysServiceLogContext(), "TIMEZONE_TRANSITION", 0, "Unknown Time Zone");
	}

	inst->postSystemTimeChange();
	inst->postBroadcastEffectiveTimeChange();
	inst->launchAppsOnTimeChange();

	inst->tzTransTimerAnew();

	return FALSE;
}

void TimePrefsHandler::tzTransCancel(gpointer userData)
{
	TimePrefsHandler* inst = TimePrefsHandler::instance();

	/* Do nothing if this g_timeout_source is not considered */
	if ( !inst->m_gsource_tzTrans || userData != inst->m_gsource_tzTrans )
		return;

	inst->m_gsource_tzTrans = NULL;
	inst->m_gsource_tzTrans_id = 0;
}

void TimePrefsHandler::startBootstrapCycle(int delaySeconds)
{

//#if defined(MACHINE_topaz) || defined(DESKTOP) || defined(MACHINE_opal)
// @TODO: better handle devices with and without cellulrr
	qDebug("No Cellular...kicking off time-set timeout cycle in %d seconds (to allow machine to settle down)", delaySeconds);
	if (m_p_lastNitzParameter)
	{
		m_p_lastNitzParameter->_timevalid = false;	//this will force NTP
	}
	startTimeoutCycle(delaySeconds);
//#endif

}

void TimePrefsHandler::startTimeoutCycle()
{
	startTimeoutCycle(TIMEOUT_INTERVAL_SEC);
}

void TimePrefsHandler::startTimeoutCycle(unsigned int timeoutInSeconds)
{
	//if one is already running, extend it up to one cycle
	if (m_gsource_periodic)
	{
		m_timeoutCycleCount = (m_timeoutCycleCount > 0 ? 1 : 0);
		qDebug("timeout cycle count extended , now %d", m_timeoutCycleCount);
		return;
	}

	//else, create a timeout source and attach it
	//create a new periodic source

	if (timeoutInSeconds == 0)
	{
		timeoutInSeconds = strtoul((PrefsDb::instance()->getPref(".sysservice-time-nitzHandlerTimeout")).c_str(),NULL,10L);
		if ((timeoutInSeconds == 0) || (timeoutInSeconds > 300))
			timeoutInSeconds = TIMEOUT_INTERVAL_SEC;
	}

	m_gsource_periodic = g_timeout_source_new_seconds(timeoutInSeconds);
	if (m_gsource_periodic == NULL)
	{
		qWarning() << "Failed to create periodic source";
		return;
	}
	//add the callback functions to it
	g_source_set_callback(m_gsource_periodic,TimePrefsHandler::source_periodic,m_gsource_periodic,TimePrefsHandler::source_periodic_destroy);
			//attach the new periodic source
	GMainContext *context = g_main_loop_get_context(g_mainloop.get());
	m_gsource_periodic_id = g_source_attach(m_gsource_periodic,context);
	if (m_gsource_periodic_id == 0)
	{
		qWarning() << "Failed to attach periodic source";
		//destroy the periodic source
		g_source_destroy(m_gsource_periodic);
		m_gsource_periodic = NULL;
	}
	else {
		qDebug("Timeout cycle of %d seconds started", timeoutInSeconds);
		g_source_unref(m_gsource_periodic);		//it's owned now by the context
	}

}

void TimePrefsHandler::timeout_destroy(gpointer userData)
{
	if (userData != m_gsource_periodic)
		return;			//makes it harder for someone to call this function directly

	m_gsource_periodic_id = 0;
	m_gsource_periodic = NULL;
}

int  TimePrefsHandler::timeoutNitzHandlerEntry(NitzParameters& nitz,int& flags,std::string& r_statusMgs)
{
	//if a previous message was valid, use the flags from the previous message.
	if (m_p_lastNitzParameter)
	{
		flags = m_lastNitzFlags;
		nitz = *m_p_lastNitzParameter;
	}
	else
	{
		//rescan from prefs
		if (PrefsDb::instance()->getPref("timeZonesUseGenericExclusively") == "true")
			flags |= NITZHANDLER_FLAGBIT_GZONEFORCE;
		if (PrefsDb::instance()->getPref("AllowGenericTimezones") == "true")
			flags |= NITZHANDLER_FLAGBIT_GZONEALLOW;
		if (PrefsDb::instance()->getPref("AllowMCCAssistedTimezones") == "true")
			flags |= NITZHANDLER_FLAGBIT_MCCALLOW;
		if (PrefsDb::instance()->getPref("AllowNTPTime") == "true")
			flags |= NITZHANDLER_FLAGBIT_NTPALLOW;
	}

	return NITZHANDLER_RETURN_SUCCESS;
}

int  TimePrefsHandler::timeoutNitzHandlerTimeValue(NitzParameters& nitz,int& flags,std::string& r_statusMsg)
{
	/*
	 * Even though NTP was attempted in the original handler when the NITZ message came in, if that was unsuccessful (which means _timevalid is still == false),
	 * try again here. Maybe a retry after the short delay it took to get here will work
	 */

	//check if network time is allowed, if not, then exit
	if (isNITZTimeEnabled() == false)
		return NITZHANDLER_RETURN_SUCCESS;

	//check the timevalid field
	if (nitz._timevalid)
	{
		return NITZHANDLER_RETURN_SUCCESS;			//time was successfully applied in the original nitz cycle...nothing to do
	}

	//else, check to see if NTP time is allowed.
	if ((flags & NITZHANDLER_FLAGBIT_NTPALLOW) == 0)
	{
		return NITZHANDLER_RETURN_SUCCESS;			//no NTP allowed...nothing left to do
	}

	updateSystemTime();

	return NITZHANDLER_RETURN_SUCCESS;

}

int	 TimePrefsHandler::timeoutNitzHandlerOffsetValue(NitzParameters& nitz,int& flags,std::string& r_statusMsg)
{
	//if automatic timezone is not allowed, then just exit
	if (isNITZTZEnabled() == false)
		return NITZHANDLER_RETURN_SUCCESS;

	//check the tzvalid field
	if (nitz._tzvalid)
	{
		return NITZHANDLER_RETURN_SUCCESS;			//time zone was successfully applied in the original nitz cycle...nothing to do
	}

	const TimeZoneInfo * tz = NULL;
	if ( flags & NITZHANDLER_FLAGBIT_MCCALLOW )
	{
		//try and pick a zone by MCC
		tz = timeZone_ZoneFromMCC(nitz._mcc,nitz._mnc);
		if (tz)
		{
			//found one!  ...set it...but first, see if the "name" is set. If not, reselect based on the offset and dst (not all MCC table zones have names)
			nitz._offset = tz->offsetToUTC;
			nitz._dst = tz->dstSupported;
			if (tz->name.empty())
			{
				tz = timeZone_ZoneFromOffset(nitz._offset,nitz._dst);
				//check to see that this zone's country doesn't span multiple zones...if it does, then it can't be used,
				// so exit early
				//if the name WAS set though, assume that the intent was to override this logic and set it
				if (isCountryAcrossMultipleTimeZones(*tz))
					return NITZHANDLER_RETURN_SUCCESS;
			}
			nitz._tzvalid = true;
			nitz._dstvalid = true;
			setTimeZone(tz);
			signalReceivedNITZUpdate(false,true);
			return NITZHANDLER_RETURN_SUCCESS;
		}
	}

	return NITZHANDLER_RETURN_SUCCESS;
}

int  TimePrefsHandler::timeoutNitzHandlerDstValue(NitzParameters& nitz,int& flags,std::string& r_statusMsg)
{
	//nothing to do here...dst can't really be helped if it didn't come in initially

	if (flags & NITZHANDLER_FLAGBIT_SKIP_DST_SELECT)
		return NITZHANDLER_RETURN_SUCCESS;		//something up the chain already figured dst into the mix...skip all this to avoid reselecting a TZ

	///However, some networks  seem to be sending dstvalid = false even when it shouldn't be. This hidden setting defaults to ignoring that
	//				but if it's set "true", then dstvalid = false will result in it being considered a NITZ TZ set failure
	if (PrefsDb::instance()->getPref(".sysservice-time-strictDstErrors") != "true")
		nitz._dstvalid = true;

	return NITZHANDLER_RETURN_SUCCESS;
}

int  TimePrefsHandler::timeoutNitzHandlerExit(NitzParameters& nitz,int& flags,std::string& r_statusMsg)
{
	return NITZHANDLER_RETURN_SUCCESS;
}

void TimePrefsHandler::setPeriodicTimeSetWakeup()
{
	qDebug("%s called",__FUNCTION__);

	if (getServiceHandle() == NULL)
	{
		//not yet on the bus
		m_sendWakeupSetToAlarmD = true;
		return;
	}

	//if (isNITZTimeEnabled() && isNTPAllowed())
	// TODO:  should really be this and not the line below, but since "AllowNTPTime" setting/key currently doesn't
	//     have a "changed" handler, there's no way to detect that it has been turned (back)on, so if it ever
	//     gets set off, there will be no more NTP events scheduled even if it gets turned back on
	if (isNITZTimeEnabled())
	{
		LSError lserror;
		LSErrorInit(&lserror);

		std::string interval = PrefsDb::instance()->getPref(".sysservice-time-autoNtpInterval");
		uint32_t timev = strtoul(interval.c_str(),NULL,10);
		if ((timev < 300) || (timev > 86400))
			timev = 86399;							//24 hour default (23h.59m.59s actually)

		std::string timeStr;

		uint32_t hr = timev / 3600;
		uint32_t mr = timev % 3600;

		if (hr >= 10)
			timeStr = Utils::toSTLString<uint32_t>(hr) + std::string(":");
		else
			timeStr = std::string("0")+Utils::toSTLString<uint32_t>(hr) + std::string(":");

		if (mr >= 600)
			timeStr.append(Utils::toSTLString<uint32_t>(mr/60)+std::string(":"));
		else
			timeStr.append(std::string("0")+Utils::toSTLString<uint32_t>(mr/60)+std::string(":"));

		uint32_t sr = mr % 60;
		if (sr >= 10)
			timeStr.append(Utils::toSTLString<uint32_t>(sr));
		else
			timeStr.append(std::string("0")+Utils::toSTLString<uint32_t>(sr));

		//std::string payload = "{\"key\":\"sysservice_ntp_periodic\",\"in\":\"01:00:00\",\"wakeup\":false,\"uri\":\"luna://com.webos.service.systemservice/time/setTimeWithNTP\",\"params\":\"{'source':'periodic'}\"}";

		std::string payload = std::string("{\"key\":\"sysservice_ntp_periodic\",\"in\":\"")
								+timeStr
								+std::string("\",\"wakeup\":false,\"uri\":\"luna://com.webos.service.systemservice/time/setTimeWithNTP\",\"params\":\"{\\\"source\\\":\\\"periodic\\\"}\"}");

		qDebug("scheduling event for %s in the future or when the device next wakes, whichever is later", timeStr.c_str());
		bool lsCallResult = LSCall(getServiceHandle(),
				"luna://com.webos.service.alarm/set",
				payload.c_str(),
				cbSetPeriodicWakeupAlarmDResponse,this,NULL, &lserror);

		if (!lsCallResult) {
			qWarning() << "call to alarmD failed";
			LSErrorFree(&lserror);
			m_sendWakeupSetToAlarmD = true;
		}
		else
		{
			m_sendWakeupSetToAlarmD = false;		//unless the response tells me otherwise, assume it succeeded so supress reschedule on alarmD reconnect
		}
	}
	else
	{
		m_sendWakeupSetToAlarmD = false;
	}
}


bool TimePrefsHandler::isNTPAllowed()
{
	return (PrefsDb::instance()->getPref("AllowNTPTime") == "true");
}

void TimePrefsHandler::signalReceivedNITZUpdate(bool time,bool zone)
{
	LSError lsError;

	if (time)
	{
		LSErrorInit(&lsError);
		if (!(LSCall(getServiceHandle(),
					"luna://com.webos.service.systemservice/setPreferences",
					"{\"receiveNetworkTimeUpdate\":true}",
					NULL,this,NULL, &lsError)))
		{
			LSErrorFree(&lsError);
		}
	}
	if (zone)
	{
		LSErrorInit(&lsError);
		if (!(LSCall(getServiceHandle(),
					"luna://com.webos.service.systemservice/setPreferences",
					"{\"receiveNetworkTimezoneUpdate\":true}",
					NULL,this,NULL, &lsError)))
		{
			LSErrorFree(&lsError);
		}
	}
}

//static
void TimePrefsHandler::dbg_time_timevalidOverride(bool& timevalid)
{
	if (PrefsDb::instance()->getPref(".sysservice-dbg-time-debugEnable") != "true")
		return;

	PMLOG_TRACE("!!!!!!!!!!!!!!! USING DEBUG OVERRIDES !!!!!!!!!!!!!!");
	std::string v = PrefsDb::instance()->getPref(".sysservice-dbg-time-timevalid");
	if (strcasecmp(v.c_str(),"true") == 0)
		timevalid = true;
	else if (strcasecmp(v.c_str(),"false") == 0)
		timevalid = false;

	qDebug("timevalid <--- %s", (timevalid ? "true" : "false"));
}

//static
void TimePrefsHandler::dbg_time_tzvalidOverride(bool& tzvalid)
{
	if (PrefsDb::instance()->getPref(".sysservice-dbg-time-debugEnable") != "true")
		return;

	PMLOG_TRACE("!!!!!!!!!!!!!!! USING DEBUG OVERRIDES !!!!!!!!!!!!!!");
	std::string v = PrefsDb::instance()->getPref(".sysservice-dbg-time-tzvalid");
	if (strcasecmp(v.c_str(),"true") == 0)
		tzvalid = true;
	else if (strcasecmp(v.c_str(),"false") == 0)
		tzvalid = false;

	qDebug("tzvalid <--- %s", (tzvalid ? "true" : "false"));
}

//static
void TimePrefsHandler::dbg_time_dstvalidOverride(bool& dstvalid)
{
	if (PrefsDb::instance()->getPref(".sysservice-dbg-time-debugEnable") != "true")
		return;

	PMLOG_TRACE("!!!!!!!!!!!!!!! USING DEBUG OVERRIDES !!!!!!!!!!!!!!");
	std::string v = PrefsDb::instance()->getPref(".sysservice-dbg-time-dstvalid");
	if (strcasecmp(v.c_str(),"true") == 0)
		dstvalid = true;
	else if (strcasecmp(v.c_str(),"false") == 0)
		dstvalid = false;

	qDebug("dstvalid <--- %s", (dstvalid ? "true" : "false"));
}

/*!
\page com_palm_systemservice_time
\n
\section com_palm_systemservice_time_set_time_with_ntp setTimeWithNTP

\e Public.

com.webos.service.systemservice/time/setTimeWithNTP

Set system time with NTP.

\subsection com_palm_systemservice_time_set_time_with_ntp_syntax Syntax:
\code
{
}
\endcode

\subsection com_palm_systemservice_time_get_ntp_time_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/time/setTimeWithNTP '{}'
\endcode
*/
//static
bool TimePrefsHandler::cbSetTimeWithNTP(LSHandle* lsHandle, LSMessage *message,
										void *user_data)
{
	LSMessageJsonParser parser( message, STRICT_SCHEMA(
		PROPS_1(
			WITHDEFAULT(source, string, "unknown")
		)
	));

	ESchemaErrorOptions schErrOption = Settings::instance()->schemaValidationOption;
	if (!parser.parse(__FUNCTION__, lsHandle, schErrOption))
		return true;

	const char* str = parser.getPayload();

	PmLogInfo(sysServiceLogContext(), "REQUEST_NTP_SYNC", 1,
		PMLOGKS("SENDER", LSMessageGetSenderServiceName(message)),
		"/time/setTimeWithNTP received with %s",
		str
	);

	if ( !str )
	{
		PmLogDebug(sysServiceLogContext(), "Received LSMessage with NULL payload (in call)");
		return false;
	}

	PmLogDebug(sysServiceLogContext(), "received message %s", parser.getPayload());

	TimePrefsHandler* th = (TimePrefsHandler*) user_data;

	// category associated with this callback should be registered correctly
	assert( th );

	// it's an actual event...
	th->updateSystemTime();

	// schedule another
	th->setPeriodicTimeSetWakeup();

	// simple response
	LSError lsError;
	LSErrorInit(&lsError);
	if (!LSMessageReply(lsHandle, message, "{\"returnValue\":true}", &lsError))
	{
		PmLogError(sysServiceLogContext(), "LSMESSAGEREPLY_FAILURE",
				   1, PMLOGKS("MESSAGE", lsError.message),
				   "LSMessageReply failed");
		LSErrorFree(&lsError);
		return false;
	}

	return true;
}

//static
bool TimePrefsHandler::cbSetPeriodicWakeupAlarmDResponse(LSHandle* lsHandle, LSMessage *message,
							void *user_data)
{
	const char* str = LSMessageGetPayload(message);
	if ( !str )
	{
		PmLogDebug(sysServiceLogContext(), "Received LSMessage with NULL payload (in reply to call)");
		return false;
	}

	// {"returnValue": boolean}
	JsonMessageParser parser( str, STRICT_SCHEMA(
		PROPS_4(
			PROPERTY(key, string),
			PROPERTY(returnValue, boolean),
			PROPERTY(errorCode, integer),
			PROPERTY(errorText, string)
		)

		REQUIRED_1(returnValue)
	));

	if (!parser.parse(__FUNCTION__))
		return false;

	PmLogDebug(sysServiceLogContext(), "received message %s", str);

	TimePrefsHandler* th = (TimePrefsHandler*) user_data;

	// call associated with this callback should be sent correctly
	assert( th );

	bool returnValue;
	bool getOk = parser.get("returnValue", returnValue);
	assert( getOk ); // schema validation should ensure type and presence

	if (!returnValue)
	{
		std::string errorText = "(none)";
		(void) parser.get("errorText", errorText);
		PmLogDebug(sysServiceLogContext(), "Error received in wakeup alarmd response %s", errorText.c_str());
	}

	//this is a response to a call...
	th->m_sendWakeupSetToAlarmD = !returnValue;
	//if it was true, then the call succeeded so supress sending it again if the service disconnects+reconnects (by setting the m_sendWakeupSetToAlarmD to false)

	// no need to reply on response to call
	return true;
}

bool TimePrefsHandler::cbServiceStateTracker(LSHandle* lsHandle, LSMessage *message,
											 void *user_data)
{
	TimePrefsHandler * th = (TimePrefsHandler *)user_data;

	if (th == NULL)
	{
		qCritical() << "user_data passed as NULL!";
		return true;
	}

	// {"serviceName": string, "connected": boolean}
	LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_2(PROPERTY(serviceName, string),
															  PROPERTY(connected, boolean))
													  REQUIRED_2(serviceName, connected)));

	if (!parser.parse(__FUNCTION__, lsHandle, Settings::instance()->schemaValidationOption))
		return true;

	JValue root = parser.get();

	std::string serviceName = root["serviceName"].asString();
	bool isConnected = root["connected"].asBool();

	LS::Error error;
	if (serviceName == "com.webos.service.alarm")
	{
		if ((isConnected) && (th->m_sendWakeupSetToAlarmD))
		{
			//alarmD is connected, and the flag is set to schedule a periodic wakeup for NTP
			th->setPeriodicTimeSetWakeup();
		}
	}
	else if (serviceName == "com.webos.service.telephony" && isConnected)
	{
		(void) LSCallOneReply(th->getServiceHandle(), "luna://com.webos.service.telephony/platformQuery", "{}",
							  cbTelephonyPlatformQuery, th, NULL, error);
	}

	return true;
}

/*!
\page com_palm_systemservice_time
\n
\section com_palm_systemservice_time_get_system_time getSystemTime

\e Public.

com.webos.service.systemservice/time/getSystemTime

Get system time.

\subsection com_palm_systemservice_time_get_system_time_syntax Syntax:
\code
{
}
\endcode

\subsection com_palm_systemservice_time_get_system_time_returns Returns:
\code
{
   "utc" : int,
   "localtime" : {
	  "year"   : int,
	  "month"  : int,
	  "day"    : int,
	  "hour"   : int,
	  "minute" : int,
	  "second" : int
   },
   "offset"       : int,
   "timezone"     : string,
   "TZ"           : string,
   "timeZoneFile" : string,
   "NITZValid"    : boolean
}
\endcode

\param utc The number of milliseconds since Epoch (midnight of January 1, 1970 UTC), aka - Unix time.
\param localtime Object, see fields below.
\param year The year, i.e., 2009.
\param month The month, 1-12.
\param day The day, 1-31
\param hour The hour, 0-23
\param minute The minute, 0-59
\param second The second, 0-59
\param offset The number of minutes from UTC. This can be negative for time zones west of UTC and positive for time zones east of UTC.
\param timezone The current system time zone. It has the same format as the " TZ " environment variable. For information, see http://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html .
\param TZ The time zone abbreviation in standard Unix format that corresponds to the current time zone (e.g., PDT (Pacific Daylight Time)).
\param timeZoneFile Path to file with Linux zone information file for the currently set zone. For more information, see: http://linux.die.net/man/5/tzfile
\param NITZValid Deprecated. Formerly used to alert the UI whether or not it managed to set the time correctly using NITZ. Currently, it does not indicate anything meaningful

\subsection com_palm_systemservice_time_get_system_time_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/time/getSystemTime {}
\endcode

Example response:
\code
{
	"utc": 1346149236,
	"localtime": {
		"year": 2012,
		"month": 8,
		"day": 28,
		"hour": 3,
		"minute": 20,
		"second": 36
	},
	"offset": -420,
	"timezone": "America\/Los_Angeles",
	"TZ": "PDT",
	"timeZoneFile": "\/var\/luna\/preferences\/localtime",
	"NITZValid": true
}
\endcode
*/
//static
bool TimePrefsHandler::cbGetSystemTime(LSHandle* lsHandle, LSMessage *message,
									   void *user_data)
{
	LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_1(PROPERTY(subscribe, boolean))));
	if (!parser.parse(__FUNCTION__,lsHandle, EValidateAndErrorAlways))
        {
            return true;
        }
	TimePrefsHandler* th = (TimePrefsHandler*) user_data;
	JObject reply;

	do {
		if (LSMessageIsSubscription(message))
		{
			LS::Error error;
			bool retVal = LSSubscriptionAdd(lsHandle,"getSystemTime", message, error);
			if (!retVal)
			{
				reply = JObject {{"subscribed", false},
								 {"returnValue", false},
								 {"errorCode", 1},
								 {"errorText", error.what()}};
				break;
			}
			else
				reply = JObject {{"subscribed", true}};
		}

		reply.put("returnValue", true);
		th->attachSystemTime(reply);
		reply.put("timestamp", ClockHandler::timestampJson());

	} while (false);

	//**DEBUG validate for correct UTF-8 output
	if (!g_utf8_validate(reply.stringify().c_str(), -1, NULL))
	{
		qWarning() << "bus reply fails UTF-8 validity check! [" << reply.stringify().c_str() << "]";
	}

	std::cerr << "Result: " << reply.stringify().c_str() << std::endl;
	LS::Error error;
	(void) LSMessageReply(lsHandle, message, reply.stringify().c_str(), error);

	return true;
}

/*!
\page com_palm_systemservice_time
\n
\section com_palm_systemservice_time_get_system_timezone_file getSystemTimezoneFile

\e Public.

com.webos.service.systemservice/time/getSystemTimezoneFile

Get the path to Linux zone information file for the currently set zone. For more information, see: http://linux.die.net/man/5/tzfile

\subsection com_palm_systemservice_time_get_system_timezone_file_syntax Syntax:
\code
{
}
\endcode

\subsection com_palm_systemservice_time_get_system_timezone_file_returns Returns:
\code
{
	"timeZoneFile": string,
	"subscribed": boolean
}
\endcode

\param timeZoneFile Path to system timezone file.
\param subscribed Always false.

\subsection com_palm_systemservice_time_get_system_time_zone_file_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/time/getSystemTimezoneFile {}
\endcode

Example response
\code
{
	"timeZoneFile": "\/var\/luna\/preferences\/localtime",
	"subscribed": false
}
\endcode
*/
//static
bool TimePrefsHandler::cbGetSystemTimezoneFile(LSHandle* lsHandle, LSMessage *message,
											   void *)
{
	EMPTY_SCHEMA_RETURN(lsHandle, message);

	JObject reply {{"timeZoneFile", s_tzFilePath},
				   {"subscribed", false}};	//no subscriptions on this; make that explicit!


	LS::Error error;
	(void) LSMessageReply(lsHandle, message, reply.stringify().c_str(), error);

	return true;
}

/*!
\page com_palm_systemservice_time
\n
\section com_palm_systemservice_time_set_time_change_launch setTimeChangeLaunch

com.webos.service.systemservice/time/setTimeChangeLaunch

Add an application to, or remove it from the timeChangeLaunch list.

You can check what's on the list with:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/getPreferences '{"subscribe": false, "keys":["timeChangeLaunch"]}'
\endcode

\subsection com_palm_systemservice_time_set_time_change_launch_syntax Syntax:
\code
{
	"appId": string,
	"active": boolean
	"parameters": object
}
\endcode

\param appId Application ID. required.
\param active If true, adds the application to the launch list. If false, the application is removed. True by default.
\param parameters Launch parameters for the application.

\subsection com_palm_systemservice_time_set_time_change_launch_returns Returns:
\code
{
	"subscribed": boolean,
	"errorText": string,
	"returnValue": boolean
}
\endcode

\param subscribed Always false.
\param errorText Description of the error if call was not successful.
\param returnValue Indicates if the call was successful.

\subsection com_palm_systemservice_time_set_time_change_launch_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/time/setTimeChangeLaunch
	'{ "appId": "com.webos.service.app.someApp", "active": true, "parameters": { "param1": "foo", "param2": "bar" } } }'
\endcode

Example response for a successful call:
\code
{
	"subscribed": false,
	"returnValue": true
}
\endcode

Example response for a failed call:
\code
{
	"subscribed": false,
	"errorText": "missing required parameter appId",
	"returnValue": false
}
\endcode
*/
//static
bool TimePrefsHandler::cbSetTimeChangeLaunch(LSHandle* lsHandle, LSMessage *message,
											 void *user_data)
{
	bool retVal;
	JValue tmpJson_listArray;
	JValue storedJson_listObject;

	JValue label;
	std::string errorText;
	std::string rawCurrentPref;

	//format:  { "appId":<string; REQ>, "active":<boolean; REQ> , "parameters":<json object>; REQ> }
	LSMessageJsonParser parser(message, RELAXED_SCHEMA(
										   PROPS_3(PROPERTY(appId, string),
												   PROPERTY(active, boolean),
												   PROPERTY(parameters, object))
										   REQUIRED_3(appId, active, parameters)));

	if (!parser.parse(__FUNCTION__, lsHandle, EValidateAndErrorAlways))
		return true;

	JValue root = parser.get();

	std::string appId = root["appId"].asString();
	bool active = root["active"].asBool();
	JValue params = root["parameters"];

	/*
	 *
	 * Format of the stored app launch list
	 *
	 * {"launchList":[ { "appId":"com.webos.service.app.x", "parameters":{} }, { "appId":"com.webos.service.app.y", "parameters":{} },... ]}
	 *
	 */

	//get the currently stored list of launch apps
	rawCurrentPref = PrefsDb::instance()->getPref("timeChangeLaunch");
	JValue storedJson = JDomParser::fromString(rawCurrentPref);
	if (!storedJson.isValid()) {
		storedJson = JObject();
	}

	//get the launchList array object out of it
	JValue storedJson_listArray = storedJson["launchList"];
	if (!storedJson_listArray.isValid() && active) {
		//nothing in the list yet, and I need to add...
		storedJson_listArray = JArray();
		storedJson.put("launchList", storedJson_listArray);
	}
	else if (!storedJson_listArray.isValid() && !active) {
		//nothing in the list yet, and I was told to remove; nothing to do, so just go to Done
		errorText = "cannot deactivate an appId that isn't in the list";
		goto Done;
	}
	else if (storedJson_listArray.isValid() && !active) {
		//list exists, and I have to remove
		goto Remove;
	}
	//else, Add
	goto Add;		//making it explicit

Add:
	//go through the array and try and find the appid (if it's in there already)
	for (const JValue key: storedJson_listArray.items()) {

		label = key["appId"];
		if (!label.isString()) {
			continue; //something really bad happened; something was stored in the list w/o an appId!
		}
		std::string foundAppId = label.asString();
		if (appId == foundAppId)
		{
			storedJson_listObject = key;
			break;
		}
	}

	if (storedJson_listObject.isValid() && !storedJson_listObject.isNull()) {
		//found it...
		//json_object_object_add(storedJson_listObject,(char *)"parameters",json_object_new_string(params.c_str()));
		storedJson_listObject.put("parameters", params);
	}
	else {
		//create a new object, populate, and add it to the array
		storedJson_listObject = JObject {{"appId", appId}, {"parameters", params}};
		storedJson_listArray.append(storedJson_listObject);
	}

	goto Store;

Remove:

	tmpJson_listArray = JArray();
	for (const JValue key: storedJson_listArray.items()) {

		label = key["appId"];
		if (!label.isString()) {
			continue; //something really bad happened; something was stored in the list w/o an appId!
		}
		std::string foundAppId = label.asString();
		if (appId == foundAppId)
			continue;
		//else add to the new array
		tmpJson_listArray.append(key);
	}

	//replace the old array with the new one in the storedJson object
	storedJson.put("launchList", tmpJson_listArray);
	//TODO: verify that this object add that replaced the json array that was in the object already, actually deallocated the old json array

	//...proceed right below to Store
	goto Store;

Store:
	//store the pref back, in string form
	rawCurrentPref = storedJson.stringify();
	PrefsDb::instance()->setPref("timeChangeLaunch",rawCurrentPref.c_str());

Done:
	JObject jsonOutput {{"subscribed", false}};	//no subscriptions on this; make that explicit!
	if (errorText.size()) {
		jsonOutput.put("errorText", errorText);
		jsonOutput.put("returnValue", false);
		qWarning() << errorText.c_str();
	}
	else
		jsonOutput.put("returnValue", true);

	LS::Error error;
	retVal = LSMessageReply(lsHandle, message, jsonOutput.stringify().c_str(), error);

	return true;
}

/*!
\page com_palm_systemservice_time
\n
\section com_palm_systemservice_time_get_ntp_time getNTPTime

\e Public.

com.webos.service.systemservice/time/getNTPTime

Get NTP time.

\subsection com_palm_systemservice_time_get_ntp_time_syntax Syntax:
\code
{
}
\endcode

\subsection com_palm_systemservice_time_get_ntp_time_returns Returns:
\code
{
	"subscribed": boolean,
	"returnValue": true,
	"utc": int
}
\endcode

\param subscribed Always false.
\param returnValue Indicates if the call was succesful.
\param utc The number of milliseconds since Epoch (midnight of January 1, 1970 UTC), aka - Unix time.

\subsection com_palm_systemservice_time_get_ntp_time_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/time/getNTPTime '{ }'
\endcode

Example response for a succesful call:
\code
{
	"subscribed": false,
	"returnValue": true,
	"utc": 1346156673
}
\endcode
*/
bool TimePrefsHandler::cbGetNTPTime(LSHandle* lsHandle, LSMessage *message,
							void *user_data)
{
	TimePrefsHandler *th = static_cast<TimePrefsHandler*>(user_data);
	PmLogInfo(sysServiceLogContext(), "REQUEST_NTP_TIME", 2,
		PMLOGKS("SENDER", LSMessageGetSenderServiceName(message)),
		PMLOGKFV("ALLOWED", "%s", th->isNTPAllowed() ? "true" : "false"),
		"/time/getNTPTime received with %s", LSMessageGetPayload(message)
	);

	if (th->isNTPAllowed())
	{
		return th->m_ntpClock.requestNTP(message);
	}
	else
	{
		const char * const denyReply = "{\"subscribed\":false,"
									   "\"returnValue\":false,"
									   "\"errorText\":\"NTP requests are prohibited at the moment\"}";

		PmLogWarning(sysServiceLogContext(), "NTP_REQUEST_DENY", 0,
			"Got NTP request while it is not allowed"
		);

		LS::Error error;
		if (!LSMessageRespond(message, denyReply, error.get()))
		{
			PmLogError(sysServiceLogContext(), "NTP_DENY_RESPOND_FAIL", 1,
				PMLOGKS("REASON", error.what()),
				"Failed to send response for NTP query call"
			);
			return false;
		}
		else
		{
			return true;
		}
	}
}

/*!
\page com_palm_systemservice_time
\n
\section com_palm_systemservice_time_convert_date convertDate

\e Public.

com.webos.service.systemservice/time/convertDate

Converts a date from one timezone to another.

\subsection com_palm_systemservice_time_convert_date_syntax Syntax:
\code
{
	"date": string,
	"source_tz": string,
	"dest_tz": string
}
\endcode

\param date Date to convert as a string in format: "Y-m-d H:M:S". Required.
\param source_tz Source timezone. Required.
\param dest_tz Destination timezone. Required.

\subsection com_palm_systemservice_time_convert_date_returns Returns:
\code
{
	"returnValue": boolean,
	"date": string,
	"errorText": string
}
\endcode

\param returnValue Indicates if the call was succesful.
\param date The date in the new destination timezone if call was succesful.
\param errorText Description of the error if call was not succesful.

\subsection com_palm_systemservice_time_convert_date_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/time/convertDate '{ "date": "1982-12-06 17:25:33", "source_tz": "America/Los_Angeles", "dest_tz":"America/New_York" }'
\endcode

Example response for a succesful call:
\code
{
	"returnValue": true,
	"date": "Mon Dec  6 20:25:33 1982\n"
}
\endcode

Example response for a failed call:
\code
{
	"returnValue": false,
	"errorText": "timezone not found: 'Finland'"
}
\endcode
*/
bool TimePrefsHandler::cbConvertDate(LSHandle* pHandle, LSMessage* pMessage, void*)
{
	Utils::gstring status {nullptr};
	Utils::gstring error_text {nullptr};
	bool ret = false;
	struct tm local_tm;
	char * bad_char = NULL;

	// {"date": string, "source_tz": string, "dest_tz": string}
	LSMessageJsonParser parser(pMessage, STRICT_SCHEMA(PROPS_3(PROPERTY(date, string),
															  PROPERTY(source_tz, string),
															  PROPERTY(dest_tz, string))
													  REQUIRED_3(date, source_tz, dest_tz)));

	if (!parser.parse(__FUNCTION__, pHandle, EValidateAndErrorAlways))
		return true;

	JValue root = parser.get();

	do
	{

		std::string date = root["date"].asString();
		std::string source_tz = root["source_tz"].asString();
		std::string dest_tz = root["dest_tz"].asString();

		qDebug("%s: converting %s from %s to %s", __func__, date.c_str(), source_tz.c_str(), dest_tz.c_str());

		bad_char = (char *) strptime(date.c_str(), "%Y-%m-%d %H:%M:%S", &local_tm);
		if (NULL == bad_char) {
			error_text = g_strdup_printf("unrecognized date format: '%s'", date.c_str());
			break;
		} else if (*bad_char != '\0') {
			error_text = g_strdup_printf("unrecognized characters in date: '%s'", date.c_str());
			break;
		}

		if (!tz_exists(source_tz.c_str())) {
			error_text = g_strdup_printf("timezone not found: '%s'", source_tz.c_str());
			break;
		}

		if (!tz_exists(dest_tz.c_str())) {
			error_text = g_strdup_printf("timezone not found: '%s'", dest_tz.c_str());
			break;
		}

		set_tz(source_tz.c_str());

		time_t local_time;
		local_time = mktime(&local_tm);
		// ctime adds '\n' to the end of the result, so we need a little workaround
		std::string str_time = ctime(&local_time);
		str_time.pop_back();
		qDebug("0 date='%s' ctime='%s' local_time=%ld timezone=%ld", date.c_str(), str_time.c_str(),
			   local_time, timezone);

		if (!tz_exists(dest_tz.c_str())) {
			error_text = g_strdup_printf("timezone not found: '%s'", dest_tz.c_str());
		}

		set_tz(dest_tz.c_str());
		qDebug("1 date='%s' ctime='%s' local_time=%ld timezone=%ld", date.c_str(), str_time.c_str(),
			   local_time, timezone);

		g_assert(error_text.get() == nullptr);
		status = g_strdup_printf("{\"returnValue\":true,\"date\":\"%s\"}", str_time.c_str());
	}
	while (false);

	if (status.get() == nullptr) {
		g_assert(error_text.get() != nullptr);
		status = g_strdup_printf("{\"returnValue\":false,\"errorText\":\"%s\"}", error_text.get());
		qWarning() << error_text.get();
	}

	LS::Error error;
	ret = LSMessageReply(pHandle, pMessage, status.get(), error);
	if (!ret)
	{
		LSREPORT(*error.get());
	}

	return ret;
}

/*!
\page com_palm_systemservice_time
\n
\section com_palm_systemservice_time_get_system_uptime getSystemUptime

\e Public.

com.webos.service.systemservice/time/getSystemUptime

Get system uptime

\subsection com_palm_systemservice_time_get_system_uptime Syntax:
\code
{}
\endcode

\subsection com_palm_systemservice_time_get_system_uptime Returns:
\code
{
	"returnValue": boolean,
	"uptime": int,
	"errorCode": int,
	"errorText": string
}
\endcode

\param returnValue Indicates if the call was succesful
\param uptime Seconds since system start
\param errorCode Error code. This is value from errno
\param errorText Error description

\subsection com_palm_systemservice_time_get_system_uptime Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/time/getSystemUptime '{ }'
\endcode

Example response for a succesful call:
\code
{
	"returnValue": true,
	"uptime":123456789
}
\endcode

Example response for a failed call:
\code
{
	"returnValue": false,
	"errorCode": 1,
	"errorText": "unexpected error"
}
\endcode
*/

bool TimePrefsHandler::getSystemUptime(LSHandle* pHandle, LSMessage* message, void*)
{
	JObject reply;

	struct sysinfo s_info;
	if(sysinfo(&s_info) == 0)
	{
		reply.put("returnValue", true);
		reply.put("uptime", int(s_info.uptime));
	}
	else
	{
		int errno_back = errno;
		reply.put("errorCode", errno_back);
		reply.put("returnValue", false);
		reply.put("errorText", strerror(errno_back));
	}

	LS::Error error;
	if (!LSMessageReply(pHandle, message, reply.stringify().c_str(), error))
		qWarning() << error.what();

	return true;
}

//static
gboolean TimePrefsHandler::source_periodic(gpointer userData)
{
	if (TimePrefsHandler::s_inst == NULL)
	{
		qWarning() << "instance handle is NULL!";
		return FALSE;
	}
	int rc = TimePrefsHandler::s_inst->timeoutFunc();

	if (rc == TIMEOUTFN_RESETCYCLE)
	{
		qDebug("Repeating timeout cycle");
		return TRUE;
	}
	else if (rc == TIMEOUTFN_ENDCYCLE)
	{
		qDebug("Ending timeout cycle");
		return FALSE;
	}

	qWarning("fall through! (rc %d)",rc);
	return TRUE;		///fall through! should never happen. if it does, repeat the cycle
}

//static
void TimePrefsHandler::source_periodic_destroy(gpointer userData)
{
	//tear down the timeout source
	if (TimePrefsHandler::s_inst == NULL)
	{
		 qWarning() << "instance handle is NULL!";
		return;
	}
	TimePrefsHandler::s_inst->timeout_destroy(userData);
}

void TimePrefsHandler::updateTimeZoneEnv()
{
	const char* tzName = (m_cpCurrentTimeZone->name).c_str();

	__qMessage("Setting Time Zone: %s, utc Offset: %d",
			tzName, m_cpCurrentTimeZone->offsetToUTC);
	tzsetWorkaround(tzName);
	__qMessage("TZ env is now [%s]", getenv("TZ"));
}

/*!
\page com_palm_systemservice_time
\n
\section com_palm_systemservice_time_launch_time_change_apps launchTimeChangeApps

\e Public.

com.webos.service.systemservice/time/launchTimeChangeApps

Launch all applications on the timeChangeLaunch list. You can check what's on the list with:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/getPreferences '{"subscribe": false, "keys":["timeChangeLaunch"]}'
\endcode


\subsection com_palm_systemservice_time_launch_time_change_apps_syntax Syntax:
\code
{
}
\endcode

\subsection com_palm_systemservice_time_launch_time_change_apps_returns Returns:
\code
{
	"subscribed": boolean,
	"returnValue": boolean
}
\endcode

\param subscribed Always false.
\param returnValue Always true.

\subsection com_palm_systemservice_time_launch_time_change_apps_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/time/launchTimeChangeApps '{}'
\endcode

Example response:
\code
{
	"subscribed": false,
	"returnValue": true
}
\endcode
*/
//static
bool TimePrefsHandler::cbLaunchTimeChangeApps(LSHandle* lsHandle, LSMessage *message,
											  void *user_data)
{
	EMPTY_SCHEMA_RETURN(lsHandle, message);

	TimePrefsHandler * th = (TimePrefsHandler *)user_data;
	if (th == NULL)
		return false;

	th->launchAppsOnTimeChange();

	JObject reply {{"subscribed", false},	//no subscriptions on this; make that explicit!
				   {"returnValue", true}};


	LS::Error error;
	(void ) LSMessageReply(lsHandle, message, reply.stringify().c_str(), error);

	return true;
}

//necessary because tzset() apparently doesn't re-set if the path to a timezone file is the same as the old path that was set)
static void tzsetWorkaround(const char * newTZ) {

	setenv("TZ","",1);
	tzset();
	sleep(1);
	setenv("TZ",newTZ, 1);
	tzset();
}

/*
 *
 *					NTP
 *
 */

std::list<std::string> TimePrefsHandler::getTimeZonesForOffset(int offset)
{
	std::list<std::string> timeZones;

	// All timezones wih matching offset
	std::pair<TimeZoneMultiMapConstIterator, TimeZoneMultiMapConstIterator> iterPair
		= m_offsetZoneMultiMap.equal_range(offset);

	for (TimeZoneMultiMapConstIterator iter = iterPair.first; iter != iterPair.second; ++iter)
		timeZones.push_back(iter->second->name);

	return timeZones;
}

void TimePrefsHandler::slotNetworkConnectionStateChanged(bool connected)
{
	PMLOG_TRACE("connected: %d", connected);
	if (!connected)
		return;

	if (!isNITZTimeEnabled())
		return;

	std::string interval = PrefsDb::instance()->getPref(".sysservice-time-autoNtpInterval");
	uint32_t timev = strtoul(interval.c_str(),NULL,10);
	if ((timev < 300) || (timev > 86400))
		timev = 86399; //24 hour default (23h.59m.59s actually)

	time_t currTime = currentStamp();
	qDebug("currTime: %d, lastNtpUpdate: %d, interval: %d",
		   (int)currTime, (int)m_lastNtpUpdate, timev);
	if ((m_lastNtpUpdate > 0) && (time_t)(m_lastNtpUpdate + timev) > currTime)
		return;

	PMLOG_TRACE("startBootstrapCycle");
	startBootstrapCycle(0);
}

bool TimePrefsHandler::cbTelephonyPlatformQuery(LSHandle* lsHandle, LSMessage *message,
												void *userData)
{
	// {"extended": string, "capabilities": string, "hfenable": boolean}
	bool isSuccess = false;

	do {
		const char* str = LSMessageGetPayload(message);
		if(!str) break;

		JValue root = JDomParser::fromString(str);

		JValue label = root["extended"];
		if (!label.isValid()) break;

		label = root["capabilities"];
		if (!label.isValid()) break;

		label = root["hfenable"];
		if (!label.isBoolean()) break;

		bool timeZoneAvailable = label.asBool();

		TimePrefsHandler* th = (TimePrefsHandler*) userData;
		th->m_nitzTimeZoneAvailable = timeZoneAvailable;

		qDebug("NITZ Time Zone Available: %d", timeZoneAvailable);
		isSuccess = true;
	} while(false);

	return isSuccess;
}

void TimePrefsHandler::updateDriftPeriod(const std::string hrValue)
{
	char* endptr = 0;
	long int val = strtol(hrValue.c_str(), &endptr, 10);
	if ( (*endptr != 0) || val < -1 || val > (24*30) /* Max 1 month is enough */)
	{
		PmLogInfo(sysServiceLogContext(), "INVALID_SYNC_PERIOD", 2,
				PMLOGKS("user", hrValue.c_str()),
				PMLOGKFV("default", "%ld", static_cast<long int>(m_driftPeriodDefault)),
				"Invalid time synchronization period. Default is used");

		m_driftPeriod = (time_t)m_driftPeriodDefault;
	}
	else
	{
		m_driftPeriod = (time_t)(val < 0 ? m_driftPeriodDisabled : (val*60*60));
	}
}

void TimePrefsHandler::clockChanged(const std::string &clockTag, int priority, time_t systemOffset, time_t lastUpdate)
{
	if (clockTag == ClockHandler::micom)
	{
		// micom isn't a real time-source it is rather a cell where some other time is hold

		std::string effectiveClockTag;
		// We've received micom time which actually stores time
		// synchronized with lastSystemTimeSource.
		if (!PrefsDb::instance()->getPref("lastSystemTimeSource", effectiveClockTag))
		{
			// if no lastSystemTimeSource were set before assume factory
			effectiveClockTag = s_factoryTimeSource;
		}

		// just simulate that we've received update from effectiveClockTag
		// instead of normal processing but with time that refers to the
		// remembered micom time (boot time if not set)
		time_t timeStamp = m_micomTimeStamp != (time_t)-1 ? m_micomTimeStamp
														  : bootStart();
		if(isManualTimeUsed())
		{
			compensateSuspendedTimeToClocks.fire(systemOffset, timeStamp);
		}

		deprecatedClockChange.fire(systemOffset, effectiveClockTag, timeStamp);


		return;
	}

	int effectivePriority = priority;

	if (isManualTimeUsed())
	{
		if (clockTag == ClockHandler::manual)
		{
			PmLogDebug(sysServiceLogContext(),
					   "In manual mode priority for user time source (%d) treated as %d",
					   priority, INT_MAX);
			effectivePriority = INT_MAX; // override everything
		}
		else
		{
			saveAlternativeFactorySource(effectivePriority, systemOffset, lastUpdate);

			if ( getMicomAvailable() == false )
			{
				PmLogInfo(sysServiceLogContext(), "MICOM_NOT_AVAILABLE", 4,
					PMLOGKFV("MICOM_STATUS","%d", getMicomAvailable()),
					PMLOGKS("CLOCK_TAG", clockTag.c_str()),
					PMLOGKFV("PRIORITY", "%d", priority),
					PMLOGKFV("UTC_OFFSET", "%ld", systemOffset),
					"In manual mode, if micom status is not available then apply pre-saved manual source information");

					applyAlternativeFactorySource();
			}
			else
			{
				PmLogInfo(sysServiceLogContext(), "IGNORE_AUTO_CLOCK", 2,
					PMLOGKS("SOURCE", clockTag.c_str()),
					PMLOGKFV("PRIORITY", "%d", priority),
					"In manual mode, if micom status is available then ignore external time sources like sdp, broadcast, and so on");
			}
			return;
		}
	}
	else if (!isNTPAllowed() && clockTag == "ntp")
	{
		PmLogWarning(sysServiceLogContext(), "NTP_SYNC_DENY", 0,
			"NTP clock source is masked. Ignoring synchronization with system time.");
		return;
	}

	time_t currentTime = time(0);

	// note that we only allow to increase priority or re-sync time if we
	// already passed through nextSyncTime
	if ( effectivePriority < m_currentTimeSourcePriority &&
			(isDriftPeriodDisabled() || currentTime < m_nextSyncTime) )
	{
		PmLogInfo(sysServiceLogContext(), "IGNORE_WORSE_CLOCK", 4,
				  PMLOGKS("SOURCE", clockTag.c_str()),
				  PMLOGKFV("PRIORITY", "%d", priority),
				  PMLOGKFV("HIGHER_PRIORITY", "%d", m_currentTimeSourcePriority),
				  PMLOGKFV("UTC_OFFSET", "%ld", systemOffset),
				  "Ignoring time-source with lower priority");
		return;
	}

	PmLogInfo(sysServiceLogContext(), "APPLY_CLOCK", 4,
			  PMLOGKS("SOURCE", clockTag.c_str()),
			  PMLOGKFV("PRIORITY", "%d", priority),
			  PMLOGKFV("CURRENT_PRIORITY", "%d", m_currentTimeSourcePriority),
			  PMLOGKFV("UTC_OFFSET", "%ld", systemOffset),
			  "Applying time from time-source update");

	// so we actually going to apply this update to our system time
	// or keep it the same if offset is zero
	if (systemSetTime(systemOffset, clockTag))
	{
		m_currentTimeSourcePriority = priority;
		// note that lastUpdate is outdated already so we need to adjust it
		m_nextSyncTime = lastUpdate + systemOffset + getDriftPeriod(); // when we should sync our time again

		PmLogInfo(sysServiceLogContext(), "SYSTEM_TIME_UPDATED", 3,
				  PMLOGKS("SOURCE", clockTag.c_str()),
				  PMLOGKFV("PRIORITY", "%d", m_currentTimeSourcePriority),
				  PMLOGKFV("NEXT_SYNC", "%ld", m_nextSyncTime),
				  "Updated system time");
	}
}

JValue TimePrefsHandler::getTimeZoneByLocale(std::string& locale)
{
	JValue root = JDomParser::fromString(m_cpCurrentTimeZone->jsonStringValue);

	TimeZoneInfo tzInfo;
	if (TZJsonHelper::extract(root, &tzInfo)) {
                if(Settings::instance()->useLocalizedTZ) {
		        std::unique_ptr<ResBundle> resBundle = std::unique_ptr<ResBundle>(new ResBundle(locale, s_file, s_resources_path));
		        tzInfo.description = resBundle->getLocString(tzInfo.description);
		        tzInfo.city = resBundle->getLocString(tzInfo.city);
		        tzInfo.country = resBundle->getLocString(tzInfo.country);
                }
		return TZJsonHelper::pack(&tzInfo);
	}

	return JValue();
}

bool TimePrefsHandler::cbTimeZoneByLocale(LSHandle* lsHandle, LSMessage *message, void *user_data)
{
	bool success(false);
	JValue replyRoot = pbnjson::Object();
	std::string reply(""), locale("");

	do {
		TimePrefsHandler* th = (TimePrefsHandler*)user_data;

		const char* payload = LSMessageGetPayload(message);
		if (!payload) break;

		JValue root = JDomParser::fromString(payload);
		if (!root.isObject()) {
			break;
		}

		JValue label = root["locale"];
		if (!label.isString()) break;

		locale = label.asString();
		JValue timeZoneObj = th->getTimeZoneByLocale(locale);
		if(!timeZoneObj.isNull())
		{
			replyRoot.put("returnValue", true);
			replyRoot.put("timeZone", timeZoneObj);
			reply = replyRoot.stringify();
			success = true;
		}
	} while(false);

	if (!success) {
		if(locale.empty()) {
			reply = "{\"errorText\":\"'locale' parameter missing\",\"returnValue\":false}";
		} else {
			reply = "{\"returnValue\":false}";
		}
	}

	LSError lsError;
	LSErrorInit(&lsError);
	if (!LSMessageReply(lsHandle, message, reply.c_str(), &lsError)) {
		LSErrorFree (&lsError);
	}

	return success;
}

void TimePrefsHandler::saveAlternativeFactorySource(int priority, time_t systemOffset, time_t lastUpdate)
{
	if(priority > m_altFactorySrcPriority)
	{
		// priority ={ sdp: 4 , broadcast-adjusted: 3 , broadcast: 2 , micom: 1 , manual: 0 }
		PmLogInfo(sysServiceLogContext(), "SAVE_ALTERNATIVE_FACTORY_SOURCE", 4,
			PMLOGKFV("PRIORITY","%d", priority),
			PMLOGKFV("ALTERNATIVE_FACTORY_SOURCE_PRIORITY","%d", m_altFactorySrcPriority),
			PMLOGKFV("SYSTEM_OFFSET", "%ld", systemOffset),
			PMLOGKFV("LAST_UPDATE", "%ld", lastUpdate),
			"if newly incoming priority is higher than pre-saved priority then save current auto clock sources");

		m_altFactorySrcPriority = priority;
		m_altFactorySrcSystemOffset = systemOffset;
		m_altFactorySrcLastUpdate = lastUpdate;
		m_altFactorySrcValid = true;
	}
}

void TimePrefsHandler::applyAlternativeFactorySource()
{
	if(m_altFactorySrcValid && ("factory" == m_systemTimeSourceTag))
	{
		PmLogInfo(sysServiceLogContext(), "APPLY_ALTERNATIVE_FACTORY_SOURCE", 4,
			PMLOGKFV("VALID_ALTERNATIVE_FACTORY_SOURCE", "%d", m_altFactorySrcValid),
			PMLOGKFV("ALTERNATIVE_FACTORY_SOURCE_PRIORITY", "%d", m_altFactorySrcPriority),
			PMLOGKFV("CURRENT_PRIORITY", "%d", m_currentTimeSourcePriority),
			PMLOGKS("SYSTEMTIME_SOURCE_TAG", m_systemTimeSourceTag.c_str()),
			"if the flag is changed from true to false then fire(call update())");
		if (systemSetTime(m_altFactorySrcSystemOffset, m_systemTimeSourceTag))
		{
			// note that lastUpdate is outdated already so we need to adjust it
			m_nextSyncTime = m_altFactorySrcLastUpdate + m_altFactorySrcSystemOffset + getDriftPeriod(); // when we should sync our time again

			PmLogInfo(sysServiceLogContext(), "SYSTEM_TIME_UPDATED", 1,
				PMLOGKFV("NEXT_SYNC", "%ld", m_nextSyncTime),
				"Updated system time");
		}
		m_altFactorySrcValid = false;
	}
}

void TimePrefsHandler::handleNotAvailableSource(const std::string& source)
{
	PmLogInfo(sysServiceLogContext(), "HANDLE_NOT_AVAILABLE_SOURCE", 1,
		PMLOGKS("SOURCE", source.c_str()),
		"handle not available source");

	// if source is micom
	if(source.compare("micom") == 0)
	{
		setMicomAvailable(false);
		applyAlternativeFactorySource();
	}

	// TODO: if others(broadcast, sdp, and so on) need to be handled, add handling code here
}

int TimePrefsHandler::enableNetworkTimeSync(bool enable)
{
        std::string useNetworkTime = enable ? "true" : "false";

        std::string command = "timedatectl set-ntp " + useNetworkTime;

        return(system(command.c_str()));
}
