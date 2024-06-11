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

#include "SystemRestore.h"

#include <cassert>

#include <glib.h>

#include <pbnjson.hpp>
#include <luna-service2/lunaservice.h>

#ifdef WEBOS_QT
#include <QtCore/QDebug>
#include <QtGui/QImageReader>
#endif //WEBOS_QT

#include "Utils.h"
#include "Logging.h"
#include "PrefsDb.h"
#include "JSONUtils.h"
#include "PrefsFactory.h"

using namespace pbnjson;

SystemRestore::SystemRestore() : m_msmState(Phone)
{
	do {
		//load the defaults file
		JValue root = JDomParser::fromFile(PrefsDb::s_defaultPrefsFile);
		if (!root.isObject()) {
			PmLogWarning(sysServiceLogContext(), "LOAD_PREFERENCES_FAIL", 0, "Failed to load prefs file: %s", PrefsDb::s_defaultPrefsFile);
			break;
		}

		JValue prefs = root["preferences"];
		if (!prefs.isObject()) {
			PmLogWarning(sysServiceLogContext(), "INVALID_PREFERENCES", 0, "Failed to get valid preferences entry from file");
			break;
		}

		for (const JValue::KeyValue pref: prefs.children()) {
			std::string keyStr = pref.first.asString();
			if (keyStr == "ringtone")
				defaultRingtoneString = pref.second.asString();
			else if (keyStr == "wallpaper")
				defaultWallpaperString = pref.second.asString();
		}

	} while (false);

	//load the defaults file
	JValue root = JDomParser::fromFile(PrefsDb::s_defaultPlatformPrefsFile);
	if (!root.isObject()) {
		PmLogWarning(sysServiceLogContext(), "LOAD_PREFERENCES_FAIL", 0, "Failed to load prefs file: %s", PrefsDb::s_defaultPlatformPrefsFile);
		return;
	}

	JValue prefs = root["preferences"];
	if (!prefs.isObject()) {
		PmLogWarning(sysServiceLogContext(), "INVALID_PREFERENCES", 0, "Failed to get valid preferences entry from file");
		return;
	}

	for (const JValue::KeyValue pref: prefs.children()) {

		std::string keyStr = pref.first.asString();
		if (keyStr == std::string("ringtone"))
			defaultRingtoneString = pref.second.asString();
		else if (keyStr == std::string("wallpaper"))
			defaultWallpaperString = pref.second.asString();
	}

//	//override if necessary...
//	overrideStr = PrefsDb::instance()->getPref(PrefsDb::s_sysDefaultRingtoneKey);
//	if (overrideStr.length())
//		defaultRingtoneString = overrideStr;
//	overrideStr = PrefsDb::instance()->getPref(PrefsDb::s_sysDefaultWallpaperKey);
//	if (overrideStr.length())
//		defaultWallpaperString = overrideStr;

	if (defaultRingtoneString.size())
		PrefsDb::instance()->setPref(PrefsDb::s_sysDefaultRingtoneKey,defaultRingtoneString);
	else {
		std::string overrideStr = PrefsDb::instance()->getPref(PrefsDb::s_sysDefaultRingtoneKey);
		if (overrideStr.size())
			defaultRingtoneString = overrideStr;
	}

	if (defaultWallpaperString.size())
		PrefsDb::instance()->setPref(PrefsDb::s_sysDefaultWallpaperKey,defaultWallpaperString);
	else {
		std::string overrideStr = PrefsDb::instance()->getPref(PrefsDb::s_sysDefaultWallpaperKey);
		if (overrideStr.size())
			defaultWallpaperString = overrideStr;
	}
}

