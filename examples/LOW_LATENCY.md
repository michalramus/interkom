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

**The buffer size itself** — the single biggest lever, and we never touch it.
`kAudioUnitProperty_MaximumFramesPerSlice` (which we do set) only promises *"I
can handle up to this"*; it does not request anything.
- `kAudioDevicePropertyBufferFrameSize` — the actual I/O quantum. Set this.
- `kAudioDevicePropertyBufferFrameSizeRange` — what the device permits. Here:
  **15 to 4096 frames** *(measured)*, against a current 512. 64 frames is
  1.33 ms, and a plausible target.
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
  ring buffer instead.

**Intercom-specific**
- `kAudioUnitSubType_VoiceProcessingIO` instead of `HALOutput` — gives echo
  cancellation and AGC, which an intercom wants. It *adds* latency, so it is a
  deliberate trade, not a free win.
- An aggregate device when capturing and playing at once, so input and output
  share one clock. Two independent clocks force resampling and drift correction.

## Cross-cutting

- **Never allocate, lock, or do I/O in a callback.** One `malloc` that hits the
  kernel costs you a whole buffer period.
- **One lock-free ring buffer** between the audio callback and everything else.
- **Match sample rates end to end.** Every conversion stage is both latency and
  CPU, and resamplers add their own delay.
- **Count every stage.** Capture buffer + encode + network jitter buffer +
  decode + playback buffer. Shrinking a 10 ms device buffer is pointless behind a
  200 ms jitter buffer.
- **Measure, don't assume.** The 260 ms above looked like a correct 20 ms request
  right up until it was read back with `pa_stream_get_buffer_attr()`.
