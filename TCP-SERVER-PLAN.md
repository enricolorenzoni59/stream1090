# Native TCP output server plan

## Goal

Replace the `stream1090 | socat ...` deployment with native, asynchronous TCP
outputs that expose decoded Mode-S frames to `readsb` in AVR/raw and Beast
binary formats. Network activity must never block SDR input, resampling,
demodulation, or frame repair.

The implementation must preserve the current stdout byte stream by default so
existing scripts remain compatible. Users opt into one or both TCP listeners
and may explicitly disable stdout once their consumer has moved to TCP.

## User-facing result

AVR/raw example:

```sh
stream1090 -s 2.4 -d configs/rtlsdr.ini \
  --net-bind-address 127.0.0.1 --net-avr-port 30006 --no-stdout

readsb --net --net-connector=127.0.0.1,30006,raw_in
```

Beast example:

```sh
stream1090 -s 6 -d configs/airspy.ini \
  --net-bind-address 127.0.0.1 --net-beast-port 30007 --no-stdout

readsb --net --net-connector=127.0.0.1,30007,beast_in
```

Both listeners may be enabled simultaneously. AVR and Beast use distinct ports;
there is no protocol sniffing or negotiation.

## Non-goals for the first implementation

- Acting as an outbound/reconnecting TCP client to readsb's `--net-ri-port` or
  `--net-bi-port`.
- TLS, authentication, HTTP, WebSocket, UDP, or service discovery.
- Windows support. The project already targets POSIX APIs; Linux and macOS are
  required.
- Unlimited clients or unlimited buffering.
- Replaying frames that existed before a client connected.
- Accepting Beast control commands. A decoder consuming this server only needs
  the outbound frame stream.

## Safety and performance invariants

1. The DSP thread never calls `socket`, `accept`, `poll`, `send`, or `close`.
2. Publishing a frame is bounded O(1), allocation-free, and `noexcept`.
3. The DSP thread never waits for the network thread or a client.
4. All listening sockets and connected client sockets are non-blocking.
5. Only the network thread owns and mutates sockets and client output buffers.
6. A slow client cannot delay another client and cannot grow memory without a
   fixed upper bound.
7. Queue overflow and slow-client disconnection are observable through
   rate-limited logs/counters, never logs emitted from the DSP hot path.
8. Shutdown wakes the network thread immediately, closes listeners and clients,
   joins the thread, and leaves no detached work behind.
9. Default behavior remains byte-compatible AVR output on stdout.
10. Binding defaults to loopback, avoiding accidental exposure on a LAN/WAN.

## Architecture

```text
                       DSP / demodulation thread
                                  |
                     tryPublish(ModeSFrame)
                       allocation-free, no wait
                                  |
                                  v
                     bounded SPSC frame ring
                                  |
                    drained at most 10 ms later
                                  |
                                  v
                       one TCP network thread
                 /                |               \
          AVR encoder       Beast encoder      control pipe
              |                  |               shutdown
       AVR client queues   Beast client queues
              |                  |
       non-blocking send   non-blocking send
```

The number of clients does not affect DSP/network synchronization. Fan-out
happens entirely after the network thread consumes a canonical frame.

## Canonical frame

The inter-thread value contains decoded data, not serialized bytes:

```cpp
enum class ModeSFrameLength : uint8_t { Short = 56, Long = 112 };

struct ModeSFrame {
    uint64_t mlatTimestamp; // low 48 bits, 12 MHz clock
    uint64_t high;          // low 48 bits used for a long frame
    uint64_t low;           // 56-bit short frame or low 64 long-frame bits
    uint8_t signalLevel;
    ModeSFrameLength length;
};
```

Properties:

- trivially copyable;
- no pointers and no ownership ambiguity;
- no dynamic allocation;
- timestamp and RSSI are calculated while the retained sample window is valid;
- the same value can be encoded as AVR and Beast without revisiting DSP state.

When RSSI support is compiled out, `signalLevel` is zero and AVR uses its
non-RSSI form. Beast still includes the protocol-mandated signal byte.

## DSP-to-network queue

Use a fixed-capacity single-producer/single-consumer ring. Producer and consumer
indices are separate cache-line-aligned atomics. Publication uses release store;
observation uses acquire load. Slots are plain `ModeSFrame` objects.

Initial capacity: 4096 slots. This is roughly 128 KiB and covers several seconds
at a busy receiver. The network thread normally drains it every 10 ms.

Full-queue policy:

- reject the newest frame;
- increment an atomic dropped-frame counter;
- return immediately;
- network/watchdog code reports the accumulated count at a controlled cadence.

