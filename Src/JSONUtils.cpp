// Copyright (c) 2012-2024 LG Electronics, Inc.
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

#include "JSONUtils.h"

#include <luna-service2++/error.hpp>

bool JsonMessageParser::parse(const char * callerFunction)
{
	if (!mParser.parse(mJson, mSchema))
	{
		const char * errorText = "Could not validate json message against schema";
		if (!mParser.parse(mJson, pbnjson::JSchema::AllSchema()))
			errorText = "Invalid json message";
		PmLogCritical(sysServiceLogContext(), "PARSE_FAILED", 0, "Called by: %s : %s \' %s \' ", callerFunction, errorText, mJson);
		return false;
	}
	return true;
}

pbnjson::JValue createJsonReply(bool returnValue, int errorCode, const char *errorText)
{
	pbnjson::JObject reply {{"returnValue", returnValue}};
	if (errorCode)
		reply.put("errorCode", errorCode);
	if (errorText)
		reply.put("errorText", errorText);
	return reply;
}

LSMessageJsonParser::LSMessageJsonParser(LSMessage *message, const char *schema)
	: mMessage(message)
	, mSchema(pbnjson::JSchema::fromString(schema))
{
}

LSMessageJsonParser::LSMessageJsonParser(LSMessage * message, const pbnjson::JSchema &schema)
	: mMessage(message)
	, mSchema(schema)
{
}

std::string LSMessageJsonParser::getMsgCategoryMethod()
{
	std::string context = "";

	if (mMessage) {
		if (LSMessageGetCategory(mMessage))
			context = "Category: " + std::string(LSMessageGetCategory(mMessage)) + " ";

		if (LSMessageGetMethod(mMessage))
			context += "Method: " + std::string(LSMessageGetMethod(mMessage));
	}

	return context;
}

std::string LSMessageJsonParser::getSender()
{
	std::string strSender = "";

	if (mMessage) {
		PmLogDebug(sysServiceLogContext(),"About to call LSMessageGetSenderServiceName()...");
		const char * sender = LSMessageGetSenderServiceName(mMessage);

		if (sender && *sender) {
			PmLogDebug(sysServiceLogContext(),"About to call LSMessageGetSender()...");
			if (LSMessageGetSender(mMessage)) {
				strSender = std::string(LSMessageGetSender(mMessage));
				PmLogDebug(sysServiceLogContext(),"sender: %s", strSender.c_str());
			}
		}
	}

	return strSender;
}

bool LSMessageJsonParser::parse(const char * callerFunction, LSHandle * lssender, ESchemaErrorOptions validationOption)
{
	if (EIgnore == validationOption) return true;

	const char * payload = getPayload();

	// Parse the message with given schema.
	if ((payload) && (!mParser.parse(payload, mSchema)))
	{
		// Unable to parse the message with given schema

		const char *    errorText = "Could not validate json message against schema";

		// Try parsing the message with empty schema, just to verify that it is a valid json message
		if (!mParser.parse(payload, pbnjson::JSchema::AllSchema()))
		{
			PmLogWarning(sysServiceLogContext(), "JSON_ERROR", 0, "[JSON Error] : [%s : %s]: The message '%s' sent by '%s' is not a valid json message", callerFunction, getMsgCategoryMethod().c_str(), payload, getSender().c_str());
			errorText = mParser.getError(); // invalid json message
		}
		else
		{
			PmLogCritical(sysServiceLogContext(), "PARSE_FAILED", 0, "[Schema Error] : [%s :%s]: Could not validate json message '%s' sent by '%s' against schema.", callerFunction, getMsgCategoryMethod().c_str(), payload, getSender().c_str());
		}

		if (EValidateAndError == validationOption || EValidateAndErrorAlways == validationOption)
		{
			if ((lssender) && (validationOption == EValidateAndErrorAlways || !getSender().empty()))
			{
				LS::Error error;
				std::string reply = createJsonReply(false, 1, errorText).stringify();
				if (!LSMessageReply(lssender, mMessage, reply.c_str(), error))
				{
					PmLogCritical(sysServiceLogContext(), "LSMESSAGEREPLY_FAILED", 0, "%s(%d) Luna Service Reply Error\"%s\"",
							  __FILE__, __LINE__, error.what());
				}
			}

			return false; // throw the error back
		}
	}

	// Message successfully parsed with given schema
	return true;
}