/* COMMENT: No gio-2.0 on device! blah...look for slow, alternate function below this one...
//static
int SystemRestore::fileCopy(const char * srcFileAndPath,const char * dstFileAndPath)
{
	if ((srcFileAndPath == NULL) || (dstFileAndPath == NULL))
		return -1;
	GFile * src = g_file_new_for_path(srcFileAndPath);
	GFile * dst = g_file_new_for_path(dstFileAndPath);

	GError * err=NULL;
	gboolean rc = g_file_copy(
			src,
			dst,
			G_FILE_COPY_OVERWRITE,
			NULL,
			NULL,
			NULL,
			&err
	);
	
	g_object_unref(src);
	g_object_unref(dst);
	
	if (rc == false) {
		PmLogWarning(sysServiceLogContext(), "FILE_COPY_ERROR", 0, "file copy error %d: [%s]",err->code,err->message);
		g_error_free(err);
		return -1;
	}
	
	//make sure there is no error struct created, and if so free it
	if (err)
		g_error_free(err);
	
	return 1;
}
*/

int SystemRestore::restoreDefaultRingtoneToMediaPartition() 
{
	//check the file specified by defaultRingtoneFileAndPath
	if (!Utils::doesExistOnFilesystem(defaultRingtoneFileAndPath.c_str())) {
		return -1;
	}
	
	//copy it to the media partition
	
	std::string pathPart ="";
	std::string filePart ="";
	Utils::splitFileAndPath(defaultRingtoneFileAndPath,pathPart,filePart);
	if (filePart.length() == 0)
	{
		PmLogWarning(sysServiceLogContext(), "FILE_LENGTH_ZERO", 0, "filepart.length == 0, %s", filePart.c_str());
		return -1;
	}
	std::string targetFileAndPath = std::string(PrefsDb::s_mediaPartitionPath)+std::string(PrefsDb::s_mediaPartitionRingtonesDir)+std::string("/")+filePart;
	int rc = Utils::fileCopy(defaultRingtoneFileAndPath.c_str(),targetFileAndPath.c_str());
	if (rc == -1)
	{
		PmLogWarning(sysServiceLogContext(), "FILE_COPY_FAILED", 0, "filecopy %s --> %s failed", defaultRingtoneFileAndPath.c_str(), targetFileAndPath.c_str());

		return -1;
	}
	return 1;
}
int SystemRestore::restoreDefaultWallpaperToMediaPartition()
{
	//check the file specified by defaultWallpaperFileAndPath
	if (!Utils::doesExistOnFilesystem(defaultWallpaperFileAndPath.c_str()) ) {
		PmLogWarning(sysServiceLogContext(), "FILE_NOT_EXIST", 0, "file %s doesn\'t exist", defaultWallpaperFileAndPath.c_str());
		return -1;
	}

	//copy it to the media partition
	
	std::string pathPart ="";
	std::string filePart ="";
	Utils::splitFileAndPath(defaultWallpaperFileAndPath,pathPart,filePart);
	if (filePart.length() == 0)
	{
		PmLogWarning(sysServiceLogContext(), "FILE_LENGTH_ZERO", 0, "filepart.length == 0, %s", filePart.c_str());
		return -1;
	}
		
	std::string targetFileAndPath = std::string(PrefsDb::s_mediaPartitionPath)+std::string(PrefsDb::s_mediaPartitionWallpapersDir)+std::string("/")+filePart;
	int rc = Utils::fileCopy(defaultWallpaperFileAndPath.c_str(),targetFileAndPath.c_str());
	if (rc == -1)
	{
		PmLogWarning(sysServiceLogContext(), "FILE_COPY_FAILED", 0, "filecopy %s --> %s failed", defaultWallpaperFileAndPath.c_str(), targetFileAndPath.c_str());
		return -1;
	}

	return 1;
}

int SystemRestore::restoreDefaultRingtoneSetting()
{
	//parse json in the defaultRingtoneString
	
	int rc=0;

	do {
		JValue root = JDomParser::fromString(defaultRingtoneString);
		if (!root.isObject()) {
			PmLogWarning(sysServiceLogContext(), "PARSE_FAILED", 0, "Failed to parse default ringtone string into json: ' %s '", defaultRingtoneString.c_str());
			break;
		}

		JValue label = root["fullPath"];
		if (!label.isString()) {
			PmLogWarning(sysServiceLogContext(), "PARSE_FAILED", 0, "Failed to parse ringtone details");
			break;
		}

		defaultRingtoneFileAndPath = label.asString();

		//restore the default ringtone files to the media partition
		rc = restoreDefaultRingtoneToMediaPartition();
		if (rc == -1) {
			rc = 0;
			break; //error of some kind
		}
		//set the key into the database...remember, at this point the handlers are *NOT* up yet, so have to do it manually
		PrefsDb::instance()->setPref(std::string("ringtone"), defaultRingtoneString);

		rc = 1;
	} while (false);

	return rc;
}

