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

#ifndef IMAGESERVICES_H
#define IMAGESERVICES_H

#include <string>
#include <luna-service2/lunaservice.h>

#include "Singleton.h"

class ImageServices : public Singleton<ImageServices>
{
	friend class Singleton<ImageServices>;

public:
	bool isValid() { return m_serviceHandle;}
	bool init(GMainLoop *loop);

	bool ezResize(const std::string& pathToSourceFile,
				  const std::string& pathToDestFile, const char* destType,
				  uint32_t widthFinal,uint32_t heightFinal,
				  std::string& r_errorText);

protected:
	static bool lsConvertImage(LSHandle* lsHandle, LSMessage* message,void* user_data);
	static bool lsImageInfo(LSHandle* lsHandle, LSMessage* message,void* user_data);
	static bool lsEzResize(LSHandle* lsHandle, LSMessage* message,void* user_data);

private:
	ImageServices();

	bool convertImage(const std::string& pathToSourceFile,
					  const std::string& pathToDestFile, const char* destType,
					  std::string& r_errorText);
	bool convertImage(const std::string& pathToSourceFile,
					  const std::string& pathToDestFile, const char* destType,
					  double focusX, double focusY, double scale,
					  uint32_t widthFinal, uint32_t heightFinal,
					  std::string& r_errorText);

private:
	LSHandle* m_serviceHandle;
	static LSMethod s_methods[];
};

#endif //IMAGESERVICES_H
