// Copyright (c) 2010-2022 LG Electronics, Inc.
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


#include <glib.h>
#include <signal.h>

#include <luna-service2/lunaservice.h>
#include <luna-service2++/error.hpp>

#include "PrefsDb.h"
#include "PrefsFactory.h"
#include "Logging.h"

#include "SystemRestore.h"
#include "Mainloop.h"
#include "TimeZoneService.h"
#include "OsInfoService.h"
#include "DeviceInfoService.h"

#include "BackupManager.h"
#include "TimePrefsHandler.h"
#include "ClockHandler.h"

#include "Utils.h"
#include "Settings.h"
#include "JSONUtils.h"

static gboolean s_useSysLog = false;

namespace {
	using namespace pbnjson;

	void setupClockHandler(ClockHandler &clockHandler, LSHandle* serviceHandle)
	{
		assert( serviceHandle );

		// Right now we have no reasonable handling for inability to register
		// category with specific methods so just ignore answer.
		// Nevertheless we have some functionality in ClockHandler through
		// which TimePrefsHandler manages system time synchronization
		(void) clockHandler.setServiceHandle(serviceHandle);

		clockHandler.manualOverride(TimePrefsHandler::instance()->isManualTimeUsed());

		// setup properties bindings
		TimePrefsHandler::instance()->systemTimeChanged.connect(&clockHandler, &ClockHandler::adjust);
		TimePrefsHandler::instance()->isManualTimeChanged.connect(&clockHandler, &ClockHandler::manualOverride);
		TimePrefsHandler::instance()->deprecatedClockChange.connectVoid(&clockHandler, &ClockHandler::update);
		TimePrefsHandler::instance()->compensateSuspendedTimeToClocks.connect(&clockHandler, &ClockHandler::compensateSuspendedTimeToClocks);
		clockHandler.clockChanged.connect(TimePrefsHandler::instance(), &TimePrefsHandler::clockChanged);
		clockHandler.notAvailableSourceHandled.connect(TimePrefsHandler::instance(), &TimePrefsHandler::handleNotAvailableSource);

		// setup time sources for clock handler
		int basePriority = 1;
		const TimePrefsHandler::TimeSources &sources = TimePrefsHandler::instance()->timeSources();
		for (size_t i = 0; i < sources.size(); ++i)
		{
			int priority = sources.size()-1 - i + basePriority;
			clockHandler.setup(sources[i], priority);
		}
	}
} // anonymous namespace

static bool cbComPalmImage2Status(LSHandle* lsHandle, LSMessage *message,
                                  void *user_data)
{
	// {"serviceName": string, "connected": boolean}
	LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_2(PROPERTY(serviceName, string),
	                                                          PROPERTY(connected, boolean))
	                                                  REQUIRED_2(serviceName, connected)));

	if (!parser.parse(__FUNCTION__, lsHandle, Settings::instance()->schemaValidationOption)) return true;

	JValue label = parser.get()["connected"];
	Settings::instance()->m_image2svcAvailable = label.asBool();

	return true;
}

static void sendSignals(LSHandle* serviceHandle)
{
	LS::Error error;

	//turn novacom on if requested
	if (Settings::instance()->m_turnNovacomOnAtStartup)
	{
		if (!(LSCall(serviceHandle, "luna://com.webos.service.connectionmanager/setnovacommode",
		             R"({"isEnabled": true, "bypassFirstUse": false})",
		             nullptr, nullptr, nullptr, error)))
		{
			qCritical() << "failed to force novacom to On state";
		}
	}

	if (!LSCall(serviceHandle, "luna://com.webos.service.bus/signal/registerServerStatus",
	            R"({"serviceName": "com.webos.service.image2", "subscribe": true})",
	            cbComPalmImage2Status, nullptr, nullptr, error))
	{
		//non-fatal
		qWarning() << error.what();
	}

	// register for storage daemon signals
	if (!LSCall(serviceHandle, "luna://com.webos.service.bus/signal/addmatch",
	            R"({"category": "/storaged", "method": "MSMAvail"})",
	            SystemRestore::msmAvailCallback, nullptr, nullptr, error))
	{
		qCritical() << error.what();
	}

	if (!LSCall(serviceHandle, "luna://com.webos.service.bus/signal/addmatch",
	            R"({"category": "/storaged", "method": "MSMProgress"})",
	            SystemRestore::msmProgressCallback, nullptr, nullptr, error))
	{
		qCritical() << error.what();
	}

	if (!LSCall(serviceHandle, "luna://com.webos.service.bus/signal/addmatch",
	            R"({"category": "/storaged", "method": "MSMEntry"})",
	            SystemRestore::msmEntryCallback, nullptr, nullptr, error))
	{
		qCritical() << error.what();
	}

	if (!LSCall(serviceHandle, "luna://com.webos.service.bus/signal/addmatch",
	            R"({"category": "/storaged", "method": "MSMFscking"})",
	            SystemRestore::msmFsckingCallback, nullptr, nullptr, error))
	{
		qCritical() << error.what();
	}

	if (!LSCall(serviceHandle, "luna://com.webos.service.bus/signal/addmatch",
	            R"({"category": "/storaged", "method": "PartitionAvail"})",
	            SystemRestore::msmPartitionAvailCallback, nullptr, nullptr, error))
	{
		qCritical() << error.what();
	}
}

