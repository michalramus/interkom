# Examples

Standalone backend examples. Each one targets a single platform audio API and
uses its **async/callback** path — the same shape a real intercom needs, where
the driver pulls audio on its own thread rather than the app blocking on reads
and writes.

| Example | Backend | Direction |
| --- | --- | --- |
| `pulseaudio/capture.cpp` | `libpulse` threaded mainloop | mic → raw file |
| `pulseaudio/playback.cpp` | `libpulse` threaded mainloop | 440 Hz sine → speakers |
| `coreaudio/capture.cpp` | AUHAL (`kAudioUnitSubType_HALOutput`) | mic → raw file |
| `coreaudio/playback.cpp` | AUHAL (`kAudioUnitSubType_HALOutput`) | 440 Hz sine → speakers |
| `rtp/pulseaudio_send.cpp` | `libpulse` + `rtp/rtp.hpp` | mic → RTP |
| `rtp/pulseaudio_recv.cpp` | `libpulse` + `rtp/rtp.hpp` | RTP → speakers |
| `rtp/coreaudio_send.cpp` | AUHAL + `rtp/rtp.hpp` | mic → RTP |
| `rtp/coreaudio_recv.cpp` | AUHAL + `rtp/rtp.hpp` | RTP → speakers |
| `rtp/uvgrtp_send.cpp` | uvgRTP | sine → RTP |
| `rtp/uvgrtp_recv.cpp` | uvgRTP | RTP → counters |
| `rtp/uvgrtp_coreaudio_recv.cpp` | uvgRTP + AUHAL | RTP → jitter buffer → speakers |

