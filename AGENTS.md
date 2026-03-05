## Cursor Cloud specific instructions

**libae** is a standalone C static library extracted from Redis's async event-driven I/O system. No external services, databases, or containers are needed.

### Build

- `make libae.a` — builds the static library
- `make timer echoserver echoclient` — builds examples
- `make clean` — removes all build artifacts
- See `Makefile` for all targets

### Run examples

- **Timer**: `./timer` — fires 9 timed "Hello World" events over ~9 seconds; the event loop does **not** exit after all events fire, use `timeout 12 ./timer` or Ctrl-C.
- **Echo server**: `./echoserver` — listens on TCP port **8000**. Output is buffered; use `stdbuf -oL ./echoserver` to see output in real-time.
- **Echo client**: `./echoclient` — connects 1024 concurrent TCP sessions to `127.0.0.1:8000`. Requires the echo server to be running first.

### Gotchas

- **stdout buffering**: all example binaries use `printf` without `fflush`, so running in background without `stdbuf -oL` will produce empty-looking logs until the buffer flushes.
- **Port reuse**: if `echoserver` crashes or is killed, port 8000 may stay in `TIME_WAIT`. Use `SO_REUSEADDR` (already set in `anet.c`) and wait a few seconds, or pick a different port if you modify the code.
- **No lint/test framework**: this is a minimal C library with no formal linter or automated test suite. Validation is done by building and running the examples.
