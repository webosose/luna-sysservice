// Copyright (c) 2010-2019 LG Electronics, Inc.
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

#include "ImageServices.h"

#include <errno.h>
#include <glib.h>

#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtCore/QtGlobal>
#include <QtGui/QImageReader>

#include <pbnjson.hpp>
#include <luna-service2++/error.hpp>

#include "Utils.h"
#include "Logging.h"
#include "JSONUtils.h"
#include "ImageHelpers.h"

using namespace pbnjson;

/*! \page com_palm_image_service Service API com.webos.service.image/
 *
 *  Public methods:
 *   - \ref image_service_convert
 *   - \ref image_service_ez_resize
 *   - \ref image_service_image_info
 */

LSMethod ImageServices::s_methods[] = {
	{ "convert" , ImageServices::lsConvertImage },
	{ "imageInfo" , ImageServices::lsImageInfo },
	{ "ezResize" , ImageServices::lsEzResize },
	{ 0, 0 }
};

/*! \page com_palm_image_service
\n
\section image_service_convert convert

\e Public.

com.webos.service.image/convert

Converts an image.

\subsection com_palm_image_convert_syntax Syntax:
\code
{
	"src": string,
	"dest": string,
	"destType": string,
	"focusX": number,
	"focusY": number,
	"scale": number,
	"cropW": number,
	"cropH": number
}
\endcode

\param src Absolute path to source file. Required.
\param dest Absolute path for output file. Required.
\param destType Type of the output file. Required.
\param focusX The horizontal coordinate of the new center of the image, from 0.0 (left edge) to 1.0 (right edge). A value of 0.5 preserves the current horizontal center of the image.
\param focusY The vertical coordinate of the new center of the image, from 0.0 (top edge) to 1.0 (bottom edge). A value of 0.5 preserves the current vertical center of the image.
\param scale Scale factor for the image, must be greater than zero.
\param cropW Crop the image to this width.
\param cropH Crop the image to this width height.

\subsection com_palm_image_convert_return Returns:
\code
{
	"subscribed": boolean,
	"returnValue": boolean,
	"errorCode": string
}
\endcode

\param subscribed Always false.
\param returnValue Indicates if the call was succesful.
\param errorCode Description of the error if call was not succesful.

\subsection com_palm_image_convert_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.image/convert '{"src": "/usr/lib/luna/system/luna-systemui/images/opensearch-small-icon.png", "dest": "/tmp/convertedimage.png", "destType": "jpg"  }'
\endcode

Example response for a succesful call:
\code
{
	"subscribed": false,
	"returnValue": true
}
\endcode
Example response for a failed call:
\code
{
	"subscribed": false,
	"returnValue": false,
	"errorCode": "'destType' parameter missing"
}
\endcode
*/
//static 
bool ImageServices::lsConvertImage(LSHandle* lsHandle, LSMessage* message,void* user_data)
{
	std::string errorText;
	int rc;
	bool specOn = false;

	// {"src": string, "dest": string, "destType": string, "focusX": number, "focusY": number, "scale": number, "cropW": number, "cropH": number}
	LSMessageJsonParser parser(message, RELAXED_SCHEMA(
											PROPS_8(PROPERTY(src, string), PROPERTY(dest, string),
													PROPERTY(destType, string), PROPERTY(focusX, number),
													PROPERTY(focusY, number), PROPERTY(scale, number),
													PROPERTY(cropW, number), PROPERTY(cropH, number))
											REQUIRED_3(src, dest, destType)));

	if (!parser.parse(__FUNCTION__, lsHandle, EValidateAndErrorAlways))
		return true;

	std::string srcfile;
	std::string destfile;
	std::string desttype;

	JValue root = parser.get();

	Utils::extractFromJson(root,"src",srcfile);
	Utils::extractFromJson(root,"dest",destfile);
	Utils::extractFromJson(root,"destType",desttype);

	do {
		double focusX = -1;
		double focusY = -1;
		double scale = -1;
		uint32_t cropW = 0;
		uint32_t cropH = 0;

		JValue label = root["focusX"];
		if (label.isNumber()) {
			focusX = label.asNumber<double>();
			if ((focusX < 0) || (focusX > 1)) {
				errorText = "'focusX' parameter out of range (must be [0.0,1.0] )";
				break;
			}
			specOn = true;
		}

		label = root["focusY"];
		if (label.isNumber()) {
			focusY = label.asNumber<double>();
			if ((focusY < 0) || (focusY > 1)) {
				errorText = "'focusY' parameter out of range (must be [0.0,1.0] )";
				break;
			}
			specOn = true;
		}

		label = root["scale"];
		if (label.isNumber()) {
			scale = label.asNumber<double>();
			if (scale <= 0) {
				errorText = "'scale' parameter out of range ( must be > 0.0 )";
				break;
			}
			specOn = true;
		}

		label = root["cropW"];
		if (label.isNumber()) {
			if (label.asNumber<int32_t>() < 0) {
				errorText = "'cropW' parameter out of range (must be > 0 )";
				break;
			}

			cropW = label.asNumber<int32_t>();
			specOn = true;
		}

		label = root["cropH"];
		if (label.isNumber()) {
			if (label.asNumber<int32_t>() < 0) {
				errorText = "'cropH' parameter out of range (must be > 0 )";
				break;
			}
			cropH = label.asNumber<int32_t>();
			specOn = true;
		}

		/*
		 *
		 * bool convertImage(const std::string& pathToSourceFile,
				const std::string& pathToDestFile,int destType,
				double focusX,double focusY,double scale,
				uint32_t widthFinal,uint32_t heightFinal,
				std::string& r_errorText);

		bool convertImage(const std::string& pathToSourceFile,
				const std::string& pathToDestFile,int destType,
				std::string& r_errorText);

		 *
		 */
		if (specOn) {
			rc = ImageServices::instance()->convertImage(srcfile, destfile, desttype.c_str(),
														 focusX, focusY,
														 scale,
														 cropW, cropH,
														 errorText);
		}
		else {
			// the "just transcode" version of convert is called
			rc = ImageServices::instance()->convertImage(srcfile, destfile, desttype.c_str(), errorText);
		}
	} while (false);

	JObject reply {{"subscribed", false}};
	if (!errorText.empty()) {
		reply.put("returnValue", false);
		reply.put("errorCode", errorText);
		qWarning() << errorText.c_str();
	}
	else {
		reply.put("returnValue", true);
	}

	LS::Error error;
	if (!LSMessageReply(lsHandle, message, reply.stringify().c_str(), error))
	{
		qWarning() << error.what();
	}

	return true;
}

