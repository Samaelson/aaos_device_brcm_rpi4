/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "Lights.h"

#include <android-base/file.h>

using ::android::base::WriteStringToFile;

namespace aidl::android::hardware::light {

static const std::string backlightFiles[] = {
    "/sys/class/backlight/rpi_backlight/brightness"
};

const static std::vector<HwLight> availableLights = {
    {.id = (int)LightType::BACKLIGHT, .type = LightType::BACKLIGHT, .ordinal = 0}
};

ndk::ScopedAStatus Lights::setLightState(int id, const HwLightState& state) {
    HwLight const& light = availableLights[id];
    std::string const brightness = std::to_string(rgbToBrightness(state));

    switch (light.type) {
        case LightType::BACKLIGHT:
            for (auto &file : backlightFiles) {
                if (!access(file.c_str(), W_OK)) {
                    WriteStringToFile(brightness, file);
                }
            }
            break;
        default:
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Lights::getLights(std::vector<HwLight>* lights) {
    for (auto i = availableLights.begin(); i != availableLights.end(); i++) {
        lights->push_back(*i);
    }

    return ndk::ScopedAStatus::ok();
}

uint32_t Lights::rgbToBrightness(const HwLightState& state) {
    uint32_t color = state.color & 0x00ffffff;
    return ((77 * ((color >> 16) & 0xff)) + (150 * ((color >> 8) & 0xff)) +
            (29 * (color & 0xff))) >> 8;
}

}  // aidl::android::hardware::light
