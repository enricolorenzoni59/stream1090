/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "Global.hpp"

#include <csignal>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        failures++;
    }
}

} // namespace

// Defined in SignalsProbe.cpp, a different translation unit.
bool probeShutdownRequested();
bool probeReselectRequested();

int main() {
    ProcessSignals::install();
    ProcessSignals::clearReselect();
    ProcessSignals::clearShutdown();

    // A SIGHUP asks for a re-selection and also stops the current run so the
    // supervisor regains control.
    std::raise(SIGHUP);
    check(ProcessSignals::reselectRequested(), "sighup sets reselect");
    check(ProcessSignals::shutdownRequested(), "sighup stops the current run");

    // The same state must be visible from another translation unit: if it were
    // per-TU, a signal delivered here would never wake the pipeline compiled
    // elsewhere, and SIGINT would be silently ignored.
    check(probeReselectRequested(), "sighup reselect visible in another TU");
    check(probeShutdownRequested(), "sighup shutdown visible in another TU");

    ProcessSignals::clearReselect();
    ProcessSignals::clearShutdown();
    check(!ProcessSignals::reselectRequested(), "clearReselect");
    check(!ProcessSignals::shutdownRequested(), "clearShutdown");

    // SIGINT is a plain shutdown: no re-selection.
    std::raise(SIGINT);
    check(ProcessSignals::shutdownRequested(), "sigint stops the run");
    check(!ProcessSignals::reselectRequested(), "sigint does not reselect");
    check(probeShutdownRequested(), "sigint shutdown visible in another TU");
    check(!probeReselectRequested(), "sigint does not reselect in another TU");

    return failures == 0 ? 0 : 1;
}