/*! \page com_palm_image_service
\n
\section image_service_ez_resize ezResize

\e Public.

com.webos.service.image/ezResize

Resize an image.

\subsection image_service_ez_resize_syntax Syntax:
\code
{
	"src": string,
	"dest": string,
	"destType": string,
	"destSizeW": integer,
	"destSizeH": integer
}
\endcode

\param src Absolute path to source file. Required.
\param dest Absolute path for output file. Required.
\param destType Type of the output file. Required.
\param destSizeW Width of the resized image. Required.
\param destSizeH Height of the resized image. Required.

\subsection image_service_ez_resize_returns Returns:
\code
{
	"subscribed": boolean,
	"returnValue": boolean,
	"errorCode": string
}
\endcode

\param subscribed Always false.
\param returnValue Indicates if the call was succesful.
\param errorCode Description of the error if call was not succesful.

\subsection image_service_ez_resize_examples Examples:
\code
luna-send -n 1 -f luna://com.webos.service.image/ezResize '{"src": "/usr/lib/luna/system/luna-systemui/images/opensearch-small-icon.png", "dest": "/tmp/convertedimage", "destType": "jpg", "destSizeW": 6, "destSizeH": 6 }'
\endcode

Example response for a succesful call:
\code
{
	"subscribed": false,
	"returnValue": true
}
\endcode

Example response for a failed call:
\code
{
	"subscribed": false,
	"returnValue": false,
	"errorCode": "'destSizeH' missing"
}
\endcode
*/
//static
bool ImageServices::lsEzResize(LSHandle* lsHandle, LSMessage* message,void* user_data)
{
	std::string errorText;

	// {"src": string, "dest": string, "destType": string, "destSizeW": integer, "destSizeH": integer}
	LSMessageJsonParser parser(message, RELAXED_SCHEMA(
											  PROPS_5(PROPERTY(src, string),
													  PROPERTY(dest, string),
													  PROPERTY(destType, string),
													  PROPERTY(destSizeW, integer),
													  PROPERTY(destSizeH, integer))
											  REQUIRED_5(src, dest, destType, destSizeW, destSizeH)));

	if (!parser.parse(__FUNCTION__, lsHandle, EValidateAndErrorAlways))
		return true;

	JValue root = parser.get();

	std::string srcfile;
	std::string destfile;
	std::string desttype;

	Utils::extractFromJson(root,"src",srcfile);
	Utils::extractFromJson(root,"dest",destfile);
	Utils::extractFromJson(root,"destType",desttype);

	uint32_t destSizeW = root["destSizeW"].asNumber<int32_t>();
	uint32_t destSizeH = root["destSizeH"].asNumber<int32_t>();

	(void)ImageServices::instance()->ezResize(srcfile, destfile, desttype.c_str(), destSizeW, destSizeH, errorText);

	JObject reply {{"subscribed", false}};
	if (!errorText.empty()) {
		reply.put("returnValue", false);
		reply.put("errorCode", errorText);
		qWarning() << errorText.c_str();
	}
	else {
		reply.put("returnValue", true);
	}

	LS::Error error;
	if (!LSMessageReply(lsHandle, message, reply.stringify().c_str(), error))
	{
		qWarning() << error.what();
	}

	return true;
}

