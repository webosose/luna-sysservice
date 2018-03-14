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

#ifndef PREFSFACTORY_H
#define PREFSFACTORY_H

#include <map>
#include <string>
#include <memory>

#include "Singleton.h"

struct LSHandle;

class PrefsHandler;

class PrefsFactory : public Singleton<PrefsFactory>
{

	friend class Singleton<PrefsFactory>;

public:
	enum Errors
	{
		ErrorNone,            // not an error
		ErrorPrefDoesntExist, // preference by key doesn't exist
		ErrorValuesDontExist, // values for key don't exist
	};

	typedef std::shared_ptr<PrefsHandler> PrefsHandlerPtr;
	typedef std::map<std::string, PrefsHandlerPtr> PrefsHandlerMap;

	void setServiceHandle(LSHandle* serviceHandle);
	LSHandle* getServiceHandle() const { return m_serviceHandle; }

	std::shared_ptr<PrefsHandler> getPrefsHandler(const std::string& key) const;
	
	void postPrefChange(const std::string& key,const std::string& value);
	void postPrefChangeValueIsCompleteString(const std::string& key,const std::string& json_string);
	void runConsistencyChecksOnAllHandlers();
	
	void refreshAllKeys();		//useful for when the database is completely restored to another version
								//at some point after sysservice startup (see BackupManager)
private:
	PrefsFactory();

	void init();
	void registerPrefHandler(const PrefsHandlerPtr &handler);
	
private:

	LSHandle* m_serviceHandle;
		
	PrefsHandlerMap m_handlersMaps;
};

#endif /* PREFSFACTORY_H */