int SystemRestore::restoreDefaultWallpaperSetting()
{
	//parse json in the defaultWallpaperString

	int rc=0;

	do {
		JValue root = JDomParser::fromString(defaultWallpaperString);
		if (!root.isObject()) {
			PmLogWarning(sysServiceLogContext(), "PARSE_FAILED", 0, "Failed to parse default wallpaper string into json: ' %s '", defaultWallpaperString.c_str());

			break;
		}

		JValue label = root["wallpaperFile"];
		if (!label.isString()) {
			PmLogWarning(sysServiceLogContext(), "PARSE_FAILED", 0, "Failed to parse wallpaper details");
			break;
		}

		//TODO: use this to cache for later so I don't have to reparse json each time
		defaultWallpaperFileAndPath = label.asString();

		//restore the default wallpaper file to the media partition
		rc = restoreDefaultWallpaperToMediaPartition();
		if (rc == -1) {
			PmLogWarning(sysServiceLogContext(), "RESTORE_ERROR", 0, "SystemRestore::restoreDefaultWallpaperSetting(): [ERROR] could not copy default wallpaper [%s] to media partition", defaultWallpaperFileAndPath.c_str());

			rc=0;
			break;		//error of some kind
		}
		//set the key into the database...remember, at this point the handlers are *NOT* up yet, so have to do it manually
		PrefsDb::instance()->setPref(std::string("wallpaper"),defaultWallpaperString);

		rc=1;
	} while (false);

	return rc;
}

bool SystemRestore::isRingtoneSettingConsistent() 
{
	std::string ringToneRawPref = PrefsDb::instance()->getPref("ringtone");
	std::string ringToneFileAndPath;
	
	bool rc=false;

	do {
		if (ringToneRawPref.length() == 0)
			break;

		//parse the setting
		JValue root = JDomParser::fromString(ringToneRawPref);
		if (!root.isObject()) {
			PmLogWarning(sysServiceLogContext(), "PARSE_ERROR", 0, "Failed to parse ringtone raw string into json: '%s'", ringToneRawPref.c_str());
			break;
		}

		JValue label = root["fullPath"];
		if (!label.isString()) {
			PmLogWarning(sysServiceLogContext(), "PARSE_ERROR", 0, "Failed to parse ringtone details");
			break;
		}

		ringToneFileAndPath = label.asString();

		PmLogDebug(sysServiceLogContext(),"checking [%s]...",ringToneFileAndPath.c_str());
		//check to see if file exists
		if (Utils::doesExistOnFilesystem(ringToneFileAndPath.c_str())) {
			if (Utils::filesizeOnFilesystem(ringToneFileAndPath.c_str()) > 0)			//TODO: a better check for corruption; see wallpaper consist. checking
				rc = true;
			else
				PmLogWarning(sysServiceLogContext(), "FILE_SIZE_ZERO", 0, "file size is 0; corrupt file");
		}
		else {
			PmLogWarning(sysServiceLogContext(), "INVALID_FILE", 0, "Sound file is not on filesystem");
		}
	} while (false);

	return rc;
}


