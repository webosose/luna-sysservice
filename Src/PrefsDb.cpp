// Copyright (c) 2010-2023 LG Electronics, Inc.
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


#include <assert.h>
#include <pbnjson.hpp>
#include <glib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "Logging.h"
#include "PrefsDb.h"
#include "Utils.h"
#include "SystemRestore.h"

using namespace pbnjson;

const char* PrefsDb::s_defaultPrefsFile = WEBOS_INSTALL_WEBOS_SYSCONFDIR "/defaultPreferences.txt";
const char* PrefsDb::s_defaultPlatformPrefsFile = WEBOS_INSTALL_WEBOS_SYSCONFDIR "/defaultPreferences-platform.txt";
const char* PrefsDb::s_customizationOverridePrefsFile = WEBOS_INSTALL_SYSMGR_DATADIR "/customization/cust-preferences.txt";
const char* PrefsDb::s_custCareNumberFile = WEBOS_INSTALL_WEBOS_SYSCONFDIR "/CustomerCareNumber.txt";
const char* PrefsDb::s_prefsDbPath = WEBOS_INSTALL_SYSMGR_LOCALSTATEDIR "/preferences/systemprefs.db";
const char* PrefsDb::s_tempBackupDbFilenameOnly = "systemprefs_backup.db";
const char* PrefsDb::s_prefsPath = WEBOS_INSTALL_SYSMGR_LOCALSTATEDIR "/preferences";

const char* PrefsDb::s_logChannel = "PrefsDb";

#if !defined(DESKTOP)
#define MEDIAPARTITIONPATH "/media/internal/"
#else
#define MEDIAPARTITIONPATH "/tmp/webos/"  // note: changed from simply "/tmp/"
#endif

const char* PrefsDb::s_mediaPartitionPath = MEDIAPARTITIONPATH;

const char* PrefsDb::s_mediaPartitionWallpapersDir = ".wallpapers";
const char* PrefsDb::s_mediaPartitionWallpaperThumbsDir = ".wallpapers/thumbs";
const char* PrefsDb::s_mediaPartitionTempDir = ".temp";
const char* PrefsDb::s_mediaPartitionRingtonesDir = "ringtones";

const char* PrefsDb::s_sysserviceDir = ".sysservice";
const char* PrefsDb::s_systemTokenFileAndPath = MEDIAPARTITIONPATH ".sysservice/token";

const char* PrefsDb::s_volumeIconFileAndPathSrc = WEBOS_INSTALL_SYSMGR_DATADIR "/system/luna-systemui/images/castle.icns";

const char* PrefsDb::s_volumeIconFile = ".VolumeIcon.icns";

const char* PrefsDb::s_volumeIconFileAndPathDest = MEDIAPARTITIONPATH ".VolumeIcon.icns";

const char* PrefsDb::s_sysDefaultWallpaperKey = ".prefsdb.setting.default.wallpaper";
const char* PrefsDb::s_sysDefaultRingtoneKey = ".prefsdb.setting.default.ringtone";

PrefsDb* PrefsDb::createStandalone(const std::string& dbFilename,bool deleteExisting)
{
	if (deleteExisting)
	{
		unlink(dbFilename.c_str());
	}

	PrefsDb * pDb = new PrefsDb(dbFilename);
	if (pDb->m_prefsDb)
		return pDb;

	//else, creation failed...delete the faulty pDb and return 0
	delete pDb;
	return nullptr;
}

PrefsDb::PrefsDb()
: m_prefsDb(0)
, m_standalone(false)
, m_dbFilename(s_prefsDbPath)
, m_deleteOnDestroy(false)
{
	openPrefsDb();
}

PrefsDb::PrefsDb(const std::string& standaloneDbFilename)
: m_prefsDb(0)
, m_standalone(true)
, m_dbFilename(standaloneDbFilename)
, m_deleteOnDestroy(false)
{
	openPrefsDb();
}

