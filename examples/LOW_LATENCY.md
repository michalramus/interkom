# Low-latency notes

What the examples **do not** do, and what to reach for when the goal is minimum
mouth-to-ear delay. Numbers marked *(measured)* were taken on a MacBook Air,
macOS 27, PulseAudio 17.0, built-in speakers at 48 kHz.

## Where the examples stand today

| | PulseAudio | CoreAudio AUHAL |
| --- | --- | --- |
| Transport | IPC to a separate daemon over a unix socket; audio itself crosses in shared memory | in-process, driver calls into you |
| Callback thread | `pa_threaded_mainloop` worker — an ordinary thread polling an fd | a real-time, time-constrained thread the HAL creates in your process |
| Direction of control | server *requests* N bytes → `on_stream_write`; server *notifies* → `on_stream_read` peeks a queue | driver *pulls* a buffer to fill; for input the callback is only a notification and you call `AudioUnitRender` yourself |
| Buffer sizing we set | `tlength`/`fragsize` = 20 ms, everything else `-1` | `MaximumFramesPerSlice` = 4096 only |
| Resulting latency | **260 ms** *(measured)* | device buffer **512 frames = 10.67 ms** *(measured)*, inherited, not chosen |

The PulseAudio number is the important one: we ask for 20 ms and get 260 ms.

### Why, and the fix

Leaving `maxlength`, `prebuf` and `minreq` as `-1` lets the server pick, and it
picks generously — `prebuf` alone came back as 255 ms. `PA_STREAM_ADJUST_LATENCY`
does not rescue this; it adjusts *around* the fields you left unset. Filling in
all four, with `maxlength == tlength` and `prebuf = 0`, is honoured exactly
*(measured)*:

| `maxlength` | `prebuf` | asked | got |
| --- | --- | --- | --- |
| `-1` | `-1` | 20 ms | 260 ms |
| `2 × tlength` | 0 | 20 ms | 40 ms (clamped up to `maxlength`) |
| `= tlength` | 0 | 20 ms | **20 ms** |
| `= tlength` | 0 | 10 ms | **10 ms** |
| `= tlength` | 0 | 5 ms | **5 ms** |

`prebuf = 0` means playback starts immediately instead of waiting for a cushion;
combined with `tlength == maxlength` the server has nowhere to hide extra audio.

One trap: `PA_STREAM_EARLY_REQUESTS` and `PA_STREAM_ADJUST_LATENCY` are mutually
exclusive — passing both fails the connect with `Invalid argument` *(measured)*.
`EARLY_REQUESTS` is the older "ask me sooner" mechanism; `ADJUST_LATENCY` is the
one to keep.

## PulseAudio knobs we did not use

**Buffer control**
- `maxlength`, `prebuf`, `minreq` — set them, per above. `minreq` is the smallest
  request the server will make; it bounds how often your callback runs.
- `pa_stream_set_buffer_attr()` — retune after connecting, without a reconnect.
- `pa_stream_get_timing_info()` / `pa_stream_update_timing_info()` — the real
  read/write indices, for measuring rather than guessing.
- `PA_STREAM_INTERPOLATE_TIMING` — lets `pa_stream_get_latency()` interpolate
  between server updates instead of only being fresh after a round trip.

**Flags**
- `PA_STREAM_START_CORKED` + `pa_stream_cork()` — connect, pre-roll, then start
  on your own schedule. The clean way to open a stream early and keep it warm,
  which beats paying device-open cost when someone presses talk.
- `PA_STREAM_FIX_FORMAT` / `FIX_RATE` / `FIX_CHANNELS` — adopt the sink's native
  parameters instead of forcing yours, removing a resample/remap stage.
- `PA_STREAM_NO_REMIX_CHANNELS` — never upmix/downmix.
- `PA_STREAM_DONT_MOVE` — pin to the chosen device so the server can't migrate
  you to something slower mid-call.
- `PA_STREAM_FAIL_ON_SUSPEND` — hear about a suspended device instead of silently
  feeding nothing.
- `PA_STREAM_NOT_MONOTONIC`, `PA_STREAM_VARIABLE_RATE`, `PA_STREAM_PASSTHROUGH`,
  `PA_STREAM_RELATIVE_VOLUME` — situational, not latency levers.

**Callbacks that tell you when you are too tight**
- `pa_stream_set_underflow_callback()` — the one that matters. Underruns are the
  cost of shrinking buffers; without this you shrink blind.
- `pa_stream_set_overflow_callback()` — the capture-side equivalent.
- `pa_stream_set_latency_update_callback()`, `set_buffer_attr_callback()`,
  `set_started_callback()`, `set_moved_callback()`, `set_suspended_callback()`,
  `set_event_callback()`.