bool SystemRestore::isWallpaperSettingConsistent()
{
	std::string wallpaperRawPref = PrefsDb::instance()->getPref("wallpaper");
	std::string wallpaperFileAndPath;
	
	bool rc=false;

	do {
		if (wallpaperRawPref.length() == 0)
			break;

		//parse the setting
		JValue root = JDomParser::fromString(wallpaperRawPref);
		if (!root.isObject()) {
			PmLogWarning(sysServiceLogContext(), "PARSE_FAILED", 0, "Failed to parse wallpaper string into json: '%s'", wallpaperRawPref.c_str());
			break;
		}

		JValue label = root["wallpaperFile"];
		if (!label.isString()) {
			PmLogWarning(sysServiceLogContext(), "PARSE_FAILED", 0, "Failed to parse wallpaper details");
			break;
		}

		wallpaperFileAndPath = label.asString();

		PmLogDebug(sysServiceLogContext(),"checking [%s]...",wallpaperFileAndPath.c_str());
		//check to see if file exists
#ifdef WEBOS_QT
		{
			QImageReader reader(wallpaperFileAndPath.c_str());
			if (reader.canRead())
				rc = true;
			else
				qWarning() <<reader.errorString()<<reader.fileName();
		}
#endif //WEBOS_QT
	} while (false);

	return rc;
}

void SystemRestore::refreshDefaultSettings()
{
	std::string wallpaperRawDefaultPref;
	std::string ringtoneRawDefaultPref;

	wallpaperRawDefaultPref = PrefsDb::instance()->getPref(PrefsDb::s_sysDefaultWallpaperKey);
	
	if (wallpaperRawDefaultPref.length() == 0)
		return;

	do {
		//parse the setting
		JValue root = JDomParser::fromString(wallpaperRawDefaultPref);
		if (!root.isObject()) {
			PmLogWarning(sysServiceLogContext(), "PARSE_FAILED", 0, "Failed to parse wallpaper default pref string into json: '%s'", wallpaperRawDefaultPref.c_str());
			break;
		}

		JValue label = root["wallpaperFile"];
		if (!label.isString()) {
			PmLogWarning(sysServiceLogContext(), "PARSE_FAILED", 0, "Failed to parse wallpaper details");
			break;
		}

		defaultWallpaperString = wallpaperRawDefaultPref;
		defaultWallpaperFileAndPath = label.asString();
	} while (false);

	ringtoneRawDefaultPref = PrefsDb::instance()->getPref(PrefsDb::s_sysDefaultRingtoneKey);

	if (ringtoneRawDefaultPref.length() == 0)
		return;

	//parse the setting
	JValue root = JDomParser::fromString(ringtoneRawDefaultPref);
	if (!root.isObject()) {
		 PmLogWarning(sysServiceLogContext(), "PARSE_FAILED", 0, "Failed to parse ringtone deafult pref string into json: '%s'",ringtoneRawDefaultPref.c_str());
		return;
	}

	JValue label = root["fullPath"];
	if (!label.isString()) {
		PmLogWarning(sysServiceLogContext(), "PARSE_FAILED", 0, "Failed to parse ringtone details");
		return;
	}

	defaultRingtoneString = ringtoneRawDefaultPref;
	defaultRingtoneFileAndPath = label.asString();
}

int SystemRestore::createSpecialDirectories()
{

	//make sure the prefs folder exists
	(void)  g_mkdir_with_parents(PrefsDb::s_prefsPath, 0755);

	//make sure the ringtones folder exists.
	std::string path = std::string(PrefsDb::s_mediaPartitionPath)+std::string(PrefsDb::s_mediaPartitionRingtonesDir);
	(void) g_mkdir_with_parents(path.c_str(), 0755);

	//make sure the wallpapers folder exists
	path = std::string(PrefsDb::s_mediaPartitionPath)+std::string(PrefsDb::s_mediaPartitionWallpapersDir);
	(void) g_mkdir_with_parents(path.c_str(), 0755);

	//make sure the wallpapers thumbnail folder exists
	path = std::string(PrefsDb::s_mediaPartitionPath)+std::string(PrefsDb::s_mediaPartitionWallpaperThumbsDir);
	(void) g_mkdir_with_parents(path.c_str(), 0755);

//#if defined (__arm__)
//	std::string cmdline = std::string("mattrib -i /dev/mapper/store-media +h ::")+std::string(PrefsDb::s_mediaPartitionWallpapersDir);
//	system(cmdline.c_str());
//#endif

	//make sure the systemservice special folder exists
	path = std::string(PrefsDb::s_mediaPartitionPath)+std::string(PrefsDb::s_sysserviceDir);
	(void) g_mkdir_with_parents(path.c_str(), 0755);

	//make sure the systemservice temp folder exists
	path = std::string(PrefsDb::s_mediaPartitionPath)+std::string(PrefsDb::s_mediaPartitionTempDir);
	(void) g_mkdir_with_parents(path.c_str(), 0755);
	
//#if defined (__arm__)
//	cmdline = std::string("mattrib -i /dev/mapper/store-media +h ::")+std::string(PrefsDb::s_sysserviceDir);
//	system(cmdline.c_str());
//#endif

		
	return 1;
}

