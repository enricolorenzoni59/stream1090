/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "PresetDispatcher.hpp"
#if !defined(STREAM1090_CUSTOM_INPUT) || !STREAM1090_CUSTOM_INPUT
std::optional<bool> runSigned16MidPresets(const CompileTimeVars& c, const RuntimeVars& r) {
    return runPresetGroup(signed16MidPresets, c, r);
}
#endif
