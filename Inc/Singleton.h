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

#ifndef SINGLETON_HPP
#define SINGLETON_HPP

template <typename T>
class Singleton
{

public:
	static T* instance()
	{
		// Be carefule it isn't trhead safe implementation
		if (!object)
			object = new T;
		return object;
	}

	virtual ~Singleton()
	{
		object = nullptr;
	}

protected:
	Singleton() = default;
	Singleton(Singleton&) = delete;
	void operator=(Singleton&) = delete;

private:
	static T* object;
};

template <typename T>
T* Singleton<T>::object = nullptr;

#endif //SINGLETON_HPP