int SystemRestore::startupConsistencyCheck() 
{

	PMLOG_TRACE("%s:started",__FUNCTION__);
	// -- run startup tests to determine the state of the device

	if (Utils::doesExistOnFilesystem(PrefsDb::s_systemTokenFileAndPath) == false) {
		//the media partition has been reformatted or damaged
		PmLogWarning(sysServiceLogContext(), "TOKEN_MISSING", 0, "running - system token missing; media was erased/damaged");
		//run restore

		int rc=0;		//avoid having tons of if's ...so don't mess with the return values of the restore__ functions
		rc += SystemRestore::instance()->restoreDefaultRingtoneSetting();
		rc += SystemRestore::instance()->restoreDefaultWallpaperSetting();

		//create token if all these succeeded
		if (rc == 2) {
			FILE * fp = fopen(PrefsDb::s_systemTokenFileAndPath,"w");
			if (fp != NULL) {
				fprintf(fp,"%lu",time(NULL));		//doesn't matter what I put in here, but timestamp seems sane
				fflush(fp);
				fclose(fp);
			}
		}
		else {
			PmLogWarning(sysServiceLogContext(), "TOKEN_MISSING", 0, "running - system token missing and WAS NOT written because one of the restore functions failed!");
		}
	}
	else {
		
		PMLOG_TRACE("running - checking wallpaper and ringtone consistency");
		//check consistency of wallpaper setting
		if (!SystemRestore::instance()->isWallpaperSettingConsistent()) {
			//run restore on wallpaper
			SystemRestore::instance()->restoreDefaultWallpaperSetting();
		}
		//check consistency of ringtone setting
		if (!SystemRestore::instance()->isRingtoneSettingConsistent()) {
			SystemRestore::instance()->restoreDefaultRingtoneSetting();
		}
	}

	//check the media icon file
	if (Utils::filesizeOnFilesystem(PrefsDb::s_volumeIconFileAndPathDest) == 0) {
		PMLOG_TRACE("running - restoring volume icon file");
		//restore it
		Utils::fileCopy(PrefsDb::s_volumeIconFileAndPathSrc,PrefsDb::s_volumeIconFileAndPathDest);
	}

//	//attrib it all for good measure
//#if defined (__arm__)
//	int exitCode;
//	std::string cmdline = std::string("mattrib -i /dev/mapper/store-media +h ::")+std::string(PrefsDb::s_mediaPartitionWallpapersDir);
//	exitCode = system(cmdline.c_str());
//	g_warning("SystemRestore::startupConsistencyCheck() running - [%s] returned %d",cmdline.c_str(),exitCode);
//	cmdline = std::string("mattrib -i /dev/mapper/store-media +h ::")+std::string(PrefsDb::s_sysserviceDir);
//	exitCode = system(cmdline.c_str());
//	g_warning("SystemRestore::startupConsistencyCheck() running - [%s] returned %d",cmdline.c_str(),exitCode);
//	cmdline = std::string("mattrib -i /dev/mapper/store-media +h ::")+std::string(PrefsDb::s_volumeIconFile);
//	exitCode = system(cmdline.c_str());
//	g_warning("SystemRestore::startupConsistencyCheck() running - [%s] returned %d",cmdline.c_str(),exitCode);
//#endif

	PMLOG_TRACE("%s:finished",__FUNCTION__);
	return 1;
}

