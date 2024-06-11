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

#include "OsInfoService.h"

#include <pbnjson.hpp>
#include <luna-service2++/error.hpp>

#include "JSONUtils.h"
#include "Logging.h"

using namespace pbnjson;

LSMethod s_os_methods[]  = {
	{ "query",  OsInfoService::cbGetOsInformation },
	{ 0, 0 },
};

/*! \page com_palm_os_info_service Service API com.webos.service.systemservice/osInfo/
 *
 *  Public methods:
 *   - \ref os_info_query
 */

OsInfoService::OsInfoService()
{
}

const OsInfoService::command_map_t& OsInfoService::getCommandMap()
{
	static command_map_t map =
	{
		{"core_os_kernel_version", NYX_OS_INFO_CORE_OS_KERNEL_VERSION}, // Return Core OS kernel version info
		{"core_os_name", NYX_OS_INFO_CORE_OS_NAME}, // Return Core OS name
		{"core_os_release", NYX_OS_INFO_CORE_OS_RELEASE}, // Return Core OS release info
		{"core_os_release_codename", NYX_OS_INFO_CORE_OS_RELEASE_CODENAME}, // Return Core OS release codename
		{"webos_api_version", NYX_OS_INFO_WEBOS_API_VERSION}, // Return webOS API version
		{"webos_build_datetime", NYX_OS_INFO_WEBOS_BUILD_DATETIME}, // Return UTC timestamp for the current build
		{"webos_build_id", NYX_OS_INFO_WEBOS_BUILD_ID}, // Return webOS build ID
		{"webos_imagename", NYX_OS_INFO_WEBOS_IMAGENAME}, // Return webOS imagename
		{"webos_name", NYX_OS_INFO_WEBOS_NAME}, // Return webOS name
		{"webos_prerelease", NYX_OS_INFO_WEBOS_PRERELEASE}, // Return webOS prerelease info
		{"webos_release", NYX_OS_INFO_WEBOS_RELEASE}, // Return webOS release info
		{"webos_release_codename", NYX_OS_INFO_WEBOS_RELEASE_CODENAME}, // Return webOS release codename
		{"webos_manufacturing_version", NYX_OS_INFO_MANUFACTURING_VERSION}, // Return webOS manufacting version
		{"encryption_key_type", NYX_OS_INFO_ENCRYPTION_KEY_TYPE} // Return encryption key type
	};

	return map;
}

void OsInfoService::setServiceHandle(LSHandle* serviceHandle)
{
	LS::Error error;
	if (!LSRegisterCategory(serviceHandle, "/osInfo", s_os_methods, nullptr, nullptr, error.get()))
	{
		PmLogCritical(sysServiceLogContext(), "FAILED_TO_REGISTER", 0, "Failed in registering osinfo handler method:%s", error.what());
	}
}