Dropping the newest frame preserves the already established order in the ring.
Blocking, overwriting an unread slot, heap growth, or logging from `tryPush()` are
forbidden.

## Wakeup and batching

The network thread calls `poll()` with a 10 ms timeout, matching the existing
`SampleStream::FlushIntervalMicros`. It drains the SPSC ring after every return,
including timeout returns. This batches normal traffic without a syscall from
the DSP thread for every decoded frame.

A non-blocking POSIX pipe is included in the poll set only for control wakeups:

- shutdown;
- future runtime reconfiguration;
- future explicit flush requests.

The pipe is drained fully when readable. `eventfd` is deliberately avoided to
retain macOS portability; `ppoll` is avoided for the same reason.

## Encoders

Encoders are pure, allocation-free functions returning a fixed-size byte buffer
and used length. They have no socket, stream, or thread knowledge.

### AVR/raw

Preserve the existing uppercase hexadecimal formats exactly:

- without RSSI: `@` + 12 timestamp hex digits + payload + `;\n`;
- with RSSI: `<` + 12 timestamp hex digits + 2 RSSI hex digits + payload + `;\n`;
- short payload: 14 hex digits;
- long payload: 28 hex digits.

The existing `AVRWriter` remains the stdout adapter initially. Encoder tests pin
the native TCP form to the existing wire representation.

### Beast binary

Generate standard Beast frames:

- unescaped start marker `0x1a`;
- ASCII type `2` for 56-bit Mode-S and `3` for 112-bit Mode-S;
- six-byte big-endian 12 MHz timestamp;
- one signal byte;
- seven or fourteen Mode-S bytes in network order;
- every `0x1a` occurring after the start marker is escaped as `0x1a 0x1a`.

The encoder buffer is sized for the worst case where every body byte requires
escaping.

## TCP server event loop

One thread owns:

- zero or one AVR listening socket;
- zero or one Beast listening socket;
- the read end of the control pipe;
- all accepted clients;
- all client output buffers.

Each iteration:

1. Construct/update `pollfd` entries.
2. Request `POLLIN` for listeners and the control pipe.
3. Request `POLLOUT` for a client only when it has pending bytes.
4. Call `poll(..., 10)` and retry on `EINTR`.
5. Accept all currently available clients until `EAGAIN`.
6. Remove sockets reporting error, hangup, or peer shutdown.
7. Drain the canonical frame ring completely.
8. Encode one AVR and/or Beast batch only if that protocol has clients.
9. Append the batch to each matching client if it fits its bound.
10. Disconnect clients whose pending data would exceed the bound.
11. Perform partial non-blocking sends and retain unsent suffixes.
12. Compact buffers only when useful; do not erase from the front per write.

The initial per-client pending-data limit is 256 KiB. `MSG_NOSIGNAL` is used
where available; otherwise SIGPIPE must be prevented at process level or by the
platform socket option.

Accepted sockets use close-on-exec and non-blocking mode. `SO_KEEPALIVE` is
enabled. `TCP_NODELAY` is acceptable, though 10 ms application batching means
correctness does not depend on it.

## Binding and address handling

`getaddrinfo()` with `AI_PASSIVE` resolves the configured bind address and port.
Try returned addresses until bind succeeds. Set `SO_REUSEADDR`. IPv4 and IPv6
are supported according to the host resolver/socket stack.

Default bind address: `127.0.0.1`. Exposing `0.0.0.0` or `::` is an explicit user
choice. Startup fails before SDR acquisition if any requested listener cannot be
created, bound, or listened on; partial startup is not silently accepted.

## CLI and validation

New options:

```text
--net-bind-address <address>  Bind address (default 127.0.0.1)
--net-avr-port <1..65535>     Enable AVR/raw TCP server
--net-beast-port <1..65535>   Enable Beast TCP server
--no-stdout                   Disable legacy AVR stdout output
```

Rules:

- decimal ports only, range 1..65535;
- AVR and Beast ports must differ when both are enabled;
- `--no-stdout` requires at least one TCP output;
- bind address without a TCP port is accepted but has no effect;
- invalid values fail before device initialization;
- help text shows matching readsb `raw_in` and `beast_in` examples.

The parsed values enter an `OutputConfig` held in `RuntimeVars`. Non-copyable
server state is created only inside the selected `MainInstance` and is not put
in `RuntimeVars`, preserving preset dispatch semantics.

## Message-handler integration

The message handler continues to calculate the MLAT timestamp and RSSI at the
point a frame is recognized. It then:

1. writes through the existing `AVRWriter` when stdout is enabled;
2. calls `TcpOutputServer::tryPublish()` when a TCP output is enabled.

