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

#ifndef JSONUTILS_H
#define JSONUTILS_H

#include "Logging.h"
#include "Settings.h"

#include <luna-service2/lunaservice.h>
#include <pbnjson.hpp>

/*
 * Helper macros to build schemas in a more reliable, readable & editable way in C++
 */
#define STRINGIFY(content...) #content
#define JSON(content...) STRINGIFY(content)

/**
  * Json Online Schema Validator : http://jsonlint.com/
  * http://davidwalsh.name/json-validation
  */

// Build a schema as a const char * string without any execution overhead
#define STRICT_SCHEMA(attributes)               "{\"type\":\"object\"" attributes ",\"additionalProperties\":false}"
#define RELAXED_SCHEMA(attributes)              "{\"type\":\"object\"" attributes ",\"additionalProperties\":true}"

#define PROPS_1(p1)                             ",\"properties\":{" p1 "}"
#define PROPS_2(p1, p2)                         ",\"properties\":{" p1 "," p2 "}"
#define PROPS_3(p1, p2, p3)                     ",\"properties\":{" p1 "," p2 "," p3 "}"
#define PROPS_4(p1, p2, p3, p4)                 ",\"properties\":{" p1 "," p2 "," p3 "," p4 "}"
#define PROPS_5(p1, p2, p3, p4, p5)             ",\"properties\":{" p1 "," p2 "," p3 "," p4 "," p5 "}"
#define PROPS_6(p1, p2, p3, p4, p5, p6)         ",\"properties\":{" p1 "," p2 "," p3 "," p4 "," p5 "," p6 "}"
#define PROPS_8(p1, p2, p3, p4, p5, p6, p7, p8) ",\"properties\":{" p1 "," p2 "," p3 "," p4 "," p5 "," p6 "," p7 "," p8 "}"
#define PROPS_15(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15) ",\"properties\":{" p1 "," p2 "," p3 "," p4 "," p5 "," p6 "," p7 "," p8 ","  p9 "," p10 "," p11 "," p12 "," p13 "," p14 "," p15 "}"


#define REQUIRED_1(p1)                          ",\"required\":[\"" #p1 "\"]"
#define REQUIRED_2(p1, p2)                      ",\"required\":[\"" #p1 "\",\""  #p2 "\"]"
#define REQUIRED_3(p1, p2, p3)                  ",\"required\":[\"" #p1 "\",\""  #p2 "\",\""   #p3 "\"]"
#define REQUIRED_5(p1, p2, p3, p4, p5)          ",\"required\":[\"" #p1 "\",\""  #p2 "\",\""   #p3 "\",\""  #p4 "\",\""  #p5 "\"]"
#define REQUIRED_15(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15) ",\"required\":[\"" #p1 "\",\""  #p2 "\",\""   #p3 "\",\""  #p4 "\",\""  #p5 "\",\"" #p6 "\",\""  #p7 "\",\""  #p8 "\",\""  #p9 "\",\""  #p10 "\",\"" #p11 "\",\""  #p12 "\",\""  #p13 "\",\""  #p14 "\",\""  #p15 "\"]"

/*
 * Macros to use in place of the parameters in the PROPS_X macros above
 *
 */
#define PROPERTY(name, type) "\"" #name "\":{\"type\":\"" #type "\"}"
#define WITHDEFAULT(name, type, value) "\"" #name "\":{\"type\":\"" #type "\",\"default\":" #value "}"

// Build an Object Schema as a const char *string
//
#define NAKED_OBJECT_OPTIONAL_7(objName, p1, type1, p2, type2, p3, type3, p4, type4, p5, type5, p6, type6, p7, type7)  \
	"\"" #objName "\":{\"type\":\"object\",\"properties\":{" PROPERTY(p1, type1) "," PROPERTY(p2, type2) ","           \
					   PROPERTY(p3, type3) "," PROPERTY(p4, type4) "," PROPERTY(p5, type5) "," PROPERTY(p6, type6) "," \
					   PROPERTY(p7, type7) "},\"additionalProperties\":false}"
#define NAKED_OBJECT_OPTIONAL_8(objName, p1, type1, p2, type2, p3, type3, p4, type4, p5, type5, p6, type6, p7, type7, p8, type8) \
	"\"" #objName "\":{\"type\":\"object\",\"properties\":{" PROPERTY(p1, type1) "," PROPERTY(p2, type2) "," \
					   PROPERTY(p3, type3) "," PROPERTY(p4, type4) "," PROPERTY(p5, type5) "," PROPERTY(p6, type6) "," \
					   PROPERTY(p7, type7) "," PROPERTY(p8, type8) "},\"additionalProperties\":false}"

// Schema for Monotonic Timestamp
#define SCHEMA_TIMESTAMP { \
			"type": "object", \
			"properties": { \
				"source": { "type": "string" }, \
				"sec": { "type": "integer" }, \
				"nsec": { "type": "integer" } \
			}, \
			"required": [ "source", "sec", "nsec" ], \
			"additionalProperties": false \
		}

/*
 * Helper class to parse a json message using a schema (if specified)
 */