/*!
\page com_palm_os_info_service
\n
\section os_info_query query

\e Public.

com.webos.service.systemservice/osInfo/query

\subsection os_info_query_syntax Syntax:
\code
{
	 "parameters": [string array]
}
\endcode

\param parameters List of requested parameters. If not specified, all available parameters will be returned.

\subsection os_info_query_return Returns:
\code
{
	"returnValue": boolean,
	"errorCode": string
	"core_os_kernel_version": string
	"core_os_name": string
	"core_os_release": string
	"core_os_release_codename": string
	"webos_api_version": string
	"webos_build_id": string
	"webos_build_datetime": string,
	"webos_imagename": string
	"webos_name": string
	"webos_prerelease": string
	"webos_release": string
	"webos_release_codename": string
	"webos_manufacturing_version": string
}
\endcode

\param returnValue Indicates if the call was succesful.
\param errorCode Description of the error if call was not succesful.
\param core_os_kernel_version Core OS kernel version info
\param core_os_name Core OS name
\param core_os_release Core OS release info
\param core_os_release_codename Core OS release codename
\param webos_api_version webOS API version
\param webos_build_id webOS build ID
\param webos_build_datetime UTC timestamp for the running build (YYYYMMDDhhmmss)
\param webos_imagename webOS imagename
\param webos_name webOS name
\param webos_prerelease webOS prerelease info
\param webos_release webOS release info
\param webos_release_codename webOS release codename
\param webos_manufacturing_version webOS manufacting version

\subsection os_info_query_examples Examples:
\code
luna-send-pub -n 1 -f luna://com.webos.service.systemservice/osInfo/query '{"parameters":["core_os_name", "webos_release"]}'
\endcode

Example response for a succesful call:
\code
{
	"core_os_name": "Linux",
	"webos_release": "0.10",
	"returnValue": true
}
\endcode

Example response for a failed call:
\code
{
	"errorCode": "Cannot parse json payload"
	"returnValue": false,
}
\endcode
*/
bool OsInfoService::cbGetOsInformation(LSHandle* lsHandle, LSMessage *message, void *user_data)
{
    JObject reply;
    nyx_device_handle_t device = nullptr;
    reply.put("returnValue", true);
    do {
        auto payload = LSMessageGetPayload(message);

        if (!payload)
            break;

        JValue payloadObj = JDomParser::fromString(payload);
        if (!payloadObj.isObject()) {
            reply = JObject { { "returnValue", false }, { "errorText",
                    "Invalid message payload" } };
            break;
        }

        JValue params = payloadObj["parameters"];
        if (params.isValid()) {
            if (!params.isArray()) {
                reply = JObject { { "returnValue", false }, { "errorText",
                        "`parameters` needs to be an array" } };
                break;
            }
        } else {
            // No parameters, lets fill it with all existing keys
            params = JArray();
            for (const auto &elem : getCommandMap())
                params.append(elem.first);
        }

        nyx_error_t error = nyx_init();
        if (NYX_ERROR_NONE != error)
        {
            PmLogCritical(sysServiceLogContext(), "FAILED_TO_INITITALIZE", 0, "Failed to inititalize nyx library: %d", error);
            reply = JObject { { "returnValue", false }, { "errorText",
                    "Internal error. Can't initialize nyx" } };
            break;
        }

        error = nyx_device_open(NYX_DEVICE_OS_INFO, "Main", &device);
        if ((NYX_ERROR_NONE != error) || (NULL == device))
        {
            PmLogCritical(sysServiceLogContext(), "FAILED_TO_GET_DEVICE", 0, "Failed to get `Main` nyx device: %d ", error);
            reply = JObject { { "returnValue", false }, { "errorText",
                    "Internal error. Can't open nyx device" } };
            break;
        }

        for (JValue param : params.items()) {
            auto query = getCommandMap().find(param.asString());
            if (query == getCommandMap().end()) {
                reply = JObject { { "returnValue", false }, { "errorText",
                        "Invalid parameter: " + param.stringify() } };
                break;
            }

            const char *nyx_result = nullptr;
            error = nyx_os_info_query(device, query->second, &nyx_result);
            if (NYX_ERROR_NONE != error)
            {
                PmLogCritical(sysServiceLogContext(), "FAILED_TO_QUERY", 0, "Failed to query nyx. Parameter: %s. Error: %d", param.stringify().c_str(), error);
                reply = JObject { { "returnValue", false }, { "errorText",
                        "Can't get OS parameter: " + param.stringify() } };
                break;
            }

            reply.put(param.asString(), nyx_result);
        }
    } while (false);

    LS::Error error;
    if(!LSMessageReply(lsHandle, message, reply.stringify().c_str(), error))
    {
        PmLogWarning(sysServiceLogContext(), "LS_REPLY_FAIL", 0,  "Failed to send LS reply: %s" , error.what());
    }

    if (NULL != device)
        nyx_device_close(device);
    nyx_deinit();

    return true;
}
