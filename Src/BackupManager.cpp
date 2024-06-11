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

#include "BackupManager.h"

#include <pbnjson.hpp>
#include <luna-service2++/error.hpp>

#include "PrefsDb.h"
#include "PrefsFactory.h"

#include "Logging.h"
#include "Utils.h"
#include "Settings.h"
#include "JSONUtils.h"

using namespace pbnjson;

/* BackupManager implementation is based on the API documented at https://wiki.palm.com/display/ServicesEngineering/Backup+and+Restore+2.0+API
 * Backs up the systemprefs database
 */

std::string BackupManager::s_backupKeylistFilename = WEBOS_INSTALL_WEBOS_SYSCONFDIR "/sysservice-backupkeys.json";

/*!
 * \page com_palm_systemservice_backup Service API com.webos.service.systemservice/backup/
 *
 * Public methods:
 *
 * - \ref com_palm_systemservice_pre_backup
 * - \ref com_palm_systemservice_post_restore
 */

/**
 * These are the methods that the backup service can call when it's doing a
 * backup or restore.
 */
LSMethod BackupManager::s_BackupServerMethods[]  = {
	{ "preBackup"  , BackupManager::preBackupCallback },
	{ "postRestore", BackupManager::postRestoreCallback },
	{ 0, 0 }
};


BackupManager::BackupManager()
	: m_doBackupFiles(true)
	, m_p_backupDb(nullptr)
{
}

void BackupManager::setServiceHandle(LSHandle* serviceHandle)
{
	LS::Error error;
	if (!LSRegisterCategory(serviceHandle, "/backup", s_BackupServerMethods,
		nullptr, nullptr, error.get()))
	{
		PmLogCritical(sysServiceLogContext(), "LSREGISTER_CATEGORY_FAILED", 0, "Failed to register backup methods: %s", error.what());
	}
}

void BackupManager::copyKeysToBackupDb()
{
	if (!m_p_backupDb)
		return;

	//open the backup keys list to figure out what to copy
	JValue backupKeysJson = JDomParser::fromFile(BackupManager::s_backupKeylistFilename.c_str());

	if (!backupKeysJson.isArray()) {
		PmLogWarning(sysServiceLogContext(), "STRING_KEY_NOT_EXIST",0,"file does not contain an array of string keys");
		return;
	}

	std::list<std::string> keylist;
	for (const JValue key: backupKeysJson.items()) {
		if (!key.isString()) {
			PmLogWarning(sysServiceLogContext(),"INVALID_KEY",0,"Invalid key (skipping)");
			continue;
		}

		keylist.push_back(key.asString());
	}
	m_p_backupDb->copyKeys(PrefsDb::instance(), keylist);
}

void BackupManager::initFilesForBackup(bool useFilenameWithoutPath)
{
	if (m_p_backupDb)
	{
		if (g_file_test(m_p_backupDb->databaseFile().c_str(), G_FILE_TEST_EXISTS))
		{
			if (useFilenameWithoutPath)
			{
				m_backupFiles.push_back(m_p_backupDb->m_dbFilename.c_str());
			}
			else
			{
				char *dbFilename = strdup(m_p_backupDb->databaseFile().c_str());
                const char *cstr = basename(dbFilename ? dbFilename : "");
				std::string filename = ( cstr ? std::string(cstr) : std::string());
				free(dbFilename);
				if (filename.find("/") != std::string::npos)
					filename = std::string("");			///all for safety
				m_backupFiles.push_back(filename);
			}

			if (Settings::instance()->m_saveLastBackedUpTempDb)
			{
				Utils::fileCopy(m_p_backupDb->databaseFile().c_str(),
						(std::string(PrefsDb::s_mediaPartitionPath)+std::string(PrefsDb::s_sysserviceDir)+std::string("/lastBackedUpTempDb.db")).c_str());
			}
		}
	}
}