PrefsDb::~PrefsDb()
{
	closePrefsDb();
	if (m_deleteOnDestroy)
	{
		//on purpose that it doesn't respect deleteOnDestroy for the singleton copy
		unlink(m_dbFilename.c_str());
	}
}

bool PrefsDb::setPref(const std::string& key, const std::string& value)
{
	if (!m_prefsDb)
		return false;

	if (key.empty())
		return false;

	char *queryStr = sqlite3_mprintf("INSERT INTO Preferences "
									  "VALUES (%Q, %Q)",
									  key.c_str(), value.c_str());

	if (!queryStr)
		return false;

	int ret = sqlite3_exec(m_prefsDb, queryStr, NULL, NULL, NULL);

	if (ret) {
		qWarning("Failed to execute query for key %s", key.c_str());

		sqlite3_free(queryStr);
		return false;
	}

	sqlite3_free(queryStr);

	qDebug("set ( [%s] , [---, length %zu] )", key.c_str(), value.size());
	return true;
}

std::string PrefsDb::getPref(const std::string& key)
{
	sqlite3_stmt* statement = 0;
	const char* tail = 0;
	std::string result = "";

	if (!m_prefsDb || key.empty())
		return result;

	char *queryStr = sqlite3_mprintf("SELECT value FROM Preferences WHERE key=%Q",key.c_str());

	if (!queryStr)
		goto Done;

	if (sqlite3_prepare(m_prefsDb, queryStr, -1, &statement, &tail)) {
		qWarning("Failed to prepare sql statement: %s", queryStr);
		goto Done;
	}

	if (sqlite3_step(statement) == SQLITE_ROW) {
		const unsigned char* res = sqlite3_column_text(statement, 0);
		if (res)
			result = (const char*) res;
	}

Done:

	if (statement)
		sqlite3_finalize(statement);

	if (queryStr)
		sqlite3_free(queryStr);

	return result;
}

bool PrefsDb::getPref(const std::string& key,std::string& r_val)
{
	sqlite3_stmt* statement = 0;
	const char* tail = 0;
	bool result = false;

	if (!m_prefsDb || key.empty())
		return result;

	char *queryStr = sqlite3_mprintf("SELECT value FROM Preferences WHERE key=%Q",key.c_str());

	if (!queryStr)
		goto Done;

	if (sqlite3_prepare(m_prefsDb, queryStr, -1, &statement, &tail)) {
		qWarning("Failed to prepare sql statement: %s", queryStr);
		goto Done;
	}

	if (sqlite3_step(statement) == SQLITE_ROW) {
		const unsigned char* res = sqlite3_column_text(statement, 0);
		if (res)
		{
			r_val = (const char*) res;
			result = true;
		}
	}

	Done:

	if (statement)
		sqlite3_finalize(statement);
	if (queryStr)
		sqlite3_free(queryStr);

	return result;
}

std::map<std::string,std::string> PrefsDb::getAllPrefs()
{
	sqlite3_stmt* statement = 0;
	const char* tail = 0;
	int ret = 0;
	std::string query;
	std::map<std::string, std::string> result;

	if (!m_prefsDb)
		return result;

	query = "SELECT * FROM Preferences;";

	ret = sqlite3_prepare(m_prefsDb, query.c_str(), -1, &statement, &tail);
	if (ret) {
		qWarning() << "Failed to prepare sql statement";
		goto Done;
	}

	while ((ret = sqlite3_step(statement)) == SQLITE_ROW) {
		const char* key = (const char*) sqlite3_column_text(statement, 0);
		const char* val = (const char*) sqlite3_column_text(statement, 1);
		if (!key || !val)
			continue;

		result[key] = val;
	}

	Done:

	if (statement)
		sqlite3_finalize(statement);

	return result;
}

int PrefsDb::merge(PrefsDb * p_sourceDb,bool overwriteSameKeys)
{
	if (!p_sourceDb || (p_sourceDb == this))
		return 0;
	return merge(p_sourceDb->m_dbFilename,overwriteSameKeys);
}

