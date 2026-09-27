/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "PresetDispatcher.hpp"
#if !defined(STREAM1090_CUSTOM_INPUT) || !STREAM1090_CUSTOM_INPUT
std::optional<bool> runSigned16LowPresets(const CompileTimeVars&, const RuntimeVars&);
std::optional<bool> runSigned16MidPresets(const CompileTimeVars&, const RuntimeVars&);
std::optional<bool> runSigned16HighPresets(const CompileTimeVars&, const RuntimeVars&);
std::optional<bool> runSigned16Presets(const CompileTimeVars& c, const RuntimeVars& r) {
    if (auto result = runSigned16LowPresets(c,r)) return result;
    if (auto result = runSigned16MidPresets(c,r)) return result;
    if (auto result = runSigned16HighPresets(c,r)) return result;
    return std::nullopt;
}
#endif
