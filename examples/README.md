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

All four use mono 48 kHz. Capture writes signed 16-bit little-endian; playback
generates at 0.25 amplitude so it is audible without being painful.

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
deliberately leave alone, and why the PulseAudio one currently runs at 260 ms.

### Notes

- **macOS microphone permission.** A plain CLI binary has no bundle, so the TCC
  prompt is attributed to the terminal app running it. Grant microphone access
  to that app (System Settings → Privacy & Security → Microphone), otherwise
  capture succeeds but records silence.
- **PulseAudio needs a running server.** Without one, the examples exit with
  `pa_context_connect: Connection refused`. A null sink is enough for testing:
  `pulseaudio -n --load=module-null-sink --load=module-native-protocol-unix`,
  then capture reads that sink's monitor source.