All of them use mono 48 kHz. The four file-based examples write and read signed
16-bit little-endian; the two playback examples generate at 0.25 amplitude so
they are audible without being painful. The RTP pair is documented in
[its own section](#rtp) below.

## Building

Examples build by default as part of the normal configure:

```sh
cmake --preset debug
cmake --build --preset debug
```

Each backend is configured only when its dependencies are present: the
PulseAudio examples need `libpulse` on the `pkg-config` path, the CoreAudio
examples need Apple. Missing dependencies are reported as a status message and
skipped, not an error. Turn the whole set off with
`-DINTERKOM_BUILD_EXAMPLES=OFF`.

Binaries land in `build/<preset>/examples/`.

## Running

```sh
./build/debug/examples/example_pulseaudio_playback          # ctrl-c to stop
./build/debug/examples/example_pulseaudio_capture out.raw   # ctrl-c to stop
./build/debug/examples/example_coreaudio_playback
./build/debug/examples/example_coreaudio_capture out.raw
./build/debug/examples/example_rtp_coreaudio_send               # mic -> RTP
./build/debug/examples/example_rtp_coreaudio_recv               # RTP -> speakers
```

Capture defaults to `capture.raw` when no path is given. Inspect a recording
with:

```sh
ffplay -f s16le -ar 48000 -ch_layout mono out.raw
```

Raw PCM carries no header, so those flags *are* the format description — get
them wrong and the file still plays, just at the wrong speed. Note `-ch_layout`
rather than `-ac`: ffplay only exposes the demuxer's own options, and `-ac` was
removed in ffmpeg 9. Convert to a self-describing file with:

```sh
ffmpeg -f s16le -ar 48000 -ch_layout mono -i out.raw out.wav
```

See [LOW_LATENCY.md](LOW_LATENCY.md) for the tuning knobs these examples
deliberately leave alone, why the PulseAudio one currently runs at 260 ms, and
where the RTP pair's latency actually goes.

## RTP

`rtp/rtp.hpp` is the whole transport: a header-only `rtp::Sender` and
`rtp::Receiver` over plain POSIX UDP, with no dependency beyond libc. The four
`rtp/*.cpp` files are just the audio plumbing around them, so each backend's
pair is the previous `capture.cpp`/`playback.cpp` with the file swapped for a
socket.

Using it is two lines:

```cpp
rtp::Sender tx{"192.168.1.50"};   tx.start();  tx.send(samples, frames);
rtp::Receiver rx{rtp::kPort, 2};  rx.start();  rx.pull(out, frames);
```

Everything configurable is a `constexpr` at the top of `rtp.hpp`, or of the
example itself: there are no command-line flags. The destination address is
`kDestination` in the two send examples, the jitter depth is `kJitterPackets`
in the two receive examples.

### What goes on the wire

RFC 3550 header, RFC 3551 L16 payload: big-endian signed 16-bit, mono 48 kHz,
10 ms (480 frames, 960 bytes) per packet, dynamic payload type 96. No codec, so
no algorithmic delay at all — the cost is 1.5 Mbit/s, which is free on a LAN and
the wrong trade over a WAN. `L16/48000/1` has no static payload type (PT 11 is
44.1 kHz), hence the dynamic one plus the rtpmap in `rtp/l16.sdp`.

### Neither direction blocks the audio thread

The audio callback never touches a socket. Capture converts and queues, and a
sender thread woken by a semaphore does the `send()`. A receive thread sits
blocked in `recv()` and fills a jitter buffer, which the render callback drains
with a lock-free read. Each handoff costs one thread wakeup — tens of
microseconds against a 10 ms packet.

What the handoff is *not* is a timer. The sender thread transmits when the
callback hands it a packet's worth and sleeps otherwise, so pacing comes from
the audio device clock and there is no second clock to drift against.

### Running a pair

Both examples default to loopback, so one machine can talk to itself:

```sh
./build/debug/examples/example_rtp_coreaudio_recv &
./build/debug/examples/example_rtp_coreaudio_send
```

Beware that this is microphone into speakers on one box — use headphones or it
will feed back. For two machines, change `kDestination` in the send example.

Each prints a counter line per second. On the receive side `lost`, `late`,
`underruns` and `overruns` are the ones worth watching; `underruns` climbing is
the signal to raise `kJitterPackets`. On the send side, `errors` climbing from
zero usually means *nobody is listening*: the socket is `connect()`ed, so a
loopback ICMP port-unreachable comes straight back as a failed `send()`.

### Checking it against ffmpeg

Receive from ffmpeg instead of our sender:

```sh
./build/debug/examples/example_rtp_coreaudio_recv &
ffmpeg -re -f lavfi -i "sine=frequency=440:sample_rate=48000" \
       -ac 1 -c:a pcm_s16be -payload_type 96 -f rtp rtp://127.0.0.1:5004
```

Expect a steady underrun count from this one: `-re` paces at roughly 94 packets
per second against the 100 the device consumes *(measured)*, so the receiver
starves on purpose. That is ffmpeg's pacing, not the jitter buffer — fed at an
exact 100 packets per second, the same buffer runs 500 packets with zero
underruns *(measured)*.

Send to ffplay instead of our receiver:

```sh
ffplay -protocol_whitelist file,rtp,udp -i examples/rtp/l16.sdp &
./build/debug/examples/example_rtp_coreaudio_send
```

`rtp/l16.sdp` describes the stream for tools that need one; ffplay cannot infer
a dynamic payload type on its own.

## uvgRTP

Three examples using [uvgRTP](https://ultravideo.fi/uvgrtp.html) instead of a
hand-rolled socket, to see what a library buys.

`uvgrtp_send.cpp` and `uvgrtp_recv.cpp` are deliberately the *minimum*: no audio
device, no buffering, just the least code that puts an L16 frame on the wire and
takes one off again. Sending is four calls:

```cpp
uvgrtp::context ctx;
uvgrtp::session* session = ctx.create_session("127.0.0.1");
uvgrtp::media_stream* stream = session->create_stream(5004, RTP_FORMAT_L16_MONO, RCE_SEND_ONLY);
stream->configure_ctx(RCC_CLOCK_RATE, 48000);
stream->push_frame(payload, 960, RTP_NO_FLAGS);
```

Receiving is the same, with `RCE_RECEIVE_ONLY` and a hook:

```cpp
stream->install_receive_hook(nullptr, on_frame);
// void on_frame(void*, uvgrtp::frame::rtp_frame* f)
//   f->payload, f->payload_len, f->header.seq ... then dealloc_frame(f)
```

Two details that are not obvious. `RTP_FORMAT_L16_MONO` is RFC 3551 payload
type 11, whose clock rate is fixed at 44.1 kHz — without `RCC_CLOCK_RATE` the
timestamps are wrong for a 48 kHz stream. And uvgRTP does not byte-swap: L16 is
big-endian on the wire and the payload is passed through as given.

`uvgrtp_coreaudio_recv.cpp` is the realistic one: uvgRTP's receive hook, a jitter
buffer, and AUHAL playback in one file.

### uvgRTP has no jitter buffer, and that is normal

The only `jitter` in the whole uvgRTP source is the RTCP report *field* — the
interarrival jitter a receiver measures and reports. `RCC_RING_BUFFER_SIZE` is a
socket-read staging buffer so the OS does not drop datagrams; the hook still
fires the instant a packet lands.

No RTP library provides a playout buffer, because the library cannot know the
things one needs: your playout clock, how much latency you will accept, and how
you want to conceal a missing packet. WebRTC keeps that in a separate component
(NetEQ) for the same reason.

So the buffer is not optional. uvgRTP delivers packets when the network delivers
them; CoreAudio demands exactly 480 frames every 10 ms, on time, forever. Without
something in between, every callback that arrives a fraction of a millisecond
ahead of its packet produces silence.

### What the library replaces

| | hand-rolled (`rtp.hpp`) | uvgRTP |
| --- | --- | --- |
| Socket, RTP header, sequence, timestamps | ~120 lines | 4 calls |
| Jitter buffer | yours | yours |
| Audio plumbing | yours | yours |
| RTCP, SRTP/ZRTP, multicast, fragmentation | absent | included |
| Dependency | libc | a library to build and ship |

For L16 audio at a fixed ptime, the part uvgRTP replaces is the part that is
already simple and already tested here. It earns its place the moment you want
RTCP receiver reports, SRTP, or video alongside the audio — and RTCP in
particular is what you need to adapt a jitter buffer automatically.

### Building these

Nothing to install: CMake fetches uvgRTP (pinned to v4.0.1, Crypto++ disabled)
on the first configure and builds it alongside the project. An installed uvgRTP
is preferred when `find_package(uvgrtp)` finds one, e.g. with
`-DCMAKE_PREFIX_PATH=$HOME/.local`.

### Running them

```sh
./build/debug/examples/example_uvgrtp_recv &            # counters only
./build/debug/examples/example_uvgrtp_send              # 3 s of 440 Hz

./build/debug/examples/example_uvgrtp_coreaudio_recv &  # plays it
./build/debug/examples/example_uvgrtp_send
```

These use uvgRTP's default payload type 11, so they talk to each other but not
to the `rtp.hpp` pair above, which uses 96. Add
`configure_ctx(RCC_DYN_PAYLOAD_TYPE, 96)` to both uvgRTP streams to bridge them.

### Notes

- **macOS microphone permission.** A plain CLI binary has no bundle, so the TCC
  prompt is attributed to the terminal app running it. Grant microphone access
  to that app (System Settings → Privacy & Security → Microphone), otherwise
  capture succeeds but records silence.
- **PulseAudio needs a running server.** Without one, the examples exit with
  `pa_context_connect: Connection refused`. A null sink is enough for testing:
  `pulseaudio -n --load=module-null-sink --load=module-native-protocol-unix`,
  then capture reads that sink's monitor source.