Both paths receive identical timestamp, payload, and signal values. `flush()`
continues to flush stdout; TCP flushing belongs exclusively to the network
thread.

The server starts before the sample/device loop and stops after the final frame
has been published. Normal stop attempts a short bounded drain before closing;
shutdown never waits indefinitely for client delivery.

## Logging and observability

Log on stderr through the existing logger:

- each successful listener address/port/protocol;
- client connect and disconnect with protocol and peer;
- listener/startup failure with OS error;
- slow-client disconnection and queued bytes;
- frame-ring overflow count, rate limited;
- total accepted clients, slow disconnects, and dropped frames at shutdown.

Never mix logs into stdout because stdout remains a machine-readable AVR feed.

## TDD strategy

Every behavior is introduced by a failing test before production code.

### Layer 1: canonical frame and SPSC ring

- empty ring cannot pop;
- push/pop preserves a short and a long frame exactly;
- FIFO ordering across wraparound;
- full ring rejects without corrupting unread data;
- capacity becomes reusable after pops;
- sustained producer/consumer concurrency preserves sequence and contents;
- compile-time assertions for trivial copyability and power-of-two capacity.

### Layer 2: pure AVR encoder

- exact short frame without RSSI;
- exact long frame without RSSI;
- exact short frame with RSSI;
- exact long frame with RSSI;
- timestamp masks to 48 bits;
- payload masks to 56/112 bits;
- uppercase fixed-width hexadecimal output.

### Layer 3: pure Beast encoder

- exact unescaped short frame;
- exact unescaped long frame;
- big-endian timestamp and payload ordering;
- timestamp masks to 48 bits;
- signal byte position;
- body timestamp byte `0x1a` is doubled;
- signal `0x1a` is doubled;
- payload `0x1a` is doubled;
- initial start marker is not doubled;
- maximum-size escaping remains inside the fixed buffer.

### Layer 4: TCP integration

- bind to loopback and an ephemeral port;
- connect one AVR client and receive an exact frame;
- connect one Beast client and receive an exact frame;
- two clients on one listener receive identical ordered data;
- AVR and Beast listeners operate simultaneously;
- a disconnected client is removed without stopping the server;
- a replacement client can connect and receive later frames;
- stop wakes `poll()` and joins promptly;
- publishing with no client is harmless;
- queue-overflow counter is observable;
- slow client reaches its bound and is disconnected without affecting a reader.

### Layer 5: application regression

- existing build configurations compile with TCP outputs disabled;
- stdout output remains byte-identical for replay input;
- CLI rejects malformed ports, duplicate protocol ports, and outputless config;
- CLI help contains native TCP/readsb examples;
- a known frame sent to a real readsb `raw_in` connector is accepted;
- the same frame sent to `beast_in` is accepted with timestamp and RSSI;
- SIGINT/SIGTERM shuts down with connected clients and no hang;
- full project test suite passes under AddressSanitizer/UndefinedBehaviorSanitizer
  when practical.

## Implementation checklist

### Planning and baseline

- [x] Document architecture, invariants, protocols, CLI, and failure policies.
- [x] Confirm stdout currently runs synchronously in the DSP thread.
- [x] Select one `poll()` network thread rather than a thread per client.
- [x] Select a bounded SPSC ring and 10 ms network cadence.
- [x] Record a clean baseline build and test run.

### Red: frame/queue/encoder tests

- [x] Add canonical frame test fixtures.
- [x] Add failing SPSC empty/full/FIFO/wrap tests.
- [x] Add failing SPSC concurrent stress test.
- [x] Add failing exact AVR encoder tests.
- [x] Add failing exact Beast encoder and escaping tests.
- [x] Register tests in CMake and demonstrate the expected red state.

### Green: frame/queue/encoders

- [x] Implement `ModeSFrame`.
- [x] Implement fixed-capacity SPSC ring.
- [x] Implement allocation-free AVR encoder.
- [x] Implement allocation-free Beast encoder.
- [x] Make all layer 1-3 tests green.
- [x] Refactor only after green and rerun tests.

### Red: network behavior

- [x] Add failing loopback/ephemeral-port server test.
- [x] Add failing one-client AVR delivery test.
- [x] Add failing one-client Beast delivery test.
- [x] Add failing multi-client fan-out/order test.
- [x] Add failing reconnect and prompt-stop tests.
- [x] Add failing slow-client isolation test.

### Green: TCP server

