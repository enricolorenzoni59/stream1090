/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A second translation unit for SignalsTest: the signal flags must be shared,
 * so a SIGINT delivered in one TU has to be visible in another.
 */

#include "Global.hpp"

bool probeShutdownRequested() {
    return ProcessSignals::shutdownRequested();
}

bool probeReselectRequested() {
    return ProcessSignals::reselectRequested();
}
