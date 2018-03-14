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

#include <QFileInfo>

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

	QString meta = QString("%1#%2").arg(QFileInfo(file).baseName()).arg(line);
	QString data = QString().vsprintf(fmt, args);

	va_end(args);

	PmLogInfo(sysServiceLogContext(), meta.toLatin1().data(), 1, PMLOGKS("FUNC", func), "%s", data.toUtf8().data());
}

void outputQtMessages(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
	QString meta = QString("%1#%2").arg(QFileInfo(context.file).baseName()).arg(context.line);

	switch (type) {
		case QtDebugMsg:
#ifndef NO_LOGGING
			PmLogDebug(sysServiceLogContext(), meta.toLatin1().data(), 1, PMLOGKS("FUNC", context.function),
					   "%s", msg.toUtf8().data());
#endif
			break;
		case QtWarningMsg:
			PmLogWarning(sysServiceLogContext(), meta.toLatin1().data(), 1, PMLOGKS("FUNC", context.function),
						 "%s", msg.toUtf8().data());
			break;
		case QtCriticalMsg:
			PmLogError(sysServiceLogContext(), meta.toLatin1().data(), 1, PMLOGKS("FUNC", context.function),
					   "%s", msg.toUtf8().data());
			break;
		case QtFatalMsg:
			PmLogCritical(sysServiceLogContext(), meta.toLatin1().data(), 1, PMLOGKS("FUNC", context.function),
						  "%s", msg.toUtf8().data());
			abort();
	}
}