- [x] Implement listener creation and address resolution.
- [x] Implement non-blocking accepted sockets.
- [x] Implement control pipe and prompt stop.
- [x] Implement `poll()` event loop.
- [x] Implement protocol-specific client ownership.
- [x] Implement partial writes and bounded pending buffers.
- [x] Implement slow-client disconnection.
- [ ] Implement counters and rate-limited logging.
- [x] Make implemented layer 4 tests green.
- [x] Verify frame, queue, and loopback server tests with ASan/UBSan.

### Application integration

- [x] Add `OutputConfig` to runtime configuration.
- [x] Add and validate the four CLI options.
- [x] Start listeners before starting the SDR/device stream.
- [x] Publish canonical frames from RSSI and non-RSSI handlers.
- [x] Preserve optional stdout output and flush cadence.
- [x] Drain and join server during both normal and signal shutdown.
- [x] Update help, README, package description, and examples.
- [x] Remove the Debian recommendation on `socat`.

### Acceptance

- [x] Complete build succeeds with Airspy and RTL-SDR enabled.
- [x] Complete CTest suite passes.
- [ ] Existing stdout replay is byte-identical.
- [x] AVR TCP feed works with local readsb 3.16.16 `raw_in`.
- [x] Beast TCP feed works with local readsb 3.16.16 `beast_in`.
- [x] Two simultaneous clients receive the same ordered frames.
- [x] A stalled client is disconnected while a healthy client receives every test frame.
- [ ] SIGINT/SIGTERM shutdown is prompt and leak-free.
- [x] Documentation no longer requires `socat` for readsb.

## Decisions deliberately deferred

- Outbound connector mode can reuse the same encoders and client-buffer logic,
  but needs DNS/reconnection/backoff policy and is a separate feature.
- Runtime SIGHUP reconfiguration of listeners should be added only after static
  startup/shutdown behavior is proven.
- A shared immutable encoded-chunk ring could reduce copies for dozens of
  clients, but copying tens of bytes to at most three clients is simpler and
  cheaper than additional ownership machinery.
- Configurable queue/client limits should be exposed only if real measurements
  show the defaults are inadequate.

## Independent review of the current implementation

Status: review only, no source changes made. This section is written so a
follow-up agent can act on it directly. Line references are against the
working-tree revision reviewed.

### Verification performed

- Configured a throwaway out-of-tree build (nothing in the repo was modified).
- `stream1090` plus `tcp_frame_test` and `tcp_output_server_test` compile clean
  with the project's `-Wall -Wextra` flags.
- `tcp_frame_test` and `tcp_output_server_test` pass, including the real
  `readsb` `raw_in` and `beast_in` acceptance test (3.2 s total).
- The remaining 14 CTest cases were not built for this review, so their
  "Not Run" status is not a regression.

### Confirmed sound

- Single network thread, `poll()` at 10 ms, one bounded SPSC ring with
  monotonic indices and correct acquire/release ordering. `tryPublish` is
  `noexcept`, allocation-free, and never blocks the DSP thread.
- AVR encoder is byte-identical to `AVRWriter` (same `@`/`<` prefixes, same
  widths, uppercase). Beast long-frame layout (type `2`/`3`, 6-byte big-endian
  timestamp, signal byte, 7/14 payload bytes, `0x1a` doubled everywhere after
  the start marker and not on the marker itself) is correct.
- Loopback bind default, `SO_REUSEADDR`, `O_NONBLOCK` + `FD_CLOEXEC`,
  `SO_KEEPALIVE`/`TCP_NODELAY`, `MSG_NOSIGNAL` (Linux) and `SO_NOSIGPIPE`
  (macOS), control-pipe shutdown with join.
- CLI parsing/validation: `--no-stdout` requires at least one TCP output,
  AVR/Beast ports must differ, ports constrained to 1..65535.
- Debian package no longer recommends `socat`.

### Findings and action items

Severity: P1 should be fixed before calling the feature done; P2 is worth doing
while the code is hot; P3 is polish or follow-up.

- [ ] **P1 - README uses `--net` instead of `--net-only`** (README.md:331,
      339, 355). The integration test correctly uses `--net-only`
      (tests/TcpOutputServerTest.cpp:235). With `--net`, readsb may still try to
      open a local SDR, which contradicts the "detach the device" design.
      Action: change the three `readsb` examples and the systemd
      `NET_OPTIONS` note to `--net-only`.
- [ ] **P1 - `fork()` in a multithreaded test** (tests/TcpOutputServerTest.cpp:224).
      In the child, before `exec`, the code calls non-async-signal-safe
      functions (`std::to_string`, `std::string` concatenation, `execl` is
      fine). With the server thread running this is technically UB and can
      deadlock if the malloc lock was held at fork. Action: build the
      connector argument in the parent and pass it to the child (e.g. via a
      second pipe or a global), or `snprintf` into a fixed stack buffer in the
      child.
