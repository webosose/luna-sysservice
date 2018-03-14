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

#include "Settings.h"

#include <memory>
#include <algorithm>

#include "Utils.h"

static const char* kSettingsFile = WEBOS_INSTALL_WEBOS_SYSCONFDIR "/sysservice.conf";
static const char* kSettingsFilePlatform = WEBOS_INSTALL_WEBOS_SYSCONFDIR "/sysservice-platform.conf";

Settings::Settings()
	: schemaValidationOption(EIgnore)
	, m_turnNovacomOnAtStartup(false)
	, m_saveLastBackedUpTempDb(false)
	, m_saveLastRestoredTempDb(false)
	, m_logLevel()
	, m_useComPalmImage2(false)
	, m_image2svcAvailable(false)
	, m_comPalmImage2BinaryFile("/usr/bin/acuteimaging")
	, switchTimezoneOnManualTime(false)
        , useLocalizedTZ(false)
{
	(void)load(kSettingsFile);
	(void)load(kSettingsFilePlatform);
}

#define KEY_STRING(cat,name,var) \
{\
	gchar* _vs;\
	GError* _error = 0;\
	_vs=g_key_file_get_string(keyfile,cat,name,&_error);\
	if( !_error && _vs ) { var=(const char*)_vs; g_free(_vs); }\
	else g_error_free(_error); \
}

#define KEY_BOOLEAN(cat,name,var) \
{\
	gboolean _vb;\
	GError* _error = 0;\
	_vb=g_key_file_get_boolean(keyfile,cat,name,&_error);\
	if( !_error ) { var=_vb; }\
	else g_error_free(_error); \
}

#define KEY_SCHEMA_ERR_OPTION(cat,name,var) \
{\
	int _v;\
	GError* _error = 0;\
	_v=g_key_file_get_integer(keyfile,cat,name,&_error);\
	if( !_error ) { var=(ESchemaErrorOptions)_v; }\
	else g_error_free(_error); \
}

#define KEY_DOUBLE(cat,name,var) \
{\
	double _v;\
	GError* _error = 0;\
	_v=g_key_file_get_double(keyfile,cat,name,&_error);\
	if( !_error ) { var=_v; }\
	else g_error_free(_error); \
}


bool Settings::load(const char* settingsFile)
{
	GKeyFile* keyfile;
	GKeyFileFlags flags;
	GError* error = 0;

	keyfile = g_key_file_new();
	if(!keyfile)
		return false;
	flags = GKeyFileFlags( G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS);

	if( !g_key_file_load_from_file( keyfile, settingsFile, flags, &error ) )
	{
		g_key_file_free( keyfile );
		if (error) g_error_free(error);
		return false;
	}

	KEY_BOOLEAN("Debug","turnOnNovacomAtStart",m_turnNovacomOnAtStartup);
	KEY_BOOLEAN("Debug","saveLastBackedUpTempDb",m_saveLastBackedUpTempDb);
	KEY_BOOLEAN("Debug","saveLastRestoredTempDb",m_saveLastRestoredTempDb);
	KEY_STRING("Debug","logLevel",m_logLevel);

	KEY_BOOLEAN("ImageService","useComPalmImage2",m_useComPalmImage2);
	KEY_STRING("ImageService","comPalmImage2Binary",m_comPalmImage2BinaryFile);

	KEY_SCHEMA_ERR_OPTION("General", "schemaValidationOption", schemaValidationOption);
	KEY_BOOLEAN("General", "switchTimezoneOnManualTime", switchTimezoneOnManualTime);

	g_key_file_free( keyfile );
	return true;
}

bool Settings::parseCommandlineOptions(int argc, char** argv)
{
	gchar* s_logLevelStr = NULL;

	std::unique_ptr<GError*, void(*)(GError**)>
			error(nullptr, [](GError** ptr) { g_error_free(*ptr); });
	std::unique_ptr<GOptionContext, void(*)(GOptionContext*)>
			context(g_option_context_new(nullptr), g_option_context_free);

	static GOptionEntry entries[] = {
		{ "logger", 'l', 0, G_OPTION_ARG_STRING,  &s_logLevelStr, "log level", "level"},
		{ NULL }
	};

	g_option_context_add_main_entries(context.get(), entries, nullptr);
	if (!g_option_context_parse (context.get(), &argc, &argv, error.get()))
	{
		g_printerr("Error: %s\n", (*error.get())->message);
		return false;
	}

	m_logLevel = s_logLevelStr ? s_logLevelStr : "";
	std::transform(m_logLevel.begin(), m_logLevel.end(), m_logLevel.begin(), ::tolower);

	return true;
}
