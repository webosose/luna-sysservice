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


#include "Logging.h"
#include "PrefsDb.h"
#include "Utils.h"

#include "LocalePrefsHandler.h"

using namespace pbnjson;

static const char* s_logChannel = "LocalePrefsHandler";
static const char* s_defaultLocaleFile = WEBOS_INSTALL_WEBOS_SYSCONFDIR "/locale.txt";
static const char* s_custLocaleFile = WEBOS_INSTALL_SYSMGR_DATADIR "/customization/locale.txt";
static const char* s_defaultRegionFile = WEBOS_INSTALL_WEBOS_SYSCONFDIR "/region.json";
static const char* s_custRegionFile = WEBOS_INSTALL_SYSMGR_DATADIR "/customization/region.json";

LocalePrefsHandler::LocalePrefsHandler(LSHandle* serviceHandle)
	: PrefsHandler(serviceHandle)
{
	init();
}

LocalePrefsHandler::~LocalePrefsHandler()
{
}

std::list<std::string> LocalePrefsHandler::keys() const
{
	std::list<std::string> k;
	k.push_back("locale");
	k.push_back("region");
	return k;
}

bool LocalePrefsHandler::validateLocale(const JValue &value)
{
	if (value.isObject())
		return false;

	std::string languageCode;
	std::string countryCode;

	JValue label = value["languageCode"];
	if (!label.isString()) {
		//luna_warn(s_logChannel, "Failed to find param languageCode");
		qWarning() << "Failed to find param languageCode";
	}
	else {
		languageCode = label.asString();
	}

	label = value["countryCode"];
	if (!label.isString()) {
		//luna_warn(s_logChannel, "Failed to find param countryCode");
		qWarning() << "Failed to find param countryCode";
	}
	else {
		countryCode = label.asString();
	}

	if (!languageCode.empty() && !countryCode.empty()) {
		bool found = false;

		for (LocaleEntryList::const_iterator it = m_localeEntryList.begin();
		it != m_localeEntryList.end(); ++it) {

			const LocaleEntry& locEntry = (*it);
			if (locEntry.language.second == languageCode) {

				for (NameCodePairList::const_iterator iter = locEntry.countries.begin();
				iter != locEntry.countries.end(); ++iter) {

					const NameCodePair& countryEntry = (*iter);
					if (countryEntry.second == countryCode) {
						found = true;
						break;
					}
				}
				if (found)
					break;
			}
		}
		if (!found)
			return false;
	}

	return true;

}

bool LocalePrefsHandler::validateRegion(const JValue &value)
{
	if (!value.isObject())
		return false;

	std::string regionCode;

	JValue label = value["countryCode"];
	if (!label.isString()) {
		//luna_warn(s_logChannel, "Failed to find param regionCode");
		qWarning() << "Failed to find param regionCode";
	}
	else {
		regionCode = label.asString();
	}

	if (!regionCode.empty()) {
		bool found = false;

		for (RegionEntryList::const_iterator it = m_regionEntryList.begin();
		it != m_regionEntryList.end(); ++it) {

			const RegionEntry& rEntry = (*it);
			if (rEntry.region[2] == regionCode) {
				found=true;
				break;
			}
		}
		if (!found)
			return false;
	}

	return true;

}

bool LocalePrefsHandler::validate(const std::string& key, const pbnjson::JValue &value)
{

	if (key == "locale")
		return validateLocale(value);
	else if (key == "region")
		return validateRegion(value);

	return false;
}

void LocalePrefsHandler::valueChangedLocale(const JValue &)
{
	// nothing to do
}

void LocalePrefsHandler::valueChangedRegion(const JValue &)
{
	// nothing to do
}

void LocalePrefsHandler::valueChanged(const std::string& key, const JValue &value)
{
	// We will assume that the value has been validated
	if (key == "locale")
		valueChangedLocale(value);
	else if (key == "region")
		valueChangedRegion(value);
}

JValue LocalePrefsHandler::valuesForLocale()
{
	JObject json;
	JArray langArrayObj;

	for (LocaleEntryList::const_iterator it = m_localeEntryList.begin();
	it != m_localeEntryList.end(); ++it) {

		const LocaleEntry& locEntry = (*it);
		JObject langObj {{"languageName", locEntry.language.first},
		                 {"languageCode", locEntry.language.second}};

		JArray countryArrayObj;
		for (NameCodePairList::const_iterator iter = locEntry.countries.begin();
		iter != locEntry.countries.end(); ++iter) {
			JObject countryObj {{"countryName", (*iter).first.c_str()},
			                    {"countryCode", (*iter).second.c_str()}};
			countryArrayObj.append(countryObj);
		}

		langObj.put("countries", countryArrayObj);
		langArrayObj.append(langObj);
	}

	json.put("locale", langArrayObj);

	return json;
}