**Stream metadata**
- A proplist with `PA_PROP_MEDIA_ROLE = "phone"` — marks the stream as telephony,
  which is what drives ducking and routing policy for an intercom.

**Daemon side** (no amount of client tuning beats a badly configured server)
- `default-fragments` / `default-fragment-size-msec` in `daemon.conf`.
- `high-priority`, `realtime-scheduling`, `rlimit-rtprio`.
- On Linux, `module-udev-detect tsched=0` and `tsched_buffer_size` when
  timer-based scheduling misbehaves; on macOS the device side is
  `module-coreaudio-detect` / `module-coreaudio-device` instead.

## CoreAudio knobs we did not use

**The buffer size itself** — the single biggest lever, untouched by the
capture/playback pair and used by the RTP pair.
`kAudioUnitProperty_MaximumFramesPerSlice` (which all of them set) only promises
*"I can handle up to this"*; it does not request anything.
- `kAudioDevicePropertyBufferFrameSize` — the actual I/O quantum. The RTP
  examples set it to 480 frames, one 10 ms packet, and the device honours that
  exactly on both the input and the output side *(measured)*. One callback then
  produces or consumes exactly one packet.
- `kAudioDevicePropertyBufferFrameSizeRange` — what the device permits. Here:
  **15 to 4096 frames** *(measured)*, against a default of 512. 64 frames is
  1.33 ms, and a plausible target below the packet-sized 480.
- `kAudioDevicePropertyUsesVariableBufferFrameSizes` — for devices that vary it.

**Avoiding conversion**
- `kAudioDevicePropertyNominalSampleRate` — set the device's rate to match your
  pipeline so AUHAL's sample-rate converter never engages.
- `kAudioUnitProperty_StreamFormat` — we declare mono while the device is
  **2 ch float32 48 kHz** *(measured)*, so AUHAL inserts a channel-mapping stage.
  Matching the native format removes it. Rates already match, so no SRC today.
- `kAudioDevicePropertyIOProcStreamUsage` — tell the HAL which streams you
  actually use so it does not move data you ignore.

**Measuring the real round trip** — latency is more than your buffer:
- `kAudioDevicePropertyLatency`, `kAudioStreamPropertyLatency`,
  `kAudioDevicePropertySafetyOffset`. Total ≈ buffer + safety offset + device +
  stream latency, each direction.

**Going under AUHAL**
- `AudioDeviceCreateIOProcID` + `AudioDeviceStart` — talk to the HAL directly and
  skip the AudioUnit wrapper and its converters entirely. Lowest overhead path.
- `kAudioDevicePropertyHogMode` — take the device exclusively; no mixing with
  other apps, lowest achievable latency.
- `kAudioHardwarePropertyRunLoop` — control which run loop gets HAL notifications.

**Real-time thread correctness**
- `kAudioDevicePropertyIOThreadOSWorkgroup` + `os_workgroup_join` (macOS 11+) —
  any helper thread doing audio work must join the device's workgroup, or the
  scheduler will not treat it as real-time.
- `THREAD_TIME_CONSTRAINT_POLICY` for threads you create yourself.
- Our `coreaudio/capture.cpp` calls `fwrite` **inside the render callback**. That
  is a real-time violation — file I/O can block for milliseconds. Fine for an
  example, wrong for a product: hand samples to a writer thread over a lock-free
  ring buffer instead. `rtp/rtp.hpp` is the version that does it properly, and
  the handoff costs tens of microseconds.

**Intercom-specific**
- `kAudioUnitSubType_VoiceProcessingIO` instead of `HALOutput` — gives echo
  cancellation and AGC, which an intercom wants. It *adds* latency, so it is a
  deliberate trade, not a free win.
- An aggregate device when capturing and playing at once, so input and output
  share one clock. Two independent clocks force resampling and drift correction.

## The RTP pair's budget

`rtp/rtp.hpp` and the four `rtp/*.cpp` examples are the first thing here that
has a *network* in the middle, which changes which number matters. Shrinking a
10 ms device buffer is pointless behind a 200 ms jitter buffer — so the jitter
buffer is the first thing to argue about, not the last.

One-way, mouth to ear, as the examples ship:

| Stage | Cost | Chosen or inherited |
| --- | --- | --- |
| Capture device buffer | **10 ms** *(measured)* | chosen: 480 frames, one packet |
| Convert + queue in the callback | nanoseconds | pure arithmetic, no syscall |
| Semaphore wakeup → sender thread | tens of µs | the price of a clean RT thread |
| Packetisation | 0 | one callback *is* one packet |
| Encode | 0 | L16 is not a codec |
| Network | yours | 1.5 Mbit/s mono |
| Jitter buffer | **20 ms** | chosen: `kJitterPackets = 2` |
| Playback device buffer | **10 ms** *(measured)* | chosen: 480 frames |