int PrefsDb::merge(const std::string& sourceDbFilename,bool overwriteSameKeys)
{
	if (overwriteSameKeys)
	{
		//can use the ATTACH method
		std::string attachCmd = std::string("ATTACH '")+sourceDbFilename+std::string("' AS backupDb;");
		bool sqlOk = runSqlCommand(attachCmd.c_str());
		if (!sqlOk)
		{
			qWarning() << "Failed to run ATTACH cmd to attach [" << sourceDbFilename.c_str() << "] to this db";
			return 0;
		}
		std::string mergeCmd = std::string("INSERT INTO main.Preferences SELECT * FROM backupDb.Preferences;");
		sqlOk = runSqlCommand(mergeCmd.c_str());
		if (!sqlOk)
		{
			qWarning() << "Failed to run INSERT command to merge [" << sourceDbFilename.c_str() << "] into this db";
		}
		else
		{
			qDebug("successfully merged [%s] into this db", sourceDbFilename.c_str());
		}

		closePrefsDb();
		openPrefsDb();
	}
	else
	{
		qWarning() << "Non-destructive merge not yet implemented! Nothing merged";
		return 0;
	}

	return 1;

}

int PrefsDb::copyKeys(PrefsDb * p_sourceDb,const std::list<std::string>& keys,bool overwriteSameKeys)
{
	if (!p_sourceDb || (p_sourceDb == this))
		return 0;
	if (keys.empty())
		return 0;
	if (p_sourceDb->m_prefsDb == 0)
		return 0;

	qDebug("source DB file: [%s] , target DB file: [%s] , overwriteSameKeys = %s",
		p_sourceDb->m_dbFilename.c_str(), m_dbFilename.c_str(),(overwriteSameKeys ? "YES" : "NO"));
	int n=0;
	for (std::list<std::string>::const_iterator it = keys.begin(); it != keys.end();++it)
	{
		std::string val;
		if (p_sourceDb->getPref(*it,val))
		{
			std::string myVal;
			if (!getPref(*it,myVal) || overwriteSameKeys)
			{
				PMLOG_TRACE("copying key,value = ( [%s] , [%s] ) , overwriting [%s] ",
					(*it).c_str(),val.c_str(),myVal.c_str());
				setPref(*it,val);
				++n;
			}
		}
	}
	return n;
}

sqlite3_stmt* PrefsDb::runSqlQuery(const std::string& queryStr)
{
	sqlite3_stmt* statement = 0;
	int ret = 0;
	const char* tail = 0;

	if (!m_prefsDb)
		return 0;

	if (queryStr.empty())
		return 0;

	ret = sqlite3_prepare(m_prefsDb, queryStr.c_str(), -1, &statement, &tail);
	if (ret != SQLITE_OK) {
		qWarning("Failed to prepare sql statement");
		if (statement)
		{
			sqlite3_finalize(statement);
		}
		return 0;
	}

	return statement;
}

bool PrefsDb::runSqlCommand(const std::string& cmdStr)
{
	bool rc = false;
	int ret = 0;
	char * pErrMsg = 0;

	char * queryStr = sqlite3_mprintf("%s",cmdStr.c_str());
	if (!queryStr)
		return false;

	ret = sqlite3_exec(m_prefsDb, queryStr, NULL, NULL, &pErrMsg);
	if (ret) {
		qWarning() << "Failed to execute cmd [" << queryStr << "] - extended error: [" << (pErrMsg ? pErrMsg : "<none>") << "]";
		rc = false;
	}
	else
		rc = true;

	if (queryStr)
		sqlite3_free(queryStr);
	if (pErrMsg)
		sqlite3_free(pErrMsg);
	return rc;
}