JValue LocalePrefsHandler::valuesForRegion()
{
	JObject json;
	JArray regArrayObj;

	for (RegionEntryList::const_iterator it = m_regionEntryList.begin();
	it != m_regionEntryList.end(); ++it) {

		const RegionEntry& regEntry = (*it);
		JObject regObj = {{"shortCountryName", regEntry.region[0]},
		                  {"countryName", regEntry.region[1]},
		                  {"countryCode", regEntry.region[2]}};

		regArrayObj.append(regObj);
	}

	json.put("region", regArrayObj);
	
	return json;
}
	
JValue LocalePrefsHandler::valuesForKey(const std::string& key)
{
	if (key == "locale")
		return valuesForLocale();
	else if (key == "region")
		return valuesForRegion();
	else
		return JObject();
}

void LocalePrefsHandler::init()
{
	readCurrentLocaleSetting();
	readCurrentRegionSetting();
	readLocaleFile();
	readRegionFile();
}

void LocalePrefsHandler::readCurrentRegionSetting()
{
	std::string region = PrefsDb::instance()->getPref("region");

	if (!region.empty()) {

		JValue json = JDomParser::fromString(region);
		if (json.isObject()) {

			JValue label = json["countryCode"];
			if (label.isString()) {

				m_regionCode = label.asString();
				return;
			}
		}
	}

	m_regionCode = "us";
}

void LocalePrefsHandler::readCurrentLocaleSetting()
{
	std::string locale = PrefsDb::instance()->getPref("locale");
	
	if (!locale.empty()) {

		JValue json = JDomParser::fromString(locale);
		if (json.isObject()) {

			JValue lc = json["languageCode"];
			JValue cc = json["countryCode"];
			if (lc.isString() && cc.isString()) {

				m_languageCode = lc.asString();
				m_countryCode = cc.asString();
				return;
			}
		}
	}

	m_languageCode = "en";
	m_countryCode = "us";
}

void LocalePrefsHandler::readLocaleFile()
{
	// Read the locale file
	JValue root = JDomParser::fromFile(s_custLocaleFile);
	if (!root.isObject())
		root = JDomParser::fromFile(s_defaultLocaleFile);
	if (!root.isObject()) {
		qCritical() << "Failed to load locale files: [" << s_custLocaleFile << "] nor [" << s_defaultLocaleFile << "]";
		return;
	}

	JValue locale = root["locale"];
	if (!locale.isArray()) {
		qCritical() << "Failed to get locale array from locale file";
		return;
	}

	for (const JValue loc: locale.items()) {

		LocaleEntry localeEntry;

		JValue label = loc["languageName"];
		if (!label.isString()) continue;
		localeEntry.language.first = label.asString();

		label = loc["languageCode"];
		if (!label.isString()) continue;
		localeEntry.language.second = label.asString();

		JValue countries = loc["countries"];
		if (!countries.isArray()) continue;

		for (const JValue cnt: countries.items()) {
			NameCodePair country;

			label = cnt["countryName"];
			if (!label.isString()) continue;
			country.first = label.asString();

			label = cnt["countryCode"];
			if (!label.isString()) continue;
			country.second = label.asString();

			localeEntry.countries.push_back(country);
		}

		m_localeEntryList.push_back(localeEntry);
	}
}

void LocalePrefsHandler::readRegionFile() 
{
	// Read the locale file
	JValue root = JDomParser::fromFile(s_custRegionFile);
	if (!root.isObject())
		root = JDomParser::fromFile(s_defaultRegionFile);
	if (!root.isObject()) {
		qCritical() << "Failed to load region files: [" << s_custRegionFile << "] nor [" << s_defaultRegionFile << "]";
		return;
	}

	JValue regionArray = root["region"];
	if (!regionArray.isArray()) {
		qCritical() << "Failed to get region array from region file";
		return;
	}

	for (const JValue rgn: regionArray.items()) {

		RegionEntry regionEntry;

		JValue label = rgn["countryName"];
		if (!label.isString()) continue;
		regionEntry.region[1] = label.asString();

		label = rgn["shortCountryName"];
		if (!label.isString())
			regionEntry.region[0] = regionEntry.region[1];
		else
			regionEntry.region[0] = label.asString();

		label = rgn["countryCode"];
		if (!label.isString()) continue;
		regionEntry.region[2] = label.asString();

		m_regionEntryList.push_back(regionEntry);
	}
}

std::string LocalePrefsHandler::currentLocale() const
{
	return m_languageCode + "_"  + m_countryCode;
}

std::string LocalePrefsHandler::currentRegion() const
{
	return m_regionCode;
}

