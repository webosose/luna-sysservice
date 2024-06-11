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

#ifndef LOGGING_H
#define LOGGING_H

#include <cstdio>
#include <string>
#ifdef WEBOS_QT
#include <QtCore/QDebug>
#endif //WEBOS_QT

#include "PmLogLib.h"

PmLogContext& sysServiceLogContext();

void setLogLevel(const std::string& loglstr);

void logInfo(const char* file, int line, const char* func, const char *fmt, ...);

#ifdef WEBOS_QT
void outputQtMessages(QtMsgType type, const QMessageLogContext &context, const QString &msg);
#endif //WEBOS_QT

#define __qMessage(fmt, ...)  do { logInfo(__FILE__, __LINE__, __func__, fmt, __VA_ARGS__); } while(0)

#endif /* LOGGING_H */