//TODO: STILL UNSAFE IF THE KEY HAS SINGLE QUOTES IN IT! (SEE getPref() FOR EXAMPLE OF HOW TO FIX)
std::map<std::string, std::string> PrefsDb::getPrefs(const std::list<std::string>& keys)
{
	sqlite3_stmt* statement = 0;
	const char* tail = 0;
	int ret = 0;
	std::string query;
	std::map<std::string, std::string> result;
	std::list<std::string>::const_iterator it;

	if (!m_prefsDb)
		return result;

	if (keys.empty())
		goto Done;

	query = "SELECT * FROM Preferences WHERE key='";
	query += keys.front() + "'";

	it = keys.begin();
	++it;

	for (; it != keys.end(); ++it)
		query += " OR key='" + (*it) + "'";
	query += ";";

	ret = sqlite3_prepare(m_prefsDb, query.c_str(), -1, &statement, &tail);
	if (ret) {
		qWarning() << "Failed to prepare sql statement";
		goto Done;
	}

	while ((ret = sqlite3_step(statement)) == SQLITE_ROW) {
		const char* key = (const char*) sqlite3_column_text(statement, 0);
		const char* val = (const char*) sqlite3_column_text(statement, 1);
		if (!key || !val)
			continue;

		result[key] = val;
	}

Done:

	if (statement)
		sqlite3_finalize(statement);

	return result;
}

void PrefsDb::openPrefsDb()
{
	if (m_prefsDb)
	{
		//already open
		return;
	}

	gchar* prefsDirPath = g_path_get_dirname(m_dbFilename.c_str());
    if (prefsDirPath) {
        (void) g_mkdir_with_parents(prefsDirPath, 0755);
        g_free(prefsDirPath);
    }

	int ret = sqlite3_open(m_dbFilename.c_str(), &m_prefsDb);
	if (ret) {
		qWarning() << "Failed to open preferences db [" << m_dbFilename.c_str() << "]";
		return;
	}

	if (!checkTableConsistency()) {

		qWarning() << "Failed to create Preferences table";
		sqlite3_close(m_prefsDb);
		m_prefsDb = 0;
		return;
	}

	ret = sqlite3_exec(m_prefsDb,
					   "CREATE TABLE IF NOT EXISTS Preferences "
					   "(key   TEXT NOT NULL ON CONFLICT FAIL UNIQUE ON CONFLICT REPLACE, "
					   " value TEXT);", NULL, NULL, NULL);
	if (ret) {
		qWarning() << "Failed to create Preferences table";
		sqlite3_close(m_prefsDb);
		m_prefsDb = 0;
		return;
	}
}

void PrefsDb::closePrefsDb()
{
	if (!m_prefsDb)
		return;

	(void) sqlite3_close(m_prefsDb);
	m_prefsDb = 0;
}

bool PrefsDb::checkTableConsistency()
{
	if (!m_prefsDb)
		return false;

	int ret;
	std::string query;
	sqlite3_stmt* statement = 0;
	const char* tail = 0;

	if (!integrityCheckDb())
	{
		qCritical("integrity check failed on prefs db and it cannot be recreated");
		return false;
	}

	query = "SELECT value FROM Preferences WHERE key='databaseVersion'";
	ret = sqlite3_prepare(m_prefsDb, query.c_str(), -1, &statement, &tail);
	if (ret) {
		qWarning("Failed to prepare sql statement: %s (%s)",
					  query.c_str(), sqlite3_errmsg(m_prefsDb));
		sqlite3_finalize(statement);
		goto Recreate;
	}

	ret = sqlite3_step(statement);
	sqlite3_finalize(statement);
	if (ret != SQLITE_ROW) {
		// Database not consistent. recreate
		goto Recreate;
	}

	if (!m_standalone)
	{
		// check to see if all the defaults from the s_defaultPrefsFile at least exist and if not, add them
		synchronizeDefaults();
		synchronizePlatformDefaults();

		//check the same with the "customer care" file
		synchronizeCustomerCareInfo();

		updateWithCustomizationPrefOverrides();
	}
	//Everything is now ok.
	return true;

Recreate:

	(void) sqlite3_exec(m_prefsDb, "DROP TABLE Preferences", NULL, NULL, NULL);
	ret = sqlite3_exec(m_prefsDb,
					   "CREATE TABLE Preferences "
					   "(key   TEXT NOT NULL ON CONFLICT FAIL UNIQUE ON CONFLICT REPLACE, "
					   " value TEXT);", NULL, NULL, NULL);
	if (ret) {
		qWarning() << "Failed to create Preferences table";
		return false;
	}

	ret = sqlite3_exec(m_prefsDb, "INSERT INTO Preferences VALUES ('databaseVersion', '1.0')",
					   NULL, NULL, NULL);
	if (ret) {
		qWarning() << "Failed to create Preferences table";
		return false;
	}


	if (!m_standalone)
	{
		loadDefaultPrefs();
		loadDefaultPlatformPrefs();
		updateWithCustomizationPrefOverrides();
	}
	return true;
}