/*! \page com_palm_image_service
\n
\section image_service_image_info imageInfo

\e Public.

com.webos.service.image/imageInfo

Get information for an image.

\subsection image_service_image_info_syntax Syntax:
\code
{
	"src": string
}
\endcode

\param src Absolute path to source file. Required.

\subsection image_service_image_info_returns Returns:
\code
{
	"subscribed": boolean,
	"returnValue": boolean,
	"errorCode": string,
	"width": int,
	"height": int,
	"bpp": int,
	"type": "string
}
\endcode

\param subscribed Always false.
\param returnValue Indicates if the call was succesful or not.
\param errorCode Description of the error if call was not succesful.
\param with Width of the image.
\param height Height of the image.
\param bpp Color depth, bits per pixel.
\param type Type of the image file.

\subsection image_service_image_info_examples Examples:

\code
luna-send -n 1 -f  luna://com.webos.service.image/imageInfo '{"src":"/usr/lib/luna/system/luna-systemui/images/opensearch-small-icon.png"}'
\endcode
Example response for a successful call:
\code
{
	"subscribed": false,
	"returnValue": true,
	"width": 24,
	"height": 24,
	"bpp": 8,
	"type": "png"
}
\endcode

Example response in case of a failure:
\code
{
	"subscribed": false,
	"returnValue": false,
	"errorCode": "source file does not exist"
}
\endcode
*/
//static
bool ImageServices::lsImageInfo(LSHandle* lsHandle, LSMessage* message,void* user_data)
{
	std::string errorText;

	int srcWidth = 0;
	int srcHeight = 0;
	int srcBpp = 0;
	std::string srcType;

	// {"src": string}
	LSMessageJsonParser parser(message, RELAXED_SCHEMA(
										  PROPS_1(PROPERTY(src, string))
										  REQUIRED_1(src)));

	if (!parser.parse(__FUNCTION__, lsHandle, EValidateAndErrorAlways))
		return true;

	std::string srcfile = parser.get()["src"].asString();
	QImageReader reader(QString::fromStdString(srcfile));

	if(reader.canRead()) {
		srcWidth = reader.size().width();
		srcHeight = reader.size().height();
		// QImageReader probably won't return all of these, but just to make sure we cover all cases
		switch(reader.imageFormat()) {
		case QImage::Format_ARGB32_Premultiplied:
		case QImage::Format_ARGB32:
		case QImage::Format_RGB32:
			srcBpp = 32; break;
		case QImage::Format_RGB888:
		case QImage::Format_RGB666:
		case QImage::Format_ARGB8565_Premultiplied:
		case QImage::Format_ARGB6666_Premultiplied:
		case QImage::Format_ARGB8555_Premultiplied:
			srcBpp = 24; break;
		case QImage::Format_RGB444:
		case QImage::Format_ARGB4444_Premultiplied:
		case QImage::Format_RGB16:
		case QImage::Format_RGB555:
			srcBpp = 16; break;
		case QImage::Format_Indexed8:
			srcBpp = 8; break;
		case QImage::Format_Mono:
		case QImage::Format_MonoLSB:
			srcBpp = 1; break;
		default:
			srcBpp = 0;
		}
		srcType = reader.format().data(); // png/jpg etc
	}
	else {
		errorText = reader.errorString().toStdString();
	}

	JObject reply {{"subscribed", false}};
	if (!errorText.empty()) {
		reply.put("returnValue", false);
		reply.put("errorCode", errorText);
		qWarning() << errorText.c_str();
	}
	else {
		reply.put("returnValue", true);
		reply.put("width", srcWidth);
		reply.put("height", srcHeight);
		reply.put("bpp", srcBpp);
		reply.put("type", srcType);
	}

	LS::Error error;
	if (!LSMessageReply(lsHandle, message, reply.stringify().c_str(), error))
	{
		qWarning() << error.what();
	}

	return true;
}

//////////////////////////////////////////////////////////////// PRIVATE ///////////////////////////////////////////////

ImageServices::ImageServices()
	: m_serviceHandle(nullptr)
{
}

