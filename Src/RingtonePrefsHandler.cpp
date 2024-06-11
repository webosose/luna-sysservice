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

#include "RingtonePrefsHandler.h"

#include <luna-service2++/error.hpp>

#include "SystemRestore.h"
#include "Utils.h"
#include "UrlRep.h"
#include "PrefsDb.h"
#include "Logging.h"
#include "JSONUtils.h"

using namespace pbnjson;

static const char* s_logChannel = "RingtonePrefsHandler";

static bool cbAddRingtone(LSHandle* lsHandle, LSMessage *message,
							void *user_data);

static bool cbDeleteRingtone(LSHandle* lsHandle, LSMessage *message,
							void *user_data);

/*! \page com_palm_systemservice_ringtone Service API com.webos.service.systemservice/ringtone/
 *
 *  Public methods:
 *   - \ref systemservice_ringtone_add_ringtone
 *   - \ref systemservice_ringtone_remove_ringtone
 */

static LSMethod s_methods[]  = {
	{ "addRingtone",     cbAddRingtone},
	{ "deleteRingtone",	 cbDeleteRingtone},
	{ 0, 0 },
};

RingtonePrefsHandler::RingtonePrefsHandler(LSHandle* serviceHandle) : PrefsHandler(serviceHandle)
{
	init();
}

RingtonePrefsHandler::~RingtonePrefsHandler()
{

}

void RingtonePrefsHandler::init() {
	//luna_log(s_logChannel,"RingtonePrefsHandler::init()");
	PMLOG_TRACE("RingtonePrefsHandler start");
	bool result;
	LSError lsError;
	LSErrorInit(&lsError);
	
	result = LSRegisterCategory( m_serviceHandle, "/ringtone", s_methods,
			NULL, NULL, &lsError);
	if (!result) {
		//luna_critical(s_logChannel, "Failed in registering ringtone handler method: %s", lsError.message);
		PmLogCritical(sysServiceLogContext(), "FAILED_TO_REGISTER", 0, "Failed in registering ringtone handler method:%s", lsError.message);
		LSErrorFree(&lsError);
		return;
	}

	result = LSCategorySetData(m_serviceHandle, "/ringtone", this, &lsError);
	if (!result) {
		//luna_critical(s_logChannel, "Failed in LSCategorySetData: %s", lsError.message);
		PmLogCritical(sysServiceLogContext(), "LSCATEGORYSETDATA_FAILED", 0, "Failed in LSCategorySetData:%s", lsError.message);
		LSErrorFree(&lsError);
		return;
	}

}

std::list<std::string> RingtonePrefsHandler::keys() const 
{
	std::list<std::string> k;
	k.push_back("ringtone");
	return k;
}

bool RingtonePrefsHandler::validate(const std::string& key, const pbnjson::JValue &)
{
	return true;		//TODO: should possibly see if the pref points to a valid file
}

void RingtonePrefsHandler::valueChanged(const std::string& key, const pbnjson::JValue &)
{
	//TODO: should possibly see if the pref points to a valid file
}

JValue RingtonePrefsHandler::valuesForKey(const std::string& key)
{
	//TODO: could scan ringtone folders and return possible files. However, since selection is handled by file picker which
	// may be scanning in unspecified locations, this wouldn't exactly be accurate
	return JObject {{"ringtone", JArray()}};
}

bool RingtonePrefsHandler::isPrefConsistent()
{
	return SystemRestore::instance()->isRingtoneSettingConsistent();
}

void RingtonePrefsHandler::restoreToDefault() 
{
	SystemRestore::instance()->restoreDefaultRingtoneSetting();
}

