/*
 * Copyright (C) 2021 The Android Open Source Project
 * Copyright (C) 2023 KonstaKANG
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "HealthImpl.h"

#include <android/binder_interface_utils.h>
#include <health/utils.h>

using ::aidl::android::hardware::health::HalHealthLoop;
using ::aidl::android::hardware::health::HealthImpl;

int main() {
    auto config = std::make_unique<healthd_config>();
    android::hardware::health::InitHealthdConfig(config.get());
    auto binder = ndk::SharedRefBase::make<HealthImpl>("default", std::move(config));
    auto hal_health_loop = std::make_shared<HalHealthLoop>(binder, binder);
    return hal_health_loop->StartLoop();
}
