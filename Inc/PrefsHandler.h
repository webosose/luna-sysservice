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


#ifndef PREFSHANDLER_H
#define PREFSHANDLER_H

#include <string>
#include <list>
#include <map>

#include <luna-service2/lunaservice.h>
#include <pbnjson.hpp>

#include "Logging.h"

class PrefsHandler
{
public:

	PrefsHandler(LSHandle* serviceHandle) : m_serviceHandle(serviceHandle){}
	virtual ~PrefsHandler() {}

	virtual std::list<std::string> keys() const = 0;
	virtual bool validate(const std::string& key, const pbnjson::JValue &value) = 0;
	virtual bool validate(const std::string& key, const pbnjson::JValue &value, const std::string& originId)
	{ return validate(key,value); }
	virtual void valueChanged(const std::string& key, const pbnjson::JValue &value) = 0;
	virtual void valueChanged(const std::string& key,const std::string& strval)
	{
		using namespace pbnjson;

		if (strval.empty())
			return;
		//WORKAROUND WRAPPER FOR USING valueChanged() internally.  //TODO: do this the proper way.
		// the way it is now makes a useless conversion at least once
		JValue jo = JDomParser::fromString(strval);
		if (!jo.isValid())
		{
			PmLogError(
				sysServiceLogContext(), "INVALID_PREF_VALUE", 2,
				PMLOGKS("KEY", key.c_str()),
				PMLOGKS("VALUE", strval.c_str()),
				"Can't parse value as json to set preferences, despite is was validated."
			);
			return;
		}

		valueChanged(key,jo);
	}
	virtual pbnjson::JValue valuesForKey(const std::string& key) = 0;
	// FIXME: We very likely need a windowed version the above function
	virtual bool isPrefConsistent() { return true; }
	virtual void restoreToDefault() {}
	virtual bool shouldRefreshKeys(std::map<std::string,std::string>& keyvalues) { return false;}

	LSHandle * getServiceHandle() { return m_serviceHandle;}

protected:

	LSHandle*	m_serviceHandle;
};

#endif /* PREFSHANDLER_H */