/*! \page com_palm_systemservice_ringtone
\n
\section systemservice_ringtone_add_ringtone addRingtone

\e Public.

com.webos.service.systemservice/ringtone/addRingtone

Add a ringtone.

\subsection systemservice_ringtone_add_ringtone_syntax Syntax:
\code
{
	"filePath": string
}
\endcode

\param filePath Absolute path to the ringtone file. Required.

\subsection systemservice_ringtone_add_ringtone_returns Returns:
\code
{
	"returnValue": boolean,
	"errorText": string
}
\endcode

\param returnValue Indicates if the call was succesful.
\param errorText Description of the error if call was not succesful.

\subsection systemservice_ringtone_add_ringtone_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/ringtone/addRingtone '{"filePath": "/usr/palm/sounds/ringtone.mp3" }'
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
	"errorText": "source file missing"
}
\endcode
*/
static bool cbAddRingtone(LSHandle* lsHandle, LSMessage *message, void *)
{
	// {"filePath": string}
	LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_1(PROPERTY(filePath, string))
													  REQUIRED_1(filePath)));

	if (!parser.parse(__FUNCTION__, lsHandle, Settings::instance()->schemaValidationOption))
		return true;

	bool success = false;

	int rc = 0;

	std::string errorText;
	std::string targetFileAndPath;
	std::string pathPart ="";
	std::string filePart ="";
	
	UrlRep urlRep;

	do {
		JValue root = parser.get();
		std::string srcFileName = root["filePath"].asString();

		//parse the string as a URL
		urlRep = UrlRep::fromUrl(srcFileName.c_str());

		if (urlRep.valid == false) {
			errorText = std::string("invalid specification for source file (please use url format)");
			break;
		}

		// UNSUPPORTED: non-file:// schemes
		if ((urlRep.scheme != "") && (urlRep.scheme != "file")) {
			errorText = std::string("input file specification doesn't support non-local files (use file:///path/file or /path/file format");
			break;
		}

		//check the file exist on the file system.
		if (!Utils::doesExistOnFilesystem(srcFileName.c_str())) {
			errorText = std::string("source file doesn't exist");
			break;
		}

		//copy it to the media partition

		Utils::splitFileAndPath(srcFileName, pathPart, filePart);

		if (filePart.length() == 0) {
			errorText = std::string("source file name missing.");
			break;
		}

		targetFileAndPath = std::string(PrefsDb::s_mediaPartitionPath)+std::string(PrefsDb::s_mediaPartitionRingtonesDir)+std::string("/")+filePart;
		rc = Utils::fileCopy(srcFileName.c_str(),targetFileAndPath.c_str());

		if (rc == -1) {
			errorText = std::string("Unable to add ringtone.");
			break;
		}

		success = true;
	} while (false);

	JObject response {{"returnValue", success}};

	if (!success) {
		response.put("errorText", errorText);
	}

	LS::Error error;
	(void) LSMessageReply(lsHandle, message, response.stringify().c_str(), error);

	return true;
}

/*! \page com_palm_systemservice_ringtone
\n
\section systemservice_ringtone_remove_ringtone removeRingtone

\e Public.

com.webos.service.systemservice/ringtone/removeRingtone

Delete a ringtone.

\subsection systemservice_ringtone_remove_ringtone_syntax Syntax:
\code
{
	"filePath": string
}
\endcode

\param filePath Absolute path to the ringtone file. Required.

\subsection systemservice_ringtone_remove_ringtone_returns Returns:
\code
{
	"returnValue": boolean,
	"errorText": string
}
\endcode

\param returnValue Indicates if the call was succesful.
\param errorText Description of the error if call was not succesful.

\subsection systemservice_ringtone_remove_ringtone_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/ringtone/deleteRingtone '{"filePath": "/media/internal/ringtones/ringtone.mp3" }'
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
	"errorText": "Unable to delete."
}
\endcode
*/
static bool cbDeleteRingtone(LSHandle* lsHandle, LSMessage *message, void *user_data)
{
	// {"filePath": string}
	LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_1(PROPERTY(filePath, string))
													  REQUIRED_1(filePath)));

	if (!parser.parse(__FUNCTION__, lsHandle, Settings::instance()->schemaValidationOption))
		return true;

	bool success = true;
	int rc = 0;

	std::string errorText;
	std::string pathPart ="";
	std::string filePart ="";
	std::string ringtonePartition = std::string(PrefsDb::s_mediaPartitionPath)+std::string(PrefsDb::s_mediaPartitionRingtonesDir)+std::string("/");
	
	do {
		JValue root = parser.get();

		std::string srcFileName = root["filePath"].asString();

		//check the file exist on the file system.
		if (!Utils::doesExistOnFilesystem(srcFileName.c_str())) {
			errorText = std::string("file doesn't exist");
			success = false;
			break;
		}

		//make sure we are deleting files only from ringtone partion.
		Utils::splitFileAndPath(srcFileName,pathPart,filePart);

		if (filePart.length() == 0) {
			errorText = std::string("source file name missing.");
			success = false;
			break;
		}

		if(pathPart.compare(ringtonePartition) != 0) {
			errorText = std::string("Unable to delete.");
			success = false;
			break;
		}

		//UI is currently making sure that the current ringtone is not getting deleted. May be, we can check again here to make sure the current ringtone is not removed.

		rc = unlink(srcFileName.c_str());
		if(rc == -1) {
			errorText = std::string("Unable to delete ringtone.");
			success = false;
			break;
		}
	} while (false);

	JObject response {{"returnValue", success}};
	if (!success) {
		response.put("errorText", errorText);
		PmLogWarning(sysServiceLogContext(), "ERROR_MESSAGE", 0, "error: %s", errorText.c_str());
	}

	LS::Error error;
	(void) LSMessageReply(lsHandle, message, response.stringify().c_str(), error);

	return true;
}
