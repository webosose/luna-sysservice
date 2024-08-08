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

#ifndef SETTINGS_H
#define SETTINGS_H

#include <string>

#include <glib.h>

#include "Singleton.h"

/**
  * Schema Error Options
  */
enum ESchemaErrorOptions
{
	EIgnore = 0,            /**< Ignore the schema */
	EValidateAndContinue,   /**< Validate, Log the error & Continue */
	EValidateAndError,      /**< Validate, Log the error & Reply with correct schema */
	EValidateAndErrorAlways, /**< Validate, Log the error & Reply with correct schema (even to empty sender) */
	EDefault                /**< Default, loads the value from settings (luna.conf) file  */
};

class Settings : public Singleton<Settings>
{
	friend class Singleton<Settings>;

public:
	bool parseCommandlineOptions(int argc, char** argv);

public:
	bool	m_turnNovacomOnAtStartup;
	bool	m_saveLastBackedUpTempDb;
	bool	m_saveLastRestoredTempDb;
	std::string m_logLevel;

	bool	m_useComPalmImage2;
	bool	m_image2svcAvailable;
	std::string m_comPalmImage2BinaryFile;

	ESchemaErrorOptions schemaValidationOption;
	bool	switchTimezoneOnManualTime;
	bool	useLocalizedTZ;

private:
	Settings();

	bool load(const char* settingsFile);
};

#endif // SETTINGS_H
