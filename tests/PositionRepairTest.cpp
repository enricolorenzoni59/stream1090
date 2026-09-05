/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "DemodCore.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

struct CapturingHandler {
	void handleShort(uint64_t, uint64_t) {}

	void handleLong(uint64_t sampleTime, const Bits128& frame) {
		lastLong = frame;
		++longCount;
		longTimes.push_back(sampleTime);
		longFrames.push_back(frame);
	}

	uint32_t longCount { 0 };
	Bits128 lastLong;
	// Every timestamp and frame actually sent to the output, in emission
	// order. The gate must never buffer, so this doubles as the record of
	// what a downstream receiver would have seen.
	std::vector<uint64_t> longTimes;
	std::vector<Bits128> longFrames;
};

// The gate promises no buffering and no retroactive emission: output
// timestamps must be strictly increasing, and a frame withheld once must
// never appear later just because a subsequent frame satisfied the gate.
bool outputTimestampsAreMonotonic(const CapturingHandler& handler) {
	for (size_t i = 1; i < handler.longTimes.size(); ++i) {
		if (handler.longTimes[i] <= handler.longTimes[i - 1])
			return false;
	}
	return true;
}

bool frameWasNeverEmitted(const CapturingHandler& handler, const Bits128& frame) {
	for (const auto& emitted : handler.longFrames) {
		if (emitted == frame)
			return false;
	}
	return true;
}

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

