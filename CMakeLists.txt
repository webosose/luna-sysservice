# @@@LICENSE
#
# Copyright (c) 2012-2024 LG Electronics, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 2.8.7)

project(LunaSysService CXX)

include(webOS/webOS)
webos_modules_init(1 0 0 QUALIFIER RC9)
webos_component(4 4 0)

include(FindPkgConfig)

# -- check for glib 2.0
pkg_check_modules(GLIB2 REQUIRED glib-2.0)
webos_add_compiler_flags(ALL ${GLIB2_CFLAGS})
include_directories(${GLIB2_INCLUDE_DIRS})

# -- check for libxml 2.0
pkg_check_modules(GXML2 REQUIRED libxml-2.0)
webos_add_compiler_flags(ALL ${GXML2_CFLAGS})

# -- check for sqlite 3.0
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
webos_add_compiler_flags(ALL ${SQLITE3_CFLAGS})

# -- check for pbnjson c and cpp libraries
pkg_check_modules(PBNJSON_C REQUIRED pbnjson_c)
webos_add_compiler_flags(ALL ${PBNJSON_C_CFLAGS})

pkg_check_modules(PBNJSON_CPP REQUIRED pbnjson_cpp)
webos_add_compiler_flags(ALL ${PBNJSON_CPP_CFLAGS})

# -- check for LS2 (internally depends on pmloglib)
pkg_check_modules(LS2 REQUIRED luna-service2)
webos_add_compiler_flags(ALL ${LS2_CFLAGS})

pkg_check_modules(LS2++ REQUIRED luna-service2++>=3)
include_directories(${LS2++_INCLUDE_DIRS})

pkg_check_modules(WEBOSI18N REQUIRED webosi18n)
include_directories(WEBOSI18N_INCLUDE_DIRS)
webos_add_compiler_flags(ALL ${WEBOSI18N_CFLAGS})

pkg_check_modules(PMLOG REQUIRED PmLogLib>=3)
include_directories(${PMLOG_INCLUDE_DIRS})

if (DESKTOP)
    webos_add_compiler_flags(ALL -DDESKTOP)
endif()

#if(${USE_WEBOS_QT})
#find_package(Qt6 COMPONENTS Core Gui)
#if (NOT Qt6_FOUND)
#     pkg_check_modules(QtGui REQUIRED Qt5Gui)
#     include_directories(${QtGui_INCLUDE_DIRS})
#endif()
#endif()

# -- check for uriparser
pkg_check_modules(URIPARSER REQUIRED liburiparser)
webos_add_compiler_flags(ALL ${URIPARSER_CFLAGS})

#-- check for NYX
pkg_check_modules(NYXLIB REQUIRED nyx>=7)
include_directories(${NYXLIB_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${NYXLIB_CFLAGS_OTHER})

#webos_config_build_doxygen(doc Doxyfile)

# -- no way to disable warn_unused_result right now.
webos_add_compiler_flags(ALL -std=c++1z -O3 -Wall -Wno-unused-but-set-variable -Wno-unused-variable -fno-rtti -fPIC)
webos_add_linker_options(ALL --no-undefined)
#Uncomment following line to see Trace logs
#add_definitions(-DPMLOG_TRACES_ENABLED -DPMLOG_TRACE_COMPONENT="LunaSysService")

# -- add local include paths
include_directories(include/public)
include_directories(Inc)

# -- export cmake WEBOS_INSTALL_* variables as C defines
webos_include_install_paths()

set(SOURCE_FILES Src/Logging.cpp
    Src/LocalePrefsHandler.cpp
    Src/Main.cpp
    Src/PrefsDb.cpp
    Src/PrefsFactory.cpp
    Src/TimePrefsHandler.cpp
    Src/BroadcastTime.cpp
    Src/BroadcastTimeHandler.cpp
    Src/UrlRep.cpp
    Src/BuildInfoHandler.cpp
    Src/Utils.cpp
    Src/SystemRestore.cpp
    Src/RingtonePrefsHandler.cpp
    Src/Mainloop.cpp
    Src/TimeZoneService.cpp
    Src/TzParser.cpp
    Src/BackupManager.cpp
    Src/Settings.cpp
    Src/NetworkConnectionListener.cpp
    Src/JSONUtils.cpp
    Src/ImageHelpers.cpp
    Src/ClockHandler.cpp
    Src/NTPClock.cpp
    Src/OsInfoService.cpp
    Src/DeviceInfoService.cpp
    )
add_executable(LunaSysService ${SOURCE_FILES})
target_link_libraries(LunaSysService
                      ${GLIB2_LDFLAGS}
                      ${GXML2_LDFLAGS}
                      ${SQLITE3_LDFLAGS}
                      ${MJSON_LDFLAGS}
                      ${PBNJSON_C_LDFLAGS}
                      ${PBNJSON_CPP_LDFLAGS}
                      ${LS2_LDFLAGS}
                      ${LS2++_LDFLAGS}
                      ${URIPARSER_LDFLAGS}
                      ${PMLOG_LDFLAGS}
                      ${NYXLIB_LDFLAGS}
                      ${WEBOSI18N_LDFLAGS}
                      rt
                      )
#if (Qt6_FOUND)
    #target_link_libraries(LunaSysService PRIVATE Qt::Core Qt::Gui)

#endif()

webos_build_system_bus_files()
webos_build_daemon()

install(FILES files/conf/sysservice-backupkeys.json DESTINATION ${WEBOS_INSTALL_WEBOS_SYSCONFDIR})
install(FILES files/conf/sysservice.conf DESTINATION ${WEBOS_INSTALL_WEBOS_SYSCONFDIR})
install(FILES files/conf/com.webos.service.systemservice.backupRegistration.json DESTINATION ${WEBOS_INSTALL_WEBOS_SYSCONFDIR}/backup)

