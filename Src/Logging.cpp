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

#include "Logging.h"

#ifdef WEBOS_QT
#include <QtCore/QFileInfo>
#endif //WEBOS_QT

PmLogContext& sysServiceLogContext()
{
	static PmLogContext logContext = PmLogGetContextInline("LunaSysService");
	return logContext;
}

void setLogLevel(const std::string& loglstr)
{
	if (loglstr == "error")
		PmLogSetContextLevel(sysServiceLogContext(), kPmLogLevel_Error);
	else if (loglstr == "critical")
		PmLogSetContextLevel(sysServiceLogContext(), kPmLogLevel_Critical);
	else if (loglstr == "warning")
		 PmLogSetContextLevel(sysServiceLogContext(), kPmLogLevel_Warning);
	else if (loglstr == "info")
		PmLogSetContextLevel(sysServiceLogContext(), kPmLogLevel_Info);
	else if (loglstr == "debug")
		PmLogSetContextLevel(sysServiceLogContext(), kPmLogLevel_Debug);
	else
		PmLogSetContextLevel(sysServiceLogContext(), kPmLogLevel_Info);
}

void logInfo(const char* file, int line, const char* func, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	//QString meta = QString("%1#%2").arg(QFileInfo(file).baseName()).arg(line);
	std::string meta = std::string(file) + std::string("#") + std::to_string(line);

	//QString data = QString().vasprintf(fmt, args);
	va_copy(args, args);
	int size = vsnprintf(nullptr, 0, fmt, args);
	// Allocate memory for the formatted string
	std::string data;
	if (size >= 0)
	{
		// Resize the string to accommodate the formatted data
		data.resize(size + 1); // +1 for null terminator

		// Format the string into the buffer
		vsnprintf(&data[0], size + 1, fmt, args);
		data.pop_back(); // Remove the extra null terminator added by vsnprintf
	}

	va_end(args);
	//PmLogInfo(sysServiceLogContext(), meta.toLatin1().data(), 1, PMLOGKS("FUNC", func), "%s", data.toUtf8().data());
	PmLogInfo(sysServiceLogContext(), meta.c_str(), 1, PMLOGKS("FUNC", func), "%s", data);
}

#ifdef WEBOS_QT
void outputQtMessages(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
	//QString meta = QString("%1#%2").arg(QFileInfo(context.file).baseName()).arg(context.line);
	std::string meta = std::string(context.file) + std::string("#") + std::to_string(context.line);

	switch (type) {
		case QtDebugMsg:
#ifndef NO_LOGGING
			PmLogDebug(sysServiceLogContext(), meta.c_str(), 1, PMLOGKS("FUNC", context.function),
					   "%s", msg);
#endif
			break;
		case QtWarningMsg:
			PmLogWarning(sysServiceLogContext(), meta.c_str(), 1, PMLOGKS("FUNC", context.function),
						 "%s", msg);
			break;
		case QtCriticalMsg:
			PmLogError(sysServiceLogContext(), meta.c_str(), 1, PMLOGKS("FUNC", context.function),
					   "%s", msg);
			break;
		case QtFatalMsg:
			PmLogCritical(sysServiceLogContext(), meta.c_str(), 1, PMLOGKS("FUNC", context.function),
						  "%s", msg);
			abort();
	}
}
#endif //WEBOS_QT
