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

#include "DeviceInfoService.h"

#include <pbnjson.hpp>
#include <luna-service2++/error.hpp>

#include "Logging.h"

using namespace pbnjson;

LSMethod s_device_methods[]  = {
	{ "query",  DeviceInfoService::cbGetDeviceInformation },
	{ 0, 0 },
};

/*! \page com_palm_device_info_service Service API com.webos.service.systemservice/deviceInfo/
 *
 *  Public methods:
 *   - \ref device_info_query
 */

const DeviceInfoService::command_map_t& DeviceInfoService::getCommandMap()
{
	static command_map_t map =
	{
		{"board_type", NYX_DEVICE_INFO_BOARD_TYPE}, //Return board type
		{"bt_addr", NYX_DEVICE_INFO_BT_ADDR}, //Return Bluetooth address
		{"device_name", NYX_DEVICE_INFO_DEVICE_NAME}, //Return device name
		{"hardware_id", NYX_DEVICE_INFO_HARDWARE_ID}, //Return hardware ID
		{"hardware_revision", NYX_DEVICE_INFO_HARDWARE_REVISION}, //Return hardware revision
		{"installer", NYX_DEVICE_INFO_INSTALLER}, //Return installer
		{"keyboard_type", NYX_DEVICE_INFO_KEYBOARD_TYPE}, //Return keyboard type
		{"modem_present", NYX_DEVICE_INFO_MODEM_PRESENT}, //Return modem availability
		{"nduid", NYX_DEVICE_INFO_NDUID}, //Return NDUID
		{"product_id", NYX_DEVICE_INFO_PRODUCT_ID}, //Return product ID
		{"radio_type", NYX_DEVICE_INFO_RADIO_TYPE}, //Return radio type
		{"ram_size", NYX_DEVICE_INFO_RAM_SIZE}, //Return RAM size
		{"serial_number", NYX_DEVICE_INFO_SERIAL_NUMBER}, //Return serial number
		{"storage_free", NYX_DEVICE_INFO_STORAGE_FREE}, //Return free storage size
		{"storage_size", NYX_DEVICE_INFO_STORAGE_SIZE}, //Return storage size
		{"wifi_addr", NYX_DEVICE_INFO_WIFI_ADDR}, //Return WiFi MAC address
		{"last_reset_type", NYX_DEVICE_INFO_LAST_RESET_TYPE}, //Reason code for last reboot (may come from /proc/cmdline)
		{"battery_challange", NYX_DEVICE_INFO_BATT_CH}, //Battery challenge
		{"battery_response", NYX_DEVICE_INFO_BATT_RSP}, //Battery response
		{"wired_addr", NYX_DEVICE_INFO_WIRED_ADDR}, //Return Wired MAC address
	};

	return map;
}

void DeviceInfoService::setServiceHandle(LSHandle* serviceHandle)
{
	LS::Error error;
	if (!LSRegisterCategory(serviceHandle, "/deviceInfo",
		s_device_methods, nullptr, nullptr, error.get()))
	{
		PmLogCritical(sysServiceLogContext(), "LSREGISTERCATEGORY_FAILED", 0, "Failed in registering deviceinfo handler method:%s", error.what());
	}
}

/*!
\page com_palm_device_info_service
\n
\section device_info_query query

\e Private. Available only at the private bus.

com.webos.service.systemservice/deviceInfo/query

\subsection device_info_query_syntax Syntax:
\code
{
	"parameters": [string array]
}
\endcode

\param parameters List of requested parameters. If not specified, all available parameters wiil be returned. 

\subsection os_info_query_return Returns:
\code
{
	"returnValue": boolean,
	"errorCode": string
	"board_type": string
	"bt_addr": string
	"device_name": string
	"hardware_id": string
	"hardware_revision": string
	"installer": string
	"keyboard_type": string
	"modem_present": string
	"nduid": string
	"product_id": string
	"radio_type": string
	"ram_size": string
	"serial_number": string
	"storage_free": string
	"storage_size": string
	"wifi_addr": string
	"last_reset_type": string
	"battery_challange": string
	"battery_response": string
}
\endcode

\param returnValue Indicates if the call was succesful.
\param errorCode Description of the error if call was not succesful.
\param board_type Board type
\param bt_addr Bluetooth address
\param device_name Device name
\param hardware_id Hardware ID
\param hardware_revision Hardware revision
\param installer Installer
\param keyboard_type Keyboard type
\param modem_present Modem availability
\param nduid NDUID
\param product_id Product ID
\param radio_type Radio type
\param ram_size RAM size
\param serial_number Serial number
\param storage_free Free storage size
\param storage_size Storage size
\param wifi_addr WiFi MAC address
\param last_reset_type Reason code for last reboot (may come from /proc/cmdline)
\param battery_challange Battery challenge
\param battery_response Battery response

All listed parameters can have `not supported` value, if not supported by the device.

\subsection device_info_qeury_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.systemservice/deviceInfo/query '{"parameters":["device_name", "storage_size"]}'
\endcode

Example response for a succesful call:
\code
{
	"device_name": "qemux86",
	"storage_size": "32 GB",
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
bool DeviceInfoService::cbGetDeviceInformation(LSHandle* lsHandle, LSMessage *message, void *user_data)
{
    JObject reply;
    nyx_device_handle_t device = nullptr;

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
                PmLogCritical(sysServiceLogContext(), "NYX_INIT_FAILED", 0, "Failed to inititalize nyx library: %d", error);
            reply = JObject { { "returnValue", false }, { "errorText",
                    "Internal error. Can't initialize nyx" } };
            break;
        }

        error = nyx_device_open(NYX_DEVICE_DEVICE_INFO, "Main", &device);
            if ((NYX_ERROR_NONE != error) || (NULL == device))
            {
                PmLogCritical(sysServiceLogContext(), "NYX_DEVICE_OPEN_FAILED", 0, "Failed to get `Main` nyx device: %d ", error);
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
            // Some device don't have all available parameters. We will just ignore them.
            error = nyx_device_info_query(device, query->second, &nyx_result);
            if (NYX_ERROR_NONE == error) {
                reply.put(param, nyx_result);
            } else {
                reply.put(param, "not supported");
            }
        }

        reply.put("returnValue", true);
    } while (false);

    LS::Error error;
    if(!LSMessageReply(lsHandle, message, reply.stringify().c_str(), error))
    {
        PmLogWarning(sysServiceLogContext(), "LS_REPLY_ERROR", 0,"%s",error.what());
    }

    if (NULL != device)
        nyx_device_close(device);
    nyx_deinit();

    return true;
}