//static
int SystemRestore::runtimeConsistencyCheck() 
{
	PMLOG_TRACE("%s:started",__FUNCTION__);
	
	PrefsFactory::instance()->runConsistencyChecksOnAllHandlers();
	
	//check the media icon file
	if (Utils::filesizeOnFilesystem(PrefsDb::s_volumeIconFileAndPathDest) == 0) {
	PMLOG_TRACE("running - restoring volume icon file");
		//restore it
		Utils::fileCopy(PrefsDb::s_volumeIconFileAndPathSrc,PrefsDb::s_volumeIconFileAndPathDest);
	}

	//attrib it all for good measure
//#if defined (__arm__)
//	int exitCode;
//	std::string cmdline = std::string("mattrib -i /dev/mapper/store-media +h ::")+std::string(PrefsDb::s_mediaPartitionWallpapersDir);
//	exitCode = system(cmdline.c_str());
//	g_warning("SystemRestore::runtimeConsistencyCheck() running - [%s] returned %d",cmdline.c_str(),exitCode);
//	cmdline = std::string("mattrib -i /dev/mapper/store-media +h ::")+std::string(PrefsDb::s_sysserviceDir);
//	exitCode = system(cmdline.c_str());
//	g_warning("SystemRestore::runtimeConsistencyCheck() running - [%s] returned %d",cmdline.c_str(),exitCode);
//	cmdline = std::string("mattrib -i /dev/mapper/store-media +h ::")+std::string(PrefsDb::s_volumeIconFile);
//	exitCode = system(cmdline.c_str());
//	g_warning("SystemRestore::runtimeConsistencyCheck() running - [%s] returned %d",cmdline.c_str(),exitCode);
//#endif

	PMLOG_TRACE("%s:finished",__FUNCTION__);
	return 1;
}

bool SystemRestore::msmAvailCallback(LSHandle* handle, LSMessage* message, void* ctxt)
{
	if (LSMessageIsHubErrorMessage(message)) {  // returns false if message is NULL
		PmLogWarning(sysServiceLogContext(), "HUB_ERROR_MESSAGE", 0, "The message received is an error message from the hub");
		return true;
	}
	return SystemRestore::instance()->msmAvail(message);
}

bool SystemRestore::msmProgressCallback(LSHandle* handle, LSMessage* message, void* ctxt)
{
	if (LSMessageIsHubErrorMessage(message)) {  // returns false if message is NULL
		PmLogWarning(sysServiceLogContext(), "HUB_ERROR_MESSAGE", 0, "The message received is an error message from the hub");
		return true;
	}
	return SystemRestore::instance()->msmProgress(message);
}

bool SystemRestore::msmEntryCallback(LSHandle* handle, LSMessage* message, void* ctxt)
{
	if (LSMessageIsHubErrorMessage(message)) {  // returns false if message is NULL
		PmLogWarning(sysServiceLogContext(), "HUB_ERROR_MESSAGE", 0, "The message received is an error message from the hub");
		return true;
	}
	return SystemRestore::instance()->msmEntry(message);
}

bool SystemRestore::msmFsckingCallback(LSHandle* handle, LSMessage* message, void* ctxt)
{
	if (LSMessageIsHubErrorMessage(message)) {  // returns false if message is NULL
		PmLogWarning(sysServiceLogContext(), "HUB_ERROR_MESSAGE", 0, "The message received is an error message from the hub");
		return true;
	}
	return SystemRestore::instance()->msmFscking(message);
}

bool SystemRestore::msmPartitionAvailCallback(LSHandle* handle, LSMessage* message, void* ctxt)
{
	if (LSMessageIsHubErrorMessage(message)) {  // returns false if message is NULL
		PmLogWarning(sysServiceLogContext(), "HUB_ERROR_MESSAGE", 0, "The message received is an error message from the hub");
		return true;
	}
	return SystemRestore::instance()->msmPartitionAvailable(message);
}


bool SystemRestore::msmAvail(LSMessage* message)
{
	// {"mode-avail": boolean}
	LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_2(PROPERTY(mode-avail, boolean),
															  PROPERTY(returnValue,boolean))
													  REQUIRED_1(returnValue)));

	if (!parser.parse(__FUNCTION__, nullptr, Settings::instance()->schemaValidationOption))
		return false;

	JValue avail = parser.get()["mode-avail"];
	if (!avail.isValid()) return false;

	PmLogDebug(sysServiceLogContext(),"msmAvail(): MSM available: %s", avail.asBool() ? "true" : "false");

	//attrib it all for good measure  ... necessary because attrib-ing at boot doesn't always work, storaged sometimes lies about partition available
	//			...so try it again right before the user can go into storage mode and see the hidden files anyways
