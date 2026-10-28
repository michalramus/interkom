# interkom

Intercom over RTP: clients (beltpacks) send their mic to a server, which sends
each of them a mix of everyone else.

## Building

```sh
cmake --preset debug
cmake --build --preset debug
```

uvgRTP (v4.0.1, without Crypto++) is fetched and built by CMake on the first
configure; an installed copy found by `find_package(uvgrtp)` is used instead.
`interkom_server` and `interkom_client` are only built when an audio backend
(CoreAudio on macOS, PulseAudio elsewhere) is found.

## Running

```sh
./build/debug/interkom_server
./build/debug/interkom_client
```

Both print jitter buffer counters once a second; ctrl-c stops them.

There are no flags or config files: everything is a `constexpr` at the top of
the program.

- `src/server_main.cpp`: `kClients`, one `{address, receivePort, clientPort}`
  per client.
- `src/client_main.cpp`: `kServerAddress`, `kSendPort` (the client's
  `receivePort` on the server) and `kReceivePort` (its `clientPort`).

Both default to two clients on loopback; the client as shipped is client 0.
Running a second client on the same machine needs its ports swapped to
5008/5010.
