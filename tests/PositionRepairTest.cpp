/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "DemodCore.hpp"

#include <array>
#include <cstdint>

namespace {

struct CapturingHandler {
	void handleShort(uint64_t, uint64_t) {}

	void handleLong(uint64_t, const Bits128& frame) {
		if (longCount < frames.size())
			frames[longCount] = frame;
		++longCount;
	}

	uint32_t longCount { 0 };
	std::array<Bits128, 8> frames { Bits128(), Bits128(), Bits128(),
		Bits128(), Bits128(), Bits128(), Bits128(), Bits128() };
};

Bits128 makePosition(uint32_t icao, bool odd, uint32_t latCpr,
		uint32_t lonCpr, uint8_t capability = 5) {
	const uint64_t high = (uint64_t(17) << 43)
		| (uint64_t(capability) << 40)
		| (uint64_t(icao) << 16)
		| (uint64_t(11) << 11);
	const uint64_t low = (uint64_t(odd) << 58)
		| (uint64_t(latCpr) << 41)
		| (uint64_t(lonCpr) << 24);
	Bits128 frame(high, low);
	frame.low() |= CRC::compute<112>(frame);
	return frame;
}

Bits128 makeIdentity(uint32_t icao, uint8_t meType = 5, uint8_t capability = 5) {
	const uint64_t high = (uint64_t(17) << 43)
		| (uint64_t(capability) << 40)
		| (uint64_t(icao) << 16)
		| (uint64_t(meType) << 11);
	Bits128 frame(high, 0);
	frame.low() = CRC::compute<112>(frame);
	return frame;
}

void feedSilence(DemodCore<1, CapturingHandler>& demod, uint32_t bits) {
	for (uint32_t bit = 0; bit < bits; ++bit) {
		uint32_t value[] = { 0 };
		demod.shiftInNewBits(value);
	}
}

void feedFrame(DemodCore<1, CapturingHandler>& demod, const Bits128& frame) {
	for (int bit = 111; bit >= 0; --bit) {
		uint32_t value[] = { uint32_t(frame.get(bit)) };
		demod.shiftInNewBits(value);
	}
	feedSilence(demod, 16);
}

// Damaged so that the error table repair recovers the frame itself: the
// single flipped parity bit is what the repair undoes.
Bits128 makeRepairable(const Bits128& frame) {
	Bits128 damaged { frame };
	damaged.flip(0);
	return damaged;
}

bool repairedPairCannotBypassGlobalGate() {
	CapturingHandler handler;
	DemodCore<1, CapturingHandler> demod(handler);
	constexpr uint32_t icao = 0x345678;

	feedFrame(demod, makeIdentity(icao, 5));
	feedSilence(demod, 128);
	feedFrame(demod, makeIdentity(icao, 3));
	const auto cleanEven = makePosition(icao, false, 93000, 51372);
	const auto cleanOdd = makePosition(icao, true, 74158, 50194);
	feedFrame(demod, cleanEven);
	feedSilence(demod, 128);
	feedFrame(demod, cleanOdd);

	// Let both clean parities age out of the 10-second pairing window while
	// keeping their decoded reference and the aircraft trust alive.
	feedSilence(demod, 3'000'000);
	feedFrame(demod, makeIdentity(icao, 5));
	feedSilence(demod, 3'000'000);
	feedFrame(demod, makeIdentity(icao, 3));
	feedSilence(demod, 3'000'000);
	feedFrame(demod, makeIdentity(icao, 5));
	feedSilence(demod, 1'500'000);

	// This repaired even frame is locally 84 km from the reference, so it is
	// safe by itself and is emitted. A following repaired odd frame is also
	// locally near, but the two repaired parities form the 4700 km ghost pair.
	// The second repair must be checked against the first emitted repair.
	feedFrame(demod, makeRepairable(makePosition(icao, false, 76616, 51372)));
	if (handler.longCount != 7)
		return false;
	feedFrame(demod, makeRepairable(cleanOdd));
	return handler.longCount == 7;
}

} // namespace