//#if defined (__arm__)
//	if (available) {
//		int exitCode;
//		std::string cmdline = std::string("mattrib -i /dev/mapper/store-media +h ::")+std::string(PrefsDb::s_mediaPartitionWallpapersDir);
//		exitCode = system(cmdline.c_str());
//		g_warning("SystemRestore::msmAvail() running - [%s] returned %d",cmdline.c_str(),exitCode);
//		cmdline = std::string("mattrib -i /dev/mapper/store-media +h ::")+std::string(PrefsDb::s_sysserviceDir);
//		exitCode = system(cmdline.c_str());
//		g_warning("SystemRestore::msmAvail() running - [%s] returned %d",cmdline.c_str(),exitCode);
//		cmdline = std::string("mattrib -i /dev/mapper/store-media +h ::")+std::string(PrefsDb::s_volumeIconFile);
//		exitCode = system(cmdline.c_str());
//		g_warning("SystemRestore::msmAvail() running - [%s] returned %d",cmdline.c_str(),exitCode);
//	}
//#endif

	return true;
}

bool SystemRestore::msmProgress(LSMessage* message)
{
	// {"stage": string}
	LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_2(PROPERTY(stage, string),
															  PROPERTY(returnValue,boolean))
													  REQUIRED_1(returnValue)));

	if (!parser.parse(__FUNCTION__, nullptr, Settings::instance()->schemaValidationOption))
		return false;

	JValue stage = parser.get()["stage"];
	if (!stage.isValid()) return false;

	PmLogDebug(sysServiceLogContext(),"msmProgress(): MSM stage: [%s]", stage.asString().c_str());

	return true;
}

bool SystemRestore::msmEntry(LSMessage* message)
{
	// {"new-mode": string}
	LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_2(PROPERTY(new-mode, string),
															  PROPERTY(returnValue, boolean))
													  REQUIRED_1(returnValue)));

	if (!parser.parse(__FUNCTION__, nullptr, Settings::instance()->schemaValidationOption))
		return false;

	JValue mode = parser.get()["new-mode"];
	if (!mode.isValid()) return false;

	if (mode.asString() == "brick") {
		m_msmState = Brick;
	}
	else { //TODO: proper handling for all cases; for now, assume phone whenever not brick
		m_msmState = Phone;
	}

	PmLogDebug(sysServiceLogContext(),"msmEntry(): MSM mode: [%s]", mode.asString().c_str());

	return true;
}

bool SystemRestore::msmFscking(LSMessage* message)
{
	PMLOG_TRACE("msmFscking()");
	return true;
}

bool SystemRestore::msmPartitionAvailable(LSMessage* message) 
{
	// {"mount_point": string, "available": boolean}
	LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_3(PROPERTY(mount_point, string),
															  PROPERTY(available, boolean),
															  PROPERTY(returnValue,boolean))
													  REQUIRED_1(returnValue)));

	if (!parser.parse(__FUNCTION__, nullptr, Settings::instance()->schemaValidationOption))
		return false;

	PMLOG_TRACE("%s: signaled",__FUNCTION__);

	JValue payload = parser.get();

	std::string mountPoint;
	JValue label = payload["mount_point"];
	if (label.isValid()) {
		mountPoint = label.asString();
	}
	else {
		mountPoint = std::string("UNKNOWN");
	}

	bool available=false;
	label = payload["available"];
	if (label.isValid())
		available = label.asBool();

	PmLogDebug(sysServiceLogContext(),"msmPartitionAvailable(): mount point: [%s] , available: %s", mountPoint.c_str(),
		   (available ? "true" : "false"));

	if (available && (mountPoint == "/media/internal")) {
		SystemRestore::createSpecialDirectories();
		SystemRestore::runtimeConsistencyCheck();
	}

	return true;
}