void
main_loop_quit() {
	g_main_loop_quit(g_mainloop.get());
}

static void
signal_handler_quit(int signal) {
	main_loop_quit();
}

static inline void
fill_sigaction(struct sigaction *action,
                void (*handler)(int),
                sigset_t mask) {

	bzero((void *)action, sizeof(struct sigaction));
	action->sa_handler = handler;
	action->sa_mask = mask;
	action->sa_flags = 0;
}


static void
init_signals(void) {

    sigset_t sigset;
    struct sigaction ignore_action;
    struct sigaction quit_action;

    sigemptyset(&sigset);

    fill_sigaction(&ignore_action, SIG_IGN, sigset);
    sigaction(SIGHUP, &ignore_action, (struct sigaction *)NULL);

    fill_sigaction(&quit_action, signal_handler_quit, sigset);
    sigaction(SIGTERM, &quit_action, (struct sigaction *)NULL);
    sigaction(SIGINT, &quit_action, (struct sigaction *)NULL);
}


int main(int argc, char ** argv)
{
	setenv("QT_PLUGIN_PATH","/usr/plugins",1);
	setenv("QT_QPA_PLATFORM", "minimal",1);

	g_mainloop.reset(g_main_loop_new(nullptr, false));

	qInstallMessageHandler(outputQtMessages);

	Settings *settings = Settings::instance();
	if (!settings->parseCommandlineOptions(argc, argv))
	{
		// error already reported
		return 1;
	}
	setLogLevel(settings->m_logLevel.c_str());

	init_signals();

	SystemRestore::createSpecialDirectories();

	// Initialize the Preferences database
	PrefsDb* prefs_db = PrefsDb::instance();
	// and system restore (refresh settings while I'm at it...)
	SystemRestore* system_restore = SystemRestore::instance();
	system_restore->refreshDefaultSettings();

	//run startup restore before anything else starts
	SystemRestore::startupConsistencyCheck();

	LS::Error error;
	LSHandle* serviceHandle = nullptr;

	// Register the service
	if (!LSRegister("com.webos.service.systemservice", &serviceHandle, error))
	{
		qCritical() << "Failed to register service com.webos.service.systemservice: " << error.what();
		return 1;
	}

	if (!LSGmainAttach(serviceHandle, g_mainloop.get(), error))
	{
		qCritical() << "Failed to attach service handle to main loop: " << error.what();
		return 1;
	}

	sendSignals(serviceHandle);

	// Initialize the Prefs Factory
	PrefsFactory* prefs_factory = PrefsFactory::instance();
	prefs_factory->setServiceHandle(serviceHandle);

	BackupManager* bu_manager = BackupManager::instance();
	bu_manager->setServiceHandle(serviceHandle);


	if (!LSCall(serviceHandle, "luna://com.webos.service.settingsservice/getSystemSettings",
			R"({"keys":["localeInfo"],"subscribe":true})", TimePrefsHandler::cbLocaleHandler,
				nullptr, nullptr, error))
	{
		qDebug() << "could not get locale info: " << error.what();
		return -1;
	}

	// Clock handler
	ClockHandler clockHandler;
	setupClockHandler(clockHandler, serviceHandle);

	//init the timezone service;
	TimeZoneService* time_zone_srv = TimeZoneService::instance();
	time_zone_srv->setServiceHandle(serviceHandle);

	//init the osinfo service;
	OsInfoService *os_info_srv = OsInfoService::instance();
	os_info_srv->setServiceHandle(serviceHandle);

	//init the deviceinfo service;
	DeviceInfoService *device_info_srv = DeviceInfoService::instance();
	device_info_srv->setServiceHandle(serviceHandle);
	
	// Run the main loop
	g_main_loop_run(g_mainloop.get());

	delete device_info_srv;
	delete os_info_srv;
	delete time_zone_srv;
	delete bu_manager;
	delete prefs_factory;
	delete system_restore;
	delete prefs_db;
	delete settings;
	
	return 0;
}