int main() {
	CapturingHandler handler;
	DemodCore<1, CapturingHandler> demod(handler);

	// 0x123456 becomes trusted on the second sighting of a clean frame; two
	// identifying frames do that without ever carrying a position.
	feedFrame(demod, makeIdentity(0x123456, 5));
	feedSilence(demod, 128);
	feedFrame(demod, makeIdentity(0x123456, 3));
	if (handler.longCount != 1)
		return 1;

	// A repaired airborne position for a trusted aircraft with no clean
	// odd/even pair behind it must be rejected: the gate stays closed when
	// nothing is known, repairs never establish the first position.
	feedSilence(demod, 128);
	feedFrame(demod, makeRepairable(makePosition(0x123456, false, 93000, 51372)));
	if (handler.longCount != 1)
		return 2;

	// A CRC-clean even/odd pair establishes the reference position; both
	// frames are emitted.
	const auto firstEven = makePosition(0x123456, false, 93000, 51372);
	feedFrame(demod, firstEven);
	feedSilence(demod, 128);
	const auto firstOdd = makePosition(0x123456, true, 74158, 50194);
	feedFrame(demod, firstOdd);
	if (handler.longCount != 3)
		return 3;

	// A single-bit damage repair landing on the established position passes
	// the global pair check: the repair restores the clean even frame, which
	// pairs with the clean odd to the reference position.
	feedSilence(demod, 128);
	feedFrame(demod, makeRepairable(firstEven));
	if (handler.longCount != 4)
		return 4;

	// The ghost counterexample: a damaged even frame whose repaired CPR
	// decodes locally 84 km from the reference, inside the gate, but whose
	// global decode against the clean odd lands 4700 km away in another
	// latitude zone. The raw CPR bits go out on the wire, so the pair a
	// receiver would actually form is what the gate must validate.
	const auto ghost = makePosition(0x123456, false, 76616, 51372);
	feedSilence(demod, 128);
	feedFrame(demod, makeRepairable(ghost));
	if (handler.longCount != 4)
		return 5;

	// A repair that would place the aircraft far outside the reference zone
	// is rejected as well.
	const auto farAway = makePosition(0x123456, false, 0, 0);
	feedSilence(demod, 128);
	feedFrame(demod, makeRepairable(farAway));
	if (handler.longCount != 4)
		return 6;

	// --- a southern and western reference: the sign-safe local decode ---

	// 0x234567 becomes trusted like above.
	feedFrame(demod, makeIdentity(0x234567, 5));
	feedSilence(demod, 128);
	feedFrame(demod, makeIdentity(0x234567, 3));
	if (handler.longCount != 5)
		return 7;

	// A clean pair near Santiago (33.55 S, 70.80 W) seeds a negative
	// reference; both frames are emitted.
	const auto southEven = makePosition(0x234567, false, 53521, 47623);
	feedFrame(demod, southEven);
	feedSilence(demod, 128);
	const auto southOdd = makePosition(0x234567, true, 65736, 73400);
	feedFrame(demod, southOdd);
	if (handler.longCount != 7)
		return 8;

	// Age the odd parity out of the 10 s pair window while the reference
	// stays young and the entry alive: identifying frames keep the address
	// trusted, and they carry no position, so they refresh nothing else.
	// The ME type alternates because back to back identical frames are
	// dropped as duplicates before any of this runs.
	feedSilence(demod, 3'000'000);
	feedFrame(demod, makeIdentity(0x234567, 5));
	feedSilence(demod, 3'000'000);
	feedFrame(demod, makeIdentity(0x234567, 3));
	feedSilence(demod, 3'000'000);
	feedFrame(demod, makeIdentity(0x234567, 5));
	feedSilence(demod, 1'500'000);
	if (handler.longCount != 10)
		return 9;

	// With the opposite parity stale, the local decode against the negative
	// reference is all a receiver could reproduce: the sign-safe zone index
	// must place this repair at the reference, not a zone away.
	feedFrame(demod, makeRepairable(southEven));
	if (handler.longCount != 11)
		return 10;

	// and a repair decoding 400 km away from the southern reference, into a
	// zone the pre-fix fmod decomposition used to land in, is rejected.
	const auto southFar = makePosition(0x234567, false, 0, 0);
	feedFrame(demod, makeRepairable(southFar));
	if (handler.longCount != 11)
		return 11;

	return repairedPairCannotBypassGlobalGate() ? 0 : 12;
}
