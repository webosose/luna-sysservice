// Copyright (c) 2013-2018 LG Electronics, Inc.
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

#ifndef ERASE_HANDLER_H
#define ERASE_HANDLER_H

#include <nyx/nyx_client.h>
#include <luna-service2/lunaservice.h>

#include "Singleton.h"

class EraseHandler : public Singleton<EraseHandler>
{
    friend class Singleton<EraseHandler>;

public:

    typedef enum EraseType
    {
         kEraseVar
        ,kEraseAll
        ,kEraseMedia
        ,kEraseMDeveloper
        ,kSecureWipe
    } EraseType_t;

    bool    init();
    void    setServiceHandle(LSHandle* serviceHandle);
    bool    Erase(LSHandle* pHandle, LSMessage* pMessage, EraseType_t type);

    ~EraseHandler();

private:
    EraseHandler();

    static LSMethod    s_EraseServerMethods[];
    nyx_device_handle_t nyxSystem;
};


#endif // ERASE_HANDLER_H