bool PrefsDb::integrityCheckDb()
{
	if (!m_prefsDb)
		return false;

	sqlite3_stmt* statement = 0;
	const char* tail = 0;
	int ret = 0;
	bool integrityOk = false;

	ret = sqlite3_prepare(m_prefsDb, "PRAGMA integrity_check", -1, &statement, &tail);
	if (ret) {
		qCritical() << "Failed to prepare sql statement for integrity_check";
		goto CorruptDb;
	}

	ret = sqlite3_step(statement);
	if (ret == SQLITE_ROW) {
		const unsigned char* result = sqlite3_column_text(statement, 0);
		if (result && strcasecmp((const char*) result, "ok") == 0)
			integrityOk = true;
	}

	sqlite3_finalize(statement);

	if (!integrityOk)
		goto CorruptDb;

	qDebug("Integrity check for database passed");

	return true;

CorruptDb:

	qCritical() << "integrity check failed. recreating database";

	sqlite3_close(m_prefsDb);
	unlink(m_dbFilename.c_str());

	ret = sqlite3_open_v2 (m_dbFilename.c_str(), &m_prefsDb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
	if (ret) {
		qCritical() << "Failed to re-open prefs db at [" << m_dbFilename.c_str() << "]";
		return false;
	}

	return true;
}

void PrefsDb::synchronizeDefaults() {

	JValue root = JDomParser::fromFile(s_defaultPrefsFile);
	if (!root.isObject()) {
		qWarning() << "Failed to load json from the default prefs file:" << s_defaultPrefsFile << ". " << root.errorString().c_str();
		return;
	}

	JValue prefs = root["preferences"];
	if (!prefs.isObject()) {
		qWarning() << "Failed to get valid preferences entry from file";
		return;
	}

	for (JValue::KeyValue pref: prefs.children()) {
		std::string p_cDbv = pref.second.stringify();

		//check the key to see if it exists in the db already
		std::string key = pref.first.asString();
		std::string cv = getPref(key);

		//allow special keys to be overriden
		if ((cv.length() == 0) || ((strncmp(key.c_str(),".sysservice",11) == 0))) {

			Utils::gstring queryStr = g_strdup_printf("INSERT INTO Preferences "
													  "VALUES ('%s', '%s')",
													  key.c_str(), p_cDbv.c_str());

			if (sqlite3_exec(m_prefsDb, queryStr.get(), NULL, NULL, NULL)) {
				qWarning() << "Failed to execute query:" << queryStr.get();
			}
		}
	}
}

void PrefsDb::synchronizePlatformDefaults() {

	JValue root = JDomParser::fromFile(s_defaultPlatformPrefsFile);
	if (!root.isObject()) {
		qWarning() << "Failed to load json from the default platform prefs file: " << s_defaultPlatformPrefsFile;
		return;
	}

	JValue prefs = root["preferences"];
	if (!prefs.isObject()) {
		qWarning() << "Failed to get valid preferences entry from file";
		return;
	}

	for (const JValue::KeyValue pref: prefs.children()) {

		if (!pref.second.isString())
			continue; //TODO: really should delete this key if it is in the database

		std::string p_cDbv = pref.second.asString();

		//check the key to see if it exists in the db already
		std::string key = pref.first.asString();
		std::string cv = getPref(key);

		if (cv.length() == 0) {

			Utils::gstring queryStr = g_strdup_printf("INSERT INTO Preferences "
													  "VALUES ('%s', '%s')",
													  key.c_str(), p_cDbv.c_str());

			if (sqlite3_exec(m_prefsDb, queryStr.get(), NULL, NULL, NULL)) {
				qWarning() << "Failed to execute query:" << queryStr.get();
			}
		}
	}
}

void PrefsDb::synchronizeCustomerCareInfo() {

	JValue root = JDomParser::fromFile(s_custCareNumberFile);
	if (!root.isObject()) {
		qWarning() << "Failed to load json from the customer care file: " << s_custCareNumberFile;
		return;
	}

	JValue prefs = root["preferences"];
	if (!prefs.isObject()) {
		qWarning() << "Failed to get valid preferences entry from file";
		return;
	}

	for (const JValue::KeyValue pref: prefs.children()) {

		if (!pref.second.isString())
			continue; //TODO: really should delete this key if it is in the database

		std::string p_cDbv = pref.second.asString();

		//check the key to see if it exists in the db already
		std::string key = pref.first.asString();
		std::string cv = getPref(key);

		if (cv.length() == 0) {
			Utils::gstring queryStr = g_strdup_printf("INSERT INTO Preferences "
											  "VALUES ('%s', '%s')",
											  key.c_str(), p_cDbv.c_str());

			if (sqlite3_exec(m_prefsDb, queryStr.get(), NULL, NULL, NULL)) {
				qWarning() << "Failed to execute query:" << queryStr.get();
			}
		}
		else if (cv != p_cDbv) {
			//update
			setPref(key.c_str(), p_cDbv.c_str());
		}
	}
}

void PrefsDb::updateWithCustomizationPrefOverrides() {

	JValue root = JDomParser::fromFile(s_customizationOverridePrefsFile);
	if (!root.isObject()) {
		qWarning() << "Failed to load json from the customization's prefs override file: "
				   << s_customizationOverridePrefsFile;
		return;
	}

	JValue prefs = root["preferences"];
	if (!prefs.isObject()) {
		qWarning() << "Failed to get valid preferences entry from file";
		return;
	}

	for (const JValue::KeyValue pref: prefs.children()) {

		if (!pref.second.isString())
			continue; //TODO: really should delete this key if it is in the database

		Utils::gstring queryStr = g_strdup_printf("INSERT INTO Preferences "
										  "VALUES ('%s', '%s')",
										  pref.first.asString().c_str(),
										  pref.second.asString().c_str());

		if (sqlite3_exec(m_prefsDb, queryStr.get(), NULL, NULL, NULL)) {
			qWarning() << "Failed to execute query:" << queryStr.get();
		}
	}
}

static const char* s_DEFAULT_uaString[] =	{"uaString","\"GenericPalmModel\""};
static const char* s_DEFAULT_uaProf[]  	= 	{"uaProf","\"http://downloads.palm.com/profiles/GSM_GenericTreoUaProf.xml\""};
static const char* s_DBNEWTOKEN[] = {".prefsdb.setting.dbReset","\"1\""};

void PrefsDb::loadDefaultPrefs() {

	Utils::gstring queryStr { nullptr };

	JValue root = JDomParser::fromFile(s_defaultPrefsFile);
	if (!root.isObject()) {
		qWarning() << "Failed to load json from the default prefs file: "
				   << s_defaultPrefsFile;
		goto Stage1a;
	}

	{
		JValue prefs = root["preferences"];
		if (!prefs.isObject()) {
			qWarning() << "Failed to get valid preferences entry from file";
			goto Stage1a;
		}

		for (const JValue::KeyValue pref: prefs.children()) {

			queryStr = g_strdup_printf("INSERT INTO Preferences "
									   "VALUES ('%s', '%s')",
									   pref.first.asString().c_str(),
									   pref.second.asString().c_str());

			if (sqlite3_exec(m_prefsDb, queryStr.get(), NULL, NULL, NULL)) {
				qWarning() << "Failed to execute query:" << queryStr.get();
			}
		}
	}

Stage1a:
	// ----------------- Load in the db tokens that let the system service know what restore stage the system is in (after reformats, etc)

	queryStr = g_strdup_printf("INSERT INTO Preferences "
							   "VALUES ('%s', '%s')",
							   s_DBNEWTOKEN[0],s_DBNEWTOKEN[1]);

	if (sqlite3_exec(m_prefsDb, queryStr.get(), NULL, NULL, NULL)) {
		qWarning() << "Failed to execute query:" << queryStr.get();
	}

	//customer care number also...this is in a separate file
	root = JDomParser::fromFile(s_custCareNumberFile);
	if (!root.isObject()) {
		qWarning() << "Failed to load json from the customer care # file: "
				   << s_custCareNumberFile;
		goto Stage3;
	}

	for (const JValue::KeyValue pref: root.children()) {

		if (!pref.second.isString()) continue;

		queryStr = g_strdup_printf("INSERT INTO Preferences "
								   "VALUES ('%s', '%s')",
								   pref.first.asString().c_str(),
								   pref.second.asString().c_str());

		if (sqlite3_exec(m_prefsDb, queryStr.get(), NULL, NULL, NULL)) {
			qWarning() << "Failed to execute query:" << queryStr.get();
			continue;
		}

		qDebug("loaded key %s with value %s", pref.first.asString().c_str(), pref.second.asString().c_str());
	}

Stage3:
	queryStr = g_strdup_printf("INSERT INTO Preferences "
							   "VALUES ('%s', '%s')",
							   s_DEFAULT_uaProf[0],s_DEFAULT_uaProf[1]);

	int ret = sqlite3_exec(m_prefsDb, queryStr.get(), NULL, NULL, NULL);
	if (ret) {
		qWarning() << "[Stage 3] Failed to execute query:" << queryStr.get();
	}

	queryStr = g_strdup_printf("INSERT INTO Preferences "
							   "VALUES ('%s', '%s')",
							   s_DEFAULT_uaString[0],s_DEFAULT_uaString[1]);

	ret = sqlite3_exec(m_prefsDb, queryStr.get(), NULL, NULL, NULL);
	if (ret) {
		qWarning() << "[Stage 3] Failed to execute query:" << queryStr.get();
	}

	//back up the defaults for certain prefs
	backupDefaultPrefs();
	//refresh system restore
	SystemRestore::instance()->refreshDefaultSettings();

}

void PrefsDb::loadDefaultPlatformPrefs() {

	JValue root = JDomParser::fromFile(s_defaultPlatformPrefsFile);
	if (!root.isObject()) {
		qWarning() << "Failed to load json from the platform default prefs file: "
				   << s_defaultPrefsFile;
		return;
	}

	do {
		JValue prefs = root["preferences"];
		if (!prefs.isObject()) {
			qWarning() << "Failed to get valid preferences entry from file";
			break;
		}

		for (const JValue::KeyValue pref: prefs.children()) {

			Utils::gstring queryStr = g_strdup_printf("INSERT INTO Preferences "
											  "VALUES ('%s', '%s')",
											  pref.first.asString().c_str(),
											  pref.second.asString().c_str());

			if (sqlite3_exec(m_prefsDb, queryStr.get(), NULL, NULL, NULL)) {
				qWarning() << "Failed to execute query:" << queryStr.get();
			}
		}
	} while (false);

	//back up the defaults for certain prefs
	backupDefaultPrefs();
	//refresh system restore
	SystemRestore::instance()->refreshDefaultSettings();
}

void PrefsDb::backupDefaultPrefs()
{
	std::string prefStr = getPref("wallpaper");
	setPref(PrefsDb::s_sysDefaultWallpaperKey,prefStr);
	prefStr = getPref("ringtone");
	setPref(PrefsDb::s_sysDefaultRingtoneKey,prefStr);
}