- [ ] **P1 - `find_program(readsb)` makes the most valuable test silently
      skippable** (CMakeLists.txt:378-382). If `readsb` is absent the real
      interop test is compiled out without any signal. Action: make it an
      explicit cache option (for example `STREAM1090_READSB_TEST_EXECUTABLE`)
      and only define the macro when the user opted in, so a CI that wants the
      coverage fails loudly rather than skipping.
- [ ] **P2 - Documentation drift: canonical frame omits `signalAvailable`.**
      The plan's struct at "Canonical frame" (lines 98-105) has no
      `signalAvailable`, but `ModeSFrame.hpp:16` adds it (correctly: otherwise
      "RSSI disabled" and "RSSI == 0" are indistinguishable). Action: update
      the canonical-frame block and the surrounding prose.
- [ ] **P2 - Several checklist boxes are `[x]` without a corresponding test.**
      "Publishing with no client is harmless" and "queue-overflow counter is
      observable" are not covered by tests/TcpOutputServerTest.cpp, and there
      is no differential test pinning `encodeAvr` to `AVRWriter`. Action:
      either add the tests or uncheck the boxes.
- [ ] **P2 - `run_sync_stdin` logs neither listeners nor final counters.**
      `run_async_device` logs bind addresses/ports (MainInstance.hpp:150-155)
      and shutdown drop/slow-client counts (MainInstance.hpp:329-331);
      `run_sync_stdin` (MainInstance.hpp:337-357) does not. Action: mirror the
      async logging.
- [ ] **P2 - Server state allocated even when TCP is disabled.**
      `TcpOutputServer tcpServer(m_runtimeVars.tcpOutput)` is constructed
      unconditionally (MainInstance.hpp:141, 338), so the ~128 KiB
      `SpscFrameQueue<4096>` is heap-allocated for legacy stdout-only runs.
      Action: construct the server only inside the enable check, or make
      `Impl` lazy.
- [ ] **P2 - Shutdown drain is best-effort, not the promised bounded drain.**
      `stop()` lets the worker run one final `distributeFrames` + `flushClient`
      iteration, then `cleanup()` closes sockets, so an unsent suffix on a
      momentarily full socket is dropped. Usually fine because stop follows the
      DSP loop and watchdog join. Action: document the actual behavior in the
      plan, or add an explicit short drain loop before close.
- [ ] **P3 - Fan-out work per frame.** `distributeFrames` recomputes two
      `std::any_of` passes and re-encodes per popped frame
      (src/TcpOutputServer.cpp:214-237); `run()` allocates a fresh
      `std::vector<pollfd>` every iteration (src/TcpOutputServer.cpp:268).
      Action: hoist the per-protocol client presence and encoding out of the
      loop, and reuse/reserve the poll vector.
- [ ] **P3 - Missing observability from the plan.** Client connect/disconnect
      logging and rate-limited frame-ring overflow logging are listed in the
      plan (lines 272-277) and remain unimplemented (checklist line 390 is
      correctly unchecked). `droppedFrames()` is only surfaced at async
      shutdown. Action: implement counters/logging or explicitly defer.
- [ ] **P3 - Empty bind address means wildcard.** `--net-bind-address ""`
      reaches `getaddrinfo(nullptr, ..., AI_PASSIVE)`
      (src/TcpOutputServer.cpp:83), i.e. bind all interfaces. Action: reject
      the empty string, or require an explicit wildcard value.
- [ ] **P3 - `poll()` errors other than `EINTR` are not handled**
      (src/TcpOutputServer.cpp:282-285). On a hard error the code proceeds to
      read `revents` that may be stale. Action: `if (ready < 0) continue;`.
- [ ] **P3 - No cap on accepted clients.** Non-goal per the plan, but on a
      non-loopback bind it is a resource-consumption vector. Action: add a
      fixed maximum or document the intent.
- [ ] **P3 - `events`/`clients` indexing relies on append-only accept.**
      This is currently correct (accepts append and `removeClosedClients`
      runs at the end of the iteration), but it is implicit. Action: leave a
      short comment so a future edit does not break the index mapping.
- [ ] **P3 - Decide the fate of this plan file.** It is currently untracked. If
      it should ship, move it under `docs/` and keep it in sync; otherwise
      exclude it from the repository.

### Deferred / non-blocking

- Outbound connector mode, SIGHUP listener reconfiguration, shared
  encoded-chunk fan-out, and configurable limits remain reasonable deferred
  items; nothing in this review changes those decisions.
- No blocking correctness or memory-safety defect was found in the reviewed
  revision.