Bits128 makeIdentification(uint32_t icao, uint8_t meType = 1,
		uint8_t capability = 5) {
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

void decodeCprRelativeForTest(double refLat, double refLon, bool odd,
		uint32_t latCpr, uint32_t lonCpr, double& lat, double& lon) {
	const double dlat = odd ? 360.0 / 59.0 : 360.0 / 60.0;
	const double normalizedLat = double(latCpr) / 131072.0;
	lat = dlat * (std::floor(0.5 + refLat / dlat - normalizedLat)
		+ normalizedLat);
	if (lat > 90.0) lat -= 180.0;
	if (lat < -90.0) lat += 180.0;

	const int zones = ModeS::cprNl(lat) - (odd ? 1 : 0);
	const int ni = zones > 1 ? zones : 1;
	const double dlon = 360.0 / double(ni);
	const double normalizedLon = double(lonCpr) / 131072.0;
	lon = dlon * (std::floor(0.5 + refLon / dlon - normalizedLon)
		+ normalizedLon);
	if (lon > 180.0) lon -= 360.0;
	if (lon < -180.0) lon += 360.0;
}

double distanceKmForTest(double lat1, double lon1, double lat2, double lon2) {
	constexpr double EarthRadiusKm = 6371.0;
	constexpr double DegreesToRadians = 3.14159265358979323846 / 180.0;
	const double deltaLat = (lat2 - lat1) * DegreesToRadians;
	const double deltaLon = (lon2 - lon1) * DegreesToRadians;
	const double a = std::sin(deltaLat * 0.5) * std::sin(deltaLat * 0.5)
		+ std::cos(lat1 * DegreesToRadians) * std::cos(lat2 * DegreesToRadians)
		* std::sin(deltaLon * 0.5) * std::sin(deltaLon * 0.5);
	return 2.0 * EarthRadiusKm * std::asin(std::sqrt(a));
}

bool fixturesHaveExpectedGeometry() {
	double refLat = 0.0;
	double refLon = 0.0;
	if (!ModeS::decodeCprGlobal(93000, 51372, 74158, 50194,
			true, refLat, refLon))
		return false;

	double localLat = 0.0;
	double localLon = 0.0;
	decodeCprRelativeForTest(refLat, refLon, false, 76616, 51372,
		localLat, localLon);
	const double localGhostKm = distanceKmForTest(
		refLat, refLon, localLat, localLon);

	double globalLat = 0.0;
	double globalLon = 0.0;
	if (!ModeS::decodeCprGlobal(76616, 51372, 74158, 50194,
			false, globalLat, globalLon))
		return false;
	const double globalGhostKm = distanceKmForTest(
		refLat, refLon, globalLat, globalLon);
	if (localGhostKm < 80.0 || localGhostKm > 90.0
			|| globalGhostKm < 4'000.0)
		return false;

	if (!ModeS::decodeCprGlobal(53521, 47623, 65736, 73400,
			true, refLat, refLon))
		return false;
	decodeCprRelativeForTest(refLat, refLon, false, 53521, 47623,
		localLat, localLon);
	const double localSouthKm = distanceKmForTest(
		refLat, refLon, localLat, localLon);
	decodeCprRelativeForTest(refLat, refLon, false, 0, 0,
		localLat, localLon);
	const double farSouthKm = distanceKmForTest(
		refLat, refLon, localLat, localLon);
	return localSouthKm < 1.0 && farSouthKm > 300.0;
}

bool repairedPairCannotBypassGlobalGate(bool followingClean) {
	CapturingHandler handler;
	DemodCore<1, CapturingHandler> demod(handler);
	constexpr uint32_t icao = 0x345678;

	feedFrame(demod, makeIdentification(icao, 1));
	feedSilence(demod, 128);
	feedFrame(demod, makeIdentification(icao, 3));
	const auto cleanEven = makePosition(icao, false, 93000, 51372);
	const auto cleanOdd = makePosition(icao, true, 74158, 50194);
	feedFrame(demod, cleanEven);
	feedSilence(demod, 128);
	feedFrame(demod, cleanOdd);

	// Let both clean parities age out of the 10-second pairing window while
	// keeping their decoded reference and the aircraft trust alive.
	feedSilence(demod, 3'000'000);
	feedFrame(demod, makeIdentification(icao, 1));
	feedSilence(demod, 3'000'000);
	feedFrame(demod, makeIdentification(icao, 3));
	feedSilence(demod, 3'000'000);
	feedFrame(demod, makeIdentification(icao, 1));
	feedSilence(demod, 1'500'000);

	// Locally near is insufficient: a later clean odd frame would pair with
	// this repair into a 4700 km ghost. It is withheld here for lack of a
	// fresh opposite parity too, and the next block must show it has no way
	// back in: no buffering, no retroactive emission once a later frame
	// satisfies the gate.
	const auto repairedEven = makePosition(icao, false, 76616, 51372);
	feedFrame(demod, makeRepairable(repairedEven));
	if (handler.longCount != 6)
		return false;

	feedFrame(demod, followingClean ? cleanOdd : makeRepairable(cleanOdd));
	if (!followingClean) {
		// Still no fresh opposite parity: also discarded, and the withheld
		// even repair from above has not surfaced.
		return handler.longCount == 6
			&& frameWasNeverEmitted(handler, repairedEven);
	}
	// The clean odd frame is emitted normally right on top of the discard,
	// and it is what went out -- not a delayed replay of repairedEven.
	if (handler.longCount != 7 || handler.lastLong != cleanOdd
			|| !frameWasNeverEmitted(handler, repairedEven))
		return false;

	// Once a clean opposite parity is available, valid repairs resume, and
	// the discard from before still never surfaces.
	feedFrame(demod, makeRepairable(cleanEven));
	return handler.longCount == 8 && handler.lastLong == cleanEven
		&& frameWasNeverEmitted(handler, repairedEven)
		&& outputTimestampsAreMonotonic(handler);
}

} // namespace

int main() {
	if (!fixturesHaveExpectedGeometry())
		return 1;

	if (!repairedPairCannotBypassGlobalGate(true))
		return 14;
	if (!repairedPairCannotBypassGlobalGate(false))
		return 15;

	CapturingHandler handler;
	DemodCore<1, CapturingHandler> demod(handler);

	// 0x123456 becomes trusted on the second sighting of a clean frame; two
	// identifying frames do that without ever carrying a position.
	feedFrame(demod, makeIdentification(0x123456, 1));
	feedSilence(demod, 128);
	const auto firstIdentification = makeIdentification(0x123456, 3);
	feedFrame(demod, firstIdentification);
	if (handler.longCount != 1 || handler.lastLong != firstIdentification)
		return 2;

	// A repaired airborne position for a trusted aircraft with no clean
	// odd/even pair behind it must be rejected: the gate stays closed when
	// nothing is known, repairs never establish the first position.
	feedSilence(demod, 128);
	feedFrame(demod, makeRepairable(makePosition(0x123456, false, 93000, 51372)));
	if (handler.longCount != 1 || handler.lastLong != firstIdentification)
		return 3;

	// A CRC-clean even/odd pair establishes the reference position; both
	// frames are emitted.
	const auto firstEven = makePosition(0x123456, false, 93000, 51372);
	feedFrame(demod, firstEven);
	feedSilence(demod, 128);
	const auto firstOdd = makePosition(0x123456, true, 74158, 50194);
	feedFrame(demod, firstOdd);
	if (handler.longCount != 3 || handler.lastLong != firstOdd)
		return 4;

	// A single-bit damage repair landing on the established position passes
	// the global pair check: the repair restores the clean even frame, which
	// pairs with the clean odd to the reference position.
	feedSilence(demod, 128);
	feedFrame(demod, makeRepairable(firstEven));
	if (handler.longCount != 4 || handler.lastLong != firstEven)
		return 5;

	// The ghost counterexample: a damaged even frame whose repaired CPR
	// decodes locally 84 km from the reference, inside the gate, but whose
	// global decode against the clean odd lands 4700 km away in another
	// latitude zone. The raw CPR bits go out on the wire, so the pair a
	// receiver would actually form is what the gate must validate.
	const auto ghost = makePosition(0x123456, false, 76616, 51372);
	feedSilence(demod, 128);
	feedFrame(demod, makeRepairable(ghost));
	if (handler.longCount != 4 || handler.lastLong != firstEven)
		return 6;

	// A rejected even repair must not enter the emitted-CPR cache. If it did,
	// this valid repaired odd frame would pair with the rejected ghost and fail.
	feedSilence(demod, 128);
	feedFrame(demod, makeRepairable(firstOdd));
	if (handler.longCount != 5 || handler.lastLong != firstOdd)
		return 7;

	// A repair that would place the aircraft far outside the reference zone
	// is rejected as well.
	const auto farAway = makePosition(0x123456, false, 0, 0);
	feedSilence(demod, 128);
	feedFrame(demod, makeRepairable(farAway));
	if (handler.longCount != 5 || handler.lastLong != firstOdd)
		return 8;

	// --- a southern and western reference ---

	// 0x234567 becomes trusted like above.
	feedFrame(demod, makeIdentification(0x234567, 1));
	feedSilence(demod, 128);
	const auto southIdentification = makeIdentification(0x234567, 3);
	feedFrame(demod, southIdentification);
	if (handler.longCount != 6 || handler.lastLong != southIdentification)
		return 9;

	// A clean pair near Santiago (33.55 S, 70.80 W) seeds a negative
	// reference; both frames are emitted.
	const auto southEven = makePosition(0x234567, false, 53521, 47623);
	feedFrame(demod, southEven);
	feedSilence(demod, 128);
	const auto southOdd = makePosition(0x234567, true, 65736, 73400);
	feedFrame(demod, southOdd);
	if (handler.longCount != 8 || handler.lastLong != southOdd)
		return 10;

	// Age the odd parity out of the 10 s pair window while the reference
	// stays young and the entry alive: identifying frames keep the address
	// trusted, and they carry no position, so they refresh nothing else.
	// The ME type alternates because back to back identical frames are
	// dropped as duplicates before any of this runs.
	feedSilence(demod, 3'000'000);
	feedFrame(demod, makeIdentification(0x234567, 1));
	feedSilence(demod, 3'000'000);
	feedFrame(demod, makeIdentification(0x234567, 3));
	feedSilence(demod, 3'000'000);
	const auto latestIdentification = makeIdentification(0x234567, 1);
	feedFrame(demod, latestIdentification);
	feedSilence(demod, 1'500'000);
	if (handler.longCount != 11 || handler.lastLong != latestIdentification)
		return 11;

	// Even a repair at the reference must wait for a fresh opposite parity:
	// discarded, not queued. (southEven's bits already went out once, as the
	// original clean frame, so it is longCount staying put that proves this
	// particular repair produced nothing new.)
	feedFrame(demod, makeRepairable(southEven));
	if (handler.longCount != 11 || handler.lastLong != latestIdentification)
		return 12;

	// The next clean opposite parity is emitted normally -- the discard above
	// does not delay or suppress it, and it is what goes out, not a
	// retroactive replay of the repair withheld just before it.
	feedFrame(demod, southOdd);
	if (handler.longCount != 12 || handler.lastLong != southOdd)
		return 16;

	// A discarded repair also leaves the clean reference and the emitted-CPR
	// history exactly as they were: this repair succeeds only because both
	// still hold the original clean pair, undisturbed by the discard above.
	feedFrame(demod, makeRepairable(southEven));
	if (handler.longCount != 13 || handler.lastLong != southEven)
		return 17;

	const auto southFar = makePosition(0x234567, false, 0, 0);
	feedFrame(demod, makeRepairable(southFar));
	if (handler.longCount != 13 || handler.lastLong != southEven
			|| !frameWasNeverEmitted(handler, southFar))
		return 13;

	// The whole sequence above went out with strictly increasing timestamps:
	// no buffering, no reordering, no retroactive emission anywhere in it.
	if (!outputTimestampsAreMonotonic(handler))
		return 18;

	return 0;
}