class JsonMessageParser
{
public:
	JsonMessageParser(const char * json, const char * schema) : mJson(json), mSchema(pbnjson::JSchema::fromString(schema)) {}

	bool					parse(const char * callerFunction);
	pbnjson::JValue			get()										{ return mParser.getDom(); }

	// convenience functions to get a parameter directly.
	bool					get(const char * name, std::string & str)	{ return get()[name].asString(str) == CONV_OK; }
	bool					get(const char * name, bool & boolean)		{ return get()[name].asBool(boolean) == CONV_OK; }
	template <class T> bool	get(const char * name, T & number)			{ return get()[name].asNumber<T>(number) == CONV_OK; }
	pbnjson::JValue			get(const char * name)						{ return get()[name]; }

private:
	const char          *mJson;
	pbnjson::JSchema    mSchema;
	pbnjson::JDomParser mParser;
};

/*
 * Helper class to parse json messages coming from an LS service using pbnjson
 */
class LSMessageJsonParser
{
public:
	// Default using any specific schema. Will simply validate that the message is a valid json message.
	LSMessageJsonParser(LSMessage * message, const char * schema);
	LSMessageJsonParser(LSMessage * message, const pbnjson::JSchema &schema);

	/*!
	  * \brief Parse the message using the schema passed in constructor.
	  * \param callerFunction   -Name of the function
	  * \param sender           - If 'sender' is specified, automatically reply in case of bad syntax using standard format.
	  * \param errOption        - Schema error option
	  * \return true if parsed successfully, false otherwise
	  */
	bool                    parse(const char * callerFunction, LSHandle * sender = 0, ESchemaErrorOptions errOption = EIgnore);

	/*! \fn getMsgCategoryMethod
	  * \brief function parses the message and creates a string with category & method appended to it
	  * \return string with category and method appended
	  */
	std::string getMsgCategoryMethod();

	/*! \fn getSender
	  * \brief function retrieves the sender name from the message
	  * \return sender name if available, empty string otherwise
	  */
	std::string getSender();

	pbnjson::JValue         get() { return mParser.getDom(); }
	const char *            getPayload()    { return LSMessageGetPayload(mMessage); }

	// convenience functions to get a parameter directly.
	bool                    get(const char * name, std::string & str)   { return get()[name].asString(str) == CONV_OK; }
	bool                    get(const char * name, bool & boolean)      { return get()[name].asBool(boolean) == CONV_OK; }
	template <class T> bool get(const char * name, T & number)          { return get()[name].asNumber<T>(number) == CONV_OK; }

private:
	LSMessage *                 mMessage;
	pbnjson::JSchema            mSchema;
	pbnjson::JDomParser         mParser;
};

/**
  * Commonly used schema Macros
  */

/**
  * Main Validation Code
  */
#define VALIDATE_SCHEMA_AND_RETURN_OPTION(lsHandle, message, schema, option) {  \
	LSMessageJsonParser jsonParser(message, schema);                            \
	if (!jsonParser.parse(__FUNCTION__, lsHandle, option))                      \
		return true;                                                            \
}

#define VALIDATE_SCHEMA_AND_RETURN(lsHandle, message, schema) {                                                  \
	VALIDATE_SCHEMA_AND_RETURN_OPTION(lsHandle, message, schema, Settings::instance()->schemaValidationOption);  \
}

/**
  * Subscribe Schema : {"subscribe":boolean}
  */
#define SUBSCRIBE_SCHEMA_RETURN(lsHandle, message)   VALIDATE_SCHEMA_AND_RETURN(lsHandle, message, STRICT_SCHEMA(PROPS_1(PROPERTY(subscribe, boolean))))

/**
  * Empty/Any Schema : {}
  */
#define EMPTY_SCHEMA_RETURN(lsHandle, message)    VALIDATE_SCHEMA_AND_RETURN(lsHandle, message, pbnjson::JSchema::AllSchema())

// build a standard reply returnValue & errorCode/errorText if defined
pbnjson::JValue createJsonReply(bool returnValue = true, int errorCode = 0, const char * errorText = 0);


template <typename T>
T toInteger(const pbnjson::JValue &value)
{
	// this check will be compiled-out due to static condition
	if (sizeof(T) <= sizeof(int32_t))
	{
		return value.asNumber<int32_t>();
	}
	else
	{
		return value.asNumber<int64_t>();
	}
}

template <typename T>
pbnjson::JValue toJValue(const T& value)
{
	// this check will be compiled-out due to static condition
	if (sizeof(T) <= sizeof(int32_t))
	{
		return static_cast<int32_t>(value);
	}
	else
	{
		return static_cast<int64_t>(value);
	}
}

template <>
inline pbnjson::JValue toJValue<struct tm>(const struct tm& tmValue)
{
	pbnjson::JValue jValue = pbnjson::Object();
	jValue.put("year", tmValue.tm_year + 1900);
	jValue.put("month", tmValue.tm_mon + 1);
	jValue.put("day", tmValue.tm_mday);
	jValue.put("hour", tmValue.tm_hour);
	jValue.put("minute", tmValue.tm_min);
	jValue.put("second", tmValue.tm_sec);
	return jValue;
}

#endif // JSONUTILS_H