/*! \page com_palm_systemservice_backup
\n
\section com_palm_systemservice_pre_backup preBackup

\e Public.

com.webos.service.systemservice/backup/preBackup

Make a backup of LunaSysService preferences.

\subsection com_palm_systemservice_pre_backup_syntax Syntax:
\code
{
	"incrementalKey": object,
	"maxTempBytes":   int,
	"tempDir":        string
}
\endcode

\param incrementalKey This is used primarily for mojodb, backup service will handle other incremental backups.
\param maxTempBytes The allowed size of upload, currently 10MB (more than enough for our backups).
\param tempDir Directory to store temporarily generated files.

\subsection com_palm_systemservice_pre_backup_returns Returns:
\code
{
	"description": string,
	"version": string,
	"files": string array
}
\endcode

\param description Describes the backup.
\param version Version of the backup.
\param files List of files included in the backup.

\subsection com_palm_systemservice_pre_backup_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/backup/preBackup '{}'
\endcode

Example response for a succesful call:
\code
{
	"description": "Backup of LunaSysService, containing the systemprefs sqlite3 database",
	"version": "1.0",
	"files": [
		"\/var\/luna\/preferences\/systemprefs_backup.db"
	]
}
\endcode
*/
/**
 * Called by the backup service for all four of our callback functions: preBackup,
 * postBackup, preRestore, postRestore.
 */
bool BackupManager::preBackupCallback( LSHandle* lshandle, LSMessage *message, void *user_data)
{
	PMLOG_TRACE("%s:starting",__FUNCTION__);
	if (LSMessageIsHubErrorMessage(message)) {  // returns false if message is NULL
		PmLogWarning(sysServiceLogContext(),"HUB_ERROR_MESSAGE",0,"The message received is an error message from the hub");
		return true;
	}

	// payload is expected to have the following fields -
	// incrementalKey - this is used primarily for mojodb, backup service will handle other incremental backups
	// maxTempBytes - this is the allowed size of upload, currently 10MB (more than enough for our backups)
	// tempDir - directory to store temporarily generated files
	LSMessageJsonParser parser(message, RELAXED_SCHEMA(PROPS_1(PROPERTY(tempDir, string))));

	if (!parser.parse(__FUNCTION__, lshandle, EValidateAndErrorAlways))
		return true;

	//grab the temp dir
	JValue tempDirLabel = parser.get()["tempDir"];

	std::string tempDir;
	bool myTmp = false;
	if (tempDirLabel.isValid())
	{
		tempDir = tempDirLabel.isString() ? tempDirLabel.asString() : "";
	}
	else
	{
		PmLogDebug(sysServiceLogContext(),"No tempDir specified in preBackup message");
		tempDir = PrefsDb::s_prefsPath;
		myTmp = true;
	}

	BackupManager *self = BackupManager::instance();

	// try and create it
	std::string dbfile = tempDir;
	if (dbfile.empty() || *dbfile.rbegin() != '/')
		dbfile += '/';
	dbfile += PrefsDb::s_tempBackupDbFilenameOnly;

	self->m_p_backupDb.reset(PrefsDb::createStandalone(dbfile));
	if (!self->m_p_backupDb)
	{
		//failed to create temp db
		PmLogWarning(sysServiceLogContext(),"DB_ERROR",0,"unable to create a temporary backup db at [%s]...aborting!",dbfile.c_str());
		return self->sendPreBackupResponse(lshandle,message,std::list<std::string>());
	}

	// Attempt to copy relevant keys into the temporary backup database
	self->copyKeysToBackupDb();
	// adding the files for backup at the time of request.
	self->initFilesForBackup(myTmp);

	if (!self->m_doBackupFiles)
	{
		PmLogWarning(sysServiceLogContext(),"NO_BACKUP",0,"opted not to do a backup at this time due to doBackup internal var");
		return self->sendPreBackupResponse(lshandle,message,std::list<std::string>());
	}

	return self->sendPreBackupResponse(lshandle, message, self->m_backupFiles);
}

