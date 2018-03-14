// Copyright (c) 2015-2018 LG Electronics, Inc.
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

#ifndef ERROR_EXCEPTION_H
#define ERROR_EXCEPTION_H

#include <string>
#include <stdexcept>

class ErrorException : public std::exception
{

public:
	ErrorException(int errorCode, const std::string& errorText)
		: m_errorCode(errorCode)
		, m_errorText(errorText)
	{}

	int erroCode() const { return m_errorCode; }
	const std::string& errorText() const { return m_errorText; }

	const char* what() const throw() { return m_errorText.c_str(); }

private:
	int m_errorCode;
	std::string m_errorText;
};

#endif //ERROR_EXCEPTION_H
