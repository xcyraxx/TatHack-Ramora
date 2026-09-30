# Ramora

Ramora is a high-performance, single-threaded in-memory caching engine written in C.

It is designed to be fast, predictable, and resource-efficient, with explicit control over memory, I/O, and event handling.

Ramora supports basic key-value operations, pipelined requests, TTL-based expiration, and connection lifecycle management — all built from scratch without external dependencies. 

> **Hackathon Edition**: This version includes critical bug fixes and massive feature expansions (Persistence, `SCAN`, Security Hardening, Eviction Policies) built for a 24-hour hackathon! See `CHANGELOG.md` for full implementation details and the video presentation script.

## Features

### High-performance event loop
- epoll-based I/O
- Fully non-blocking socket handling
- Explicit read/write state tracking

### In-memory key-value store
- Supports standard operations: `SET`, `GET`, `DEL`, `EXISTS`, `INCR`, `DECR`
- Keyspace querying via `KEYS <pattern>` (POSIX globbing) and non-blocking `SCAN <cursor>`
- Predictable memory usage with precise byte-level tracking

### TTL & Expiration
- Supports per-key TTL (`EX` seconds & `PX` milliseconds natively in `SET`)
- Active expiration using a min-heap
- Immediate key removal upon expiry (not lazy)

### 🔒 Security & Authentication (New!)
- Config-driven password authentication (`requirepass`)
- Constant-time password verification to prevent timing side-channel attacks
- Built-in brute-force protection with temporary connection lockouts
- Adversarial input validation for integers (`INCR`/`DECR`) to prevent overflows

### 💾 Persistence Engine (New!)
- Crash-resilient binary snapshots with atomic file swapping (`fsync` + `rename`)
- 64-bit checksum integrity verification to prevent silent corruption
- Preserves accurate *relative remaining TTL* across server restarts
- Automatic snapshot on `SIGINT` / `SIGTERM`

### 🧠 Maxmemory & Eviction (New!)
- Configurable maximum memory cap (`maxmemory`)
- Four eviction policies: `volatile-ttl`, `allkeys-lru` (sampled approximated LRU), `allkeys-random`, and strict `noeviction`

### 📊 Telemetry & Observability (New!)
- Redis-compliant `INFO` / `STATS` commands
- Real-time tracking of memory RSS (`getrusage`), cache hit rate, eviction counts, and security lockouts

### Connection management
- Idle connection tracking using doubly-linked lists
- Connection object pooling (bounded) (custom built reusing mechanism)
- Automatic cleanup of inactive connections

### Pipelining support
- High throughput under batch workloads
- Benchmarked up to millions of ops/sec on a single core

### Memory safety
- Zero memory leaks (verified via Valgrind)
- Explicit buffer management
- Bounded object pools to cap memory usage

## Architecture Overview

Ramora is intentionally built with explicit systems-level control:

- Event loop: epoll
- Networking: TCP, non-blocking sockets
- Concurrency model: single-threaded, single-core

### Data structures:
- Hash table for key storage (Uses progressive rehashing to cap worst case latency)
- Min-heap for TTL management
- Doubly-linked lists for idle connections and for lazyfreeing connection objects.

### Buffers:
- Custom dynamic read/write buffers
- Partial read/write handling
- No assumptions about TCP packet boundaries

## Performance

Ramora is benchmarked using a custom client with configurable pipeline width, clients, number of operations, and predecided universal set of keys.

The testing report of Ramora with Redis is present in `tests/testing_report.txt`

Report have showed that Ramora is `12%` faster then Redis.

## Configuration

Ramora is configured via a server-side config file `conf/ramora.conf`, allowing control over:

- Bind address & Port
- `requirepass` (Authentication)
- `maxmemory` & `maxmemory_policy` (Eviction)
- `snapshot_file` (Persistence)
- Initial hashmap / heap / buffer capacity
- Maximum events that can be processed by one epoll cycle

## Building

### To build binaries

Run the below command in the root directory of this project:
```bash
make
```

This will create three binaries:
- `ramora-server`
- `ramora-client`
- `ramora-test`

### To clean up

```bash
make clean
```

## Usage

### Server

To start the server using the default configuration (`conf/ramora.conf` is loaded automatically):
```bash
./ramora-server
```

To run with a custom config file:
```bash
./ramora-server path/to/custom.conf
```

### Client

```bash
./ramora-client
```
This connects to `127.0.0.1:5000`.

To connect to a different host/port:
```bash
./ramora-client -h <ip> -p <port>
```

### Test

```bash
./ramora-test
```
For help and options:
```bash
./ramora-test -h
```

## Author

### Dipanshu Tiwari & xcyraxx (Hackathon Submission)