/*! \page com_palm_systemservice_backup
\n
\section com_palm_systemservice_post_restore postRestore

\e Public.

com.webos.service.systemservice/backup/postRestore

Restore a LunaSysService backup.

\subsection com_palm_systemservice_post_restore_syntax Syntax:
\code
{
	"tempDir": string,
	"files":   string array
}
\endcode

\param tempDir Directory to store temporarily generated files. Required.
\param files List of files to restore. Required.

\subsection com_palm_systemservice_post_restore_returns Returns:
\code
{
	"returnValue": boolean
}
\endcode

\param returnValue Indicates if the call was succesful.

\subsection com_palm_systemservice_post_restore_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/backup/postRestore '{"tempDir": "/tmp/", "files": ["/var/luna/preferences/systemprefs_backup.db"] }'
\endcode

Example response for a succesful call:
\code
{
	"returnValue": true
}
\endcode
*/
bool BackupManager::postRestoreCallback( LSHandle* lshandle, LSMessage *message, void *user_data)
{
	// {"tempDir": string, "files": array}
	LSMessageJsonParser parser(message, STRICT_SCHEMA(PROPS_2(PROPERTY(tempDir, string),
															  PROPERTY(files, array))
													  REQUIRED_2(tempDir, files)));

	if (!parser.parse(__FUNCTION__, lshandle, EValidateAndErrorAlways))
		return true;

	JValue root = parser.get();

	std::string tempDir = root["tempDir"].asString();
	JValue files = root["files"];

	for (const JValue &file: files.items())
	{
		std::string path = file.isString() ? file.asString() : "";
		PmLogDebug(sysServiceLogContext(),"array file: %s", path.c_str());

		if (path.empty())
		{
			PmLogWarning(sysServiceLogContext(),"FILE_PATH_EMPTY",0,"array object is a file path that is empty (skipping)");
			continue;
		}
		if (path[0] != '/')
		{
			//not an absolute path apparently...try taking on tempdir
			path = tempDir + std::string("/") + path;
			PmLogWarning(sysServiceLogContext(),"NOT_ABSOLUTE_FILE_PATH",0,"array object  is a file path that seems to be relative...trying to absolute-ize it by adding tempDir, like so: [%s]",path.c_str());
		}

		///PROCESS SPECIFIC FILES HERE....

		if (path.find("systemprefs_backup.db") != std::string::npos)
		{
			//found the backup db...

			if (Settings::instance()->m_saveLastBackedUpTempDb)
			{
				Utils::fileCopy(path.c_str(),
								(std::string(PrefsDb::s_mediaPartitionPath)+std::string(PrefsDb::s_sysserviceDir)+std::string("/lastRestoredTempDb.db")).c_str());
			}

			//run a merge
			int rc = PrefsDb::instance()->merge(path);
			if (rc == 0)
			{
				PmLogWarning(sysServiceLogContext(),"ERROR_OR_EMPTY_BACKUP",0,"merge() from [%s] didn't merge anything...could be an error or just an empty backup db",path.c_str());
			}
		}
	}

	// if for whatever reason the main db got closed, reopen it (the function will act ok if already open)
	PrefsDb::instance()->openPrefsDb();
	//now refresh all the keys
	PrefsFactory::instance()->refreshAllKeys();

	return BackupManager::instance()->sendPostRestoreResponse(lshandle,message);
}

bool BackupManager::sendPreBackupResponse(LSHandle* lshandle, LSMessage *message,
										  const std::list<std::string> fileList)
{
	std::string versionDb = PrefsDb::instance()->getPref("databaseVersion");
	if (versionDb.empty())
		versionDb = "0.0"; //signifies a problem

	// the response has to contain
	// description - what is being backed up
	// files - array of files to be backed up
	// version - version of the service
	JObject response {{"description", "Backup of LunaSysService, containing the systemprefs sqlite3 database"},
					  {"version", versionDb}};

	JArray files;
	if (m_doBackupFiles)
	{
		for (const std::string &file: fileList) {
			files.append(file)
			PMLOG_TRACE("added file %s to the backup list", file->c_str());
		}
	}
	else
	{
		PmLogWarning(sysServiceLogContext(),"NO_BACKUP",0,"opted not to do a backup at this time due to doBackup internal var");
	}
	response.put("files", files);

	PmLogDebug(sysServiceLogContext(),"Sending response to preBackupCallback: %s", response.stringify().c_str());

	LS::Error error;
	if (!LSMessageReply (lshandle, message, response.stringify().c_str(), error)) {
		PmLogWarning(sysServiceLogContext(),"PRE_BACKUP_CALLBACK_ERROR",0,"Can't send reply to preBackupCallback error: %s",error.what());
	}

	return true;
}

bool BackupManager::sendPostRestoreResponse(LSHandle* lshandle, LSMessage *message)
{
	PmLogDebug(sysServiceLogContext(),"Sending response to postRestoreCallback: %s", R"({"returnValue": true})");

	LS::Error error;
	if (!LSMessageReply (lshandle, message, R"({"returnValue": true})", error)) {
		PmLogWarning(sysServiceLogContext(),"POST_RESTORE_CALLBACK_ERROR",0,"Can't send reply to postRestoreCallback error: %s",error.what());
	}

	return true;

}
