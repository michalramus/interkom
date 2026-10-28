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
interkom_server <ip:recv:send>...                       # 1 to 32 clients
interkom_client <server-ip> <receive-port> <send-port>
```

All arguments are required; anything missing or unparsable prints usage and
exits. On the server, each client is `ip:recv:send`: `recv` is the server port
receiving that client's audio, `send` is the client's port the mix goes to. The
client's `receive-port` is that same `send`, and its `send-port` that `recv`.

Two clients on loopback:

```sh
./build/debug/interkom_server 127.0.0.1:5004:5006 127.0.0.1:5008:5010
./build/debug/interkom_client 127.0.0.1 5006 5004
./build/debug/interkom_client 127.0.0.1 5010 5008
```

Both print jitter buffer counters once a second; ctrl-c stops them. Audio and
jitter settings stay `constexpr` at the top of `src/server_main.cpp` and
`src/client_main.cpp`.