bool ImageServices::init(GMainLoop *loop)
{
	if (m_serviceHandle)
		return true;

	LS::Error error;
	LSHandle *serviceHandle = nullptr;
	if (!LSRegister("com.webos.service.image", &serviceHandle, error))
	{
		qWarning() << "Can not register com.webos.service.image: " << error.what();
		return false;
	}

	if (!LSGmainAttach(serviceHandle, loop, error))
	{
		LSUnregister(serviceHandle, nullptr);
		qWarning() << "Can not attach to main loop: " << error.what();
		return false;
	}

	if (!LSRegisterCategory(serviceHandle, "/", s_methods,
									   nullptr, nullptr, error))
	{
		LSUnregister(serviceHandle, nullptr);
		qWarning() << "Failed in registering handler methods on /:" << error.what();
		return false;
	}

	m_serviceHandle = serviceHandle;
	return true;
}

////////////////////////////////////////////// PRIVATE - IMAGE FUNCTIONS ///////////////////////////////////////////////

bool ImageServices::ezResize(const std::string& pathToSourceFile,
							 const std::string& pathToDestFile, const char* destType,
							 uint32_t widthFinal, uint32_t heightFinal,
							 std::string& r_errorText)
{
	qDebug("From: [%s], To: [%s], target: {Type: [%s], w:%d, h:%d}",
			pathToSourceFile.c_str(), pathToDestFile.c_str(), destType, widthFinal, heightFinal);

	QImageReader reader(QString::fromStdString(pathToSourceFile));
	if(!reader.canRead()) {
		r_errorText = reader.errorString().toStdString();
		return false;
	}

	QImage image;
	if (!reader.read(&image)) {
		r_errorText = reader.errorString().toStdString();
		return false;
	}
	// cropped rescale, see http://qt-project.org/doc/qt-4.8/qt.html#AspectRatioMode-enum

	QImage result(widthFinal, heightFinal, image.format());

	if(result.isNull()) {
		r_errorText = "ezResize: unable to allocate memory for QImage";
		return false;
	}

	QPainter p(&result);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.drawImage(QRect(0,0,widthFinal, heightFinal), image);
	p.end();
//    image = image.scaled(widthFinal, heightFinal, Qt::KeepAspectRatioByExpanding);
	PMLOG_TRACE("About to save image");
	if(!result.save(QString::fromStdString(pathToDestFile), destType, 100)) {
		r_errorText = "ezResize: failed to save destination file";
		return false;
	}

	return true;
}

bool ImageServices::convertImage(const std::string& pathToSourceFile,
								 const std::string& pathToDestFile, const char* destType,
								 double focusX, double focusY, double scale,
								 uint32_t widthFinal,uint32_t heightFinal,
								 std::string& r_errorText)
{
	qDebug("From: [%s], To: [%s], focus:{x:%f,y:%f}, target: {Type: [%s], w:%d, h:%d}, scale: %f",
			pathToSourceFile.c_str(), pathToDestFile.c_str(), focusX, focusY, destType, widthFinal, heightFinal, scale);

	QImageReader reader(QString::fromStdString(pathToSourceFile));
	if(!reader.canRead()) {
		r_errorText = reader.errorString().toStdString();
		return false;
	}

	if (focusX < 0)
		focusX = 0.5;
	if (focusY < 0)
		focusY = 0.5;

	//fix scale factor just in case it's negative
	if (scale < 0.0)
		scale *= -1.0;

	//TODO: WARN: strict comparison of float to 0 might fail
	if (qFuzzyCompare(scale, 0.0))
		scale = 1.0;
	qDebug("After adjustments: scale: %f, focus:{x:%f,y:%f}", scale, focusX, focusY);

	QImage image;
	double prescale;
	if(!readImageWithPrescale(reader, image, prescale)) {
		r_errorText = reader.errorString().toStdString();
		return false;
	}

	//scale the image as requested...factor in whatever the prescaler did
	scale /= prescale;
	qDebug("scale after prescale adjustment: %f, prescale: %f", scale, prescale);

	QImage dest(widthFinal, heightFinal, image.format());
	QPainter p (&dest);
	p.translate(heightFinal/2, widthFinal/2);
	p.translate(-focusX * image.width(), -focusY * image.height());
	p.scale(scale, scale);
	p.drawImage(QPoint(0,0), image);
	p.end();

	dest.save(QString::fromStdString(pathToDestFile), destType, 100);
	return true;

}

bool ImageServices::convertImage(const std::string& pathToSourceFile,
								 const std::string& pathToDestFile, const char* destType,
									   std::string& r_errorText)
{
	qDebug("From: [%s], To: [%s], target: {Type: [%s]}",
			pathToSourceFile.c_str(), pathToDestFile.c_str(), destType);

	QImageReader reader(QString::fromStdString(pathToSourceFile));
	if(!reader.canRead()) {
		r_errorText = reader.errorString().toStdString();
		return false;
	}

	QImage image;
	if (!reader.read(&image)) {
		r_errorText = reader.errorString().toStdString();
		return false;
	}

	image.save(QString::fromStdString(pathToDestFile), destType, 100);
	return true;
}