≈ **40 ms** plus the network and the HAL's own device and safety offsets, which
`kAudioDevicePropertyLatency` and `kAudioDevicePropertySafetyOffset` report and
nothing above counts.

Half of that is the jitter buffer, and it is the only stage that is pure policy:
`kJitterPackets = 1` takes the total to 30 ms, and anything less than one packet
is not a buffer at all.

### What the choices cost

**Sending from a thread rather than the callback.** The callback converts,
writes into a lock-free queue and posts a semaphore; a sender thread owns the
socket. The alternative — `send()` straight from the real-time thread — saves
the wakeup but puts a syscall on a thread that must never block. Measured at
101.9 packets per second with zero frames dropped, so the wakeup keeps up
easily *(measured)*.

**Pacing from the audio clock, not a timer.** The sender thread transmits when
the callback hands it a packet and sleeps otherwise. A timer-driven sender would
be a second clock: faster than the device and it starves, slower and its queue
grows without bound. Sequence and timestamp deltas came back as exactly 1 and
480 for every packet *(measured)*, which is what having one clock looks like.

**A fixed jitter buffer, not an adaptive one.** Underruns zero-fill, bump a
counter, and re-prime to the target. That is honest but blunt: each underrun
costs a packet or two of silence. Fed at exactly 100 packets per second, the
2-packet buffer ran 500 packets with **zero** underruns; fed by `ffmpeg -re`,
which paces at ~94 packets per second, it underran about 10 times a second
*(measured)* — the sink consuming faster than the source delivers, which no
buffer size fixes.

That last case is the one a product has to solve, and the fix is not a bigger
buffer. Two machines have two crystals; a few hundred ppm apart is normal, and
it accumulates forever. The real answer is drift compensation — resample by the
measured ratio, or time-stretch like WebRTC's NetEQ — which also buys graceful
concealment instead of silence.

### Network knobs we did use

- `connect()` on the UDP socket — pins the route once instead of resolving it
  per packet, and makes `send()` report ICMP port-unreachable as an error, so
  "nobody is listening" is visible rather than silent *(measured on loopback)*.
- `O_NONBLOCK` — a full socket buffer drops a packet instead of stalling the
  sender. For live audio, dropping late audio is correct.
- `IP_TOS = 0xB8` (DSCP EF) — marks the stream expedited forwarding, so managed
  switches and routers queue it ahead of bulk traffic.
- `SO_RCVTIMEO` — only so the receive thread notices a stop request without a
  wakeup pipe.

### Network knobs we did not use

- **Opus** (RFC 7587) instead of L16. 2.5 to 20 ms frames and roughly 1/50th the
  bitrate, at the cost of ~5 ms algorithmic delay at the shortest frame. The
  right trade the moment the path stops being a LAN.
- **RTCP** (RFC 3550) — receiver reports carry the loss and jitter the sender
  cannot see. Needed to adapt anything automatically.
- **An adaptive jitter buffer with time-stretching** — the single biggest
  remaining latency win, per the budget above.
- **Packet loss concealment** beyond silence — interpolate from the last good
  packet instead of muting.
- **`SO_RCVBUF` / `SO_SNDBUF`** — a large kernel buffer hides a burst and adds
  latency doing it; small is better here, and worth measuring.
- **RTP header extensions** for an audio level or a send timestamp, which is
  what lets a receiver measure one-way delay rather than infer it.
- **SRTP** if the path is not trusted. Encryption is cheap; the key exchange is
  the part that needs designing.
- **Multicast** for one-to-many intercom, instead of a packet per listener.
- **`SO_TIMESTAMPNS` / `SO_TIMESTAMP`** — kernel receive timestamps, for
  measuring real network jitter instead of guessing from arrival order.
- **Thread priority on the network threads.** They are ordinary threads today;
  under load the scheduler can delay them past a packet interval.

## Cross-cutting

- **Never allocate, lock, or do I/O in a callback.** One `malloc` that hits the
  kernel costs you a whole buffer period.
- **One lock-free ring buffer** between the audio callback and everything else.
- **Match sample rates end to end.** Every conversion stage is both latency and
  CPU, and resamplers add their own delay.
- **Count every stage.** Capture buffer + encode + network jitter buffer +
  decode + playback buffer. Shrinking a 10 ms device buffer is pointless behind a
  200 ms jitter buffer. The RTP budget above is what counting them looks like.
- **Measure, don't assume.** The 260 ms above looked like a correct 20 ms request
  right up until it was read back with `pa_stream_get_buffer_attr()`.
