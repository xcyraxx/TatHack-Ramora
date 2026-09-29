# Ramora Engineering Changelog & Video Presentation Notes

This document tracks all architectural modifications, bug fixes, restored capabilities, and feature additions in **Ramora**. It is organized to serve as a reference and script outline for hackathon demo video recordings and technical evaluations.

---

## Part 1: Debugging Phase & Intentional Bug Fixes

### 1. Build & Toolchain Modernization (GCC 14 / C23)
* **Problem:** In modern GCC versions (e.g. GCC 14 on modern Linux distributions), passing a signal handler with signature `void ()` instead of `void (int)` triggers a fatal `-Wincompatible-pointer-types` error.
* **Files Modified:**
  - `src/server.c`: `void handle_sigint(int sig)`
  - `client/client.c`: `void handle_sigpipe(int sig)`
* **Video Talking Point:** *"Before touching application logic, we resolved modern C standard compliance issues to ensure clean, warning-free compilation on modern Linux distributions."*

---

### 2. TTL Seconds 1,000x Acceleration Bug
* **Problem:** In `src/oper.c`, `set_ttl_Entry_sec(key, ttl)` directly returned `set_ttl_Entry_ms(key, ttl)` without converting seconds into milliseconds. Setting a 30-second TTL caused the entry to be deleted after just 30 milliseconds.
* **Root Cause:**
  ```c
  // BEFORE:
  int set_ttl_Entry_sec(char* key, int64_t ttl){
      return set_ttl_Entry_ms(key, ttl); // Seconds passed as ms!
  }
  ```
* **Fix:** Converted `ttl` to milliseconds (`ttl * 1000`) while preserving special flag values (`ttl == -1` for removing expiration).
* **Video Talking Point:** *"A critical timing bug in `set_ttl_Entry_sec` omitted the second-to-millisecond conversion factor, causing keys to vanish 1,000 times faster than requested."*

---

### 3. Min-Heap Priority Queue Invariant Corruption
* **Problem:** The active TTL expiration mechanism relies on a binary min-heap (`struct Heap`). Both sift-up and sift-down routines contained structural flaws:
  1. `heap_up`: Loop condition compared parent against `arr[pos].expiration_time`. In iterations $2+$, `arr[pos]` was already overwritten by the shifted parent node, comparing the parent against itself rather than the floating `element`.
  2. `heap_down`: Multi-level sift-down advanced `pos` without placing `element`, comparing child nodes against stale slot data.
  3. `heap_delete`: Overwrote `heap->arr[pos]` with the tail element without updating `*heap->arr[pos].ref = pos`, breaking external index tracking if no subsequent swap occurred.
* **Fix in `src/heap.c`:**
  - Restored true binary min-heap invariants for both `heap_up` and `heap_down` by comparing child and parent states against `element.expiration_time`.
  - Maintained inverted references (`*ref = pos`) on all swaps and deletions.
* **Video Talking Point:** *"Active expiration depends on the min-heap root accurately reflecting the earliest expiring key. We repaired the sift-up and sift-down algorithms to prevent heap corruption under high load."*

---

### 4. Epoll Sleep Freeze & Unsigned Underflow in Event Loop
* **Problem:** In `src/conn.c:smallest_remaining_time()`:
  1. If `gd.idle_list` was empty (no connected clients), the function returned `-1`. This caused `epoll_wait` to block indefinitely with infinite timeout, completely halting TTL expirations if clients disconnected.
  2. Unsigned subtraction `min_key_expiration_time = expiration_time - time_now` wrapped around to `UINT64_MAX` whenever a key had already expired.
* **Fix in `src/conn.c`:**
  - Decoupled TTL heap timeout evaluation from client connection state. If clients disconnect, `epoll_wait` still wakes up precisely when the next key expires.
  - Eliminated unsigned arithmetic underflow.
* **Video Talking Point:** *"We fixed an architectural freeze where a cache with expiring keys would sleep forever if client connections dropped. The event loop now accurately wakes up for TTL deadlines regardless of connection state."*

---

### 5. Stale TTL Preservation on Key Overwrite
* **Problem:** When an existing key with an active TTL was updated via `SET`, `src/oper.c:set_Entry()` updated the value buffer but left the key's index in the TTL min-heap intact. When the original timer expired, it deleted the newly overwritten key.
* **Fix in `src/oper.c`:**
  - Upon updating an existing key without an explicit TTL, if `entry->heap_idx != (size_t)-1`, `heap_delete()` is called to remove the stale timer and `heap_idx` is reset to `-1`.
* **Video Talking Point:** *"A subtle persistence bug allowed stale TTL timers to linger after a key's value was updated, leading to unexpected data deletion. Overwrites now cleanly invalidate existing expiration timers."*

---

### 6. Key Prefix Collision (Identity Confusion)
* **Problem:** In `src/buf.c`, `bufstrcmp(buffer, __str, len)` only verified `bufsize(buffer) >= len` and compared `len` bytes. Consequently, a lookup for key `"test"` matched an existing key `"testing"`.
* **Fix in `src/buf.c`:**
  - Enforced strict length equality checks (`bufsize == len` or `bufsize == len + 1` with `\0`).
* **Video Talking Point:** *"We eliminated a prefix collision vulnerability where requesting a key could inadvertently access any longer key sharing that prefix."*

---

### 7. Protocol Response Code & Payload Formatting
* **Problem:** In `src/cmd_proc.c`:
  1. `handle_set_ttl_ms`: Error response for type mismatch called `response_code(conn->wbuf, 0)` instead of `response_u32(conn->wbuf, 0)`, violating the wire protocol framing.
  2. Duplicate `if (res == 0)` check prevented `res == -1` from returning `RES_ERR`.
  3. `src/oper.c`: Trailing null bytes were appended to value buffer lengths, causing binary responses to contain `\0` in payload lengths.
* **Fix:** Corrected protocol framing, restored `RES_ERR` handling, and standardized clean payload boundaries.

---

### 8. Restored Core Command: `DEL <key>`
* **Problem:** `DEL` was completely stripped out of the command dispatch table, breaking core CRUD functionality.
* **Fix:**
  - Implemented `handle_del(struct Conn* conn, size_t offset)` in `src/cmd_proc.c`.
  - Registered `{"DEL", 2, handle_del}` in `command_list[]` and updated `command_list_len = 8`.
  - Exported `handle_del` in `include/cmd_proc.h`.
* **Video Talking Point:** *"We identified the removed core command as `DEL`, re-wired it to the internal hash map and heap deletion engine, and restored complete CRUD capability."*

---

## Part 2: Verification & Benchmark Metrics

* **Functional Verification:**
  - `PING` $\to$ `PONG`
  - `SET` / `GET` / `DEL` verified with proper `RES_OK` and `RES_NX` status codes.
  - Active expiration verified for both seconds (`TTL`) and milliseconds (`TTLMS`).
  - Timer cancellation verified (`TTL <key> -1`).
  - Key prefix collision immunity verified.
* **High-Concurrency Benchmark (`ramora-test`):**
  - **Workload:** 10,000 operations, 4 concurrent clients, 4-deep request pipeline.
  - **Throughput:**
    - `SET`: **~352,000 ops/sec**
    - `GET`: **~290,000 ops/sec**
    - `DEL`: **~323,000 ops/sec**

---

## Part 3: Feature Extension — EXISTS, INCR, DECR

### 1. `EXISTS <key>`
* **Implementation:**
  - Added `exists_Entry(char* key)` in `src/oper.c` and `handle_exists` in `src/cmd_proc.c`.
  - Registered `{"EXISTS", 2, handle_exists}` in command dispatch.
  - Performs an $O(1)$ lookup in the active hash map.
  - Zero value-copying: does not allocate, read, or transfer the entry's value buffer.
  - Passive TTL expiration check: if the key has expired, it is immediately purged and reported as non-existent (`0`).
  - Returns `RES_OK` with integer `1` (exists) or `0` (does not exist).
* **Video Talking Point:** *"We added `EXISTS`, ensuring true $O(1)$ complexity without touching or transmitting the value payload. It also integrates passive expiration, guaranteeing expired keys are never falsely reported."*

---

### 2. `INCR <key>` & `DECR <key>` (Security & Systems Hardening)
* **Implementation:**
  - Added `incr_decr_Entry(char* key, int64_t delta, int64_t* out_val)` in `src/oper.c`.
  - Registered `{"INCR", 2, handle_incr}` and `{"DECR", 2, handle_decr}` in `command_list`.
  - **Adversarial Input Validation:**
    - Avoided simplistic `atoi()` or unchecked string parsing.
    - Used `strtoll()` with strict `errno` and `endptr` checking.
    - Explicitly detects empty values, non-numeric strings, and trailing characters (e.g. `"12abc"` or `"10 20"`), returning `RES_TY` (type error).
  - **Integer Overflow/Underflow Defense:**
    - Guaranteed safe 64-bit signed integer arithmetic.
    - Explicitly checks for boundary violations (`INT64_MAX` and `INT64_MIN`), returning `RES_ERR` instead of silently wrapping around.
  - **State Semantics:**
    - Non-existent keys initialize to `1` on `INCR` and `-1` on `DECR`.
    - Updates value buffer in place without resetting or wiping the key's existing TTL.
* **Video Talking Point:** *"For `INCR` and `DECR`, we applied a security-hardened parser using `strtoll` rather than naive conversion. We rigorously defend against type confusion, trailing garbage injection, and 64-bit integer overflow/underflow attacks."*

---

## Part 4: Feature Extension — AUTH & Brute-Force Protection

### 1. Config-Driven Authentication
* **Implementation:**
  - Added `requirepass <password>` directive to `ramora.conf` and `src/config.c`.
  - Configurable brute-force controls: `max_auth_failures` (default: 5) and `auth_lockout_ms` (default: 10,000 ms).
  - Clean backward compatibility: if `requirepass` is omitted or empty, all connections default to authenticated (`conn->authenticated = 1`).

---

### 2. Connection-Level Security Guard & Pipelining Defense
* **Implementation:**
  - Added `uint8_t authenticated;`, `uint32_t auth_failures;`, and `uint64_t lockout_until;` to `struct Conn` in `include/conn.h`.
  - In `src/server.c:handle_request()`, placed an authentication barrier before command dispatch:
    - If `requirepass` is enabled and `conn->authenticated == 0`, every command other than `AUTH` is immediately rejected with `RES_ERR` and string `"NOAUTH Authentication required"`.
  - **Pipelining Bypass Defense:** Because the authentication barrier is evaluated per command inside the request parsing loop, unauthenticated pipelined batches (e.g. `[AUTH badpass][GET secret]`) cannot bypass security; subsequent requests fail with `NOAUTH`.
  - **Connection Pooling Isolation:** When a pooled connection is recycled in `free_conn()`, its authentication state, failure counters, and lockout timers are explicitly wiped. New clients reusing pooled connections can never inherit prior authenticated sessions.
* **Video Talking Point:** *"We added per-connection authentication that protects all commands while preventing session leakage across Ramora's custom connection pool. Furthermore, we verified that pipelined command chains cannot bypass the authentication gate."*

---

### 3. Timing Side-Channel Attack Mitigation (`const_time_streq`)
* **Implementation:**
  - Standard `strcmp()` terminates on the first mismatched character, creating subtle response latency timing differences that allow adversaries to guess password characters byte-by-byte.
  - Implemented `const_time_streq()` in `src/cmd_proc.c`: performs an XOR comparison across the full buffer length regardless of early mismatches, neutralizing timing side-channels.
* **Video Talking Point:** *"Drawing from our VAPT background, we replaced naive `strcmp` password matching with a constant-time comparison routine (`const_time_streq`) to prevent side-channel timing attacks."*

---

### 4. Brute-Force Protection & Temporary Lockout
* **Implementation:**
  - Tracked consecutive failed authentication attempts (`conn->auth_failures`).
  - Upon reaching `MAX_AUTH_FAILURES`, the connection is placed into a temporary lockout state until `conn->lockout_until = now + AUTH_LOCKOUT_MS`.
  - Any `AUTH` attempt during lockout is immediately rejected with `"ERR Temporary lockout due to repeated failed authentication attempts"`.
  - Successfully authenticating clears failure counters and resets lockout state.
  - Telemetry recorded in global state: `stat_auth_success`, `stat_auth_failures`, and `stat_auth_lockouts` for reporting in `INFO / STATS`.
* **Video Talking Point:** *"To protect against automated dictionary attacks and password spraying, Ramora tracks per-connection failures and enforces a temporary lockout once the threshold is crossed, recording telemetry for security auditing."*

---

## Part 5: Feature Extension — Persistence & Crash-Resilient Snapshots

### 1. Robust Binary Snapshot Format with Checksumming
* **Implementation:**
  - Designed zero-dependency binary snapshot format in `include/persist.h` and `src/persist.c`:
    - **Header:** 4-byte magic signature (`"RMRA"`), uint32 version (`1`), and uint32 entry count.
    - **Entries:** `[uint32 klen][key][uint32 vlen][val][int64 remaining_ttl_ms]`.
    - **Trailer:** 4-byte footer magic (`"ENDR"`), followed by a 64-bit FNV-1a checksum computed incrementally across all data bytes.
  - **Corruption Detection:** Any file truncation, bit rot, or payload tampering fails the checksum or footer verification, preventing corrupted snapshots from poisoning the database.
* **Video Talking Point:** *"We created a lightweight binary persistence engine featuring magic headers and 64-bit checksum verification, guaranteeing full data integrity and corrupt file detection on startup."*

---

### 2. Relative Remaining TTL Preservation
* **Implementation:**
  - Problem Statement requirement: *"Do NOT reset an expiring key's TTL to its original TTL after restart. A 30s key saved after 5s should have ~25s remaining, not a fresh 30s."*
  - Handled by calculating relative remaining time `exp_time - now_ms` at save time.
  - On startup load, expiration is scheduled at `start_now_ms + remaining_ttl_ms`.
  - Keys already expired at save time are filtered out and discarded.
* **Video Talking Point:** *"Unlike naive implementations that reset TTLs to their initial values, Ramora computes and stores exact relative remaining lifespans. Restored keys continue counting down from their remaining time rather than resetting."*

---

### 3. Crash-Resilient Atomic Writes & Signal Handling
* **Implementation:**
  - Avoided writing directly into the live snapshot file.
  - Snapshots are written to a temporary file (`.tmp`), flushed to OS buffers (`fflush`), committed to disk (`fsync`), and atomically swapped into place via `rename()`.
  - A crash or power loss during snapshot creation leaves the prior valid snapshot untouched.
  - Handled both `SIGINT` (Ctrl+C) and `SIGTERM` (systemd/Docker shutdown) to trigger automatic snapshot saving before freeing memory.
  - Configurable via `snapshot_file <path>` directive in `ramora.conf` (default: `ramora.snapshot`).
* **Video Talking Point:** *"To guarantee durability, snapshots are written atomically using `fsync` and POSIX atomic renaming, protecting against partial writes during sudden power loss. Both `SIGINT` and `SIGTERM` are hooked for graceful shutdown persistence."*

---

## Part 6: Feature Extension — INFO & STATS Telemetry

### 1. Redis-Style Introspection Engine
* **Implementation:**
  - Added `handle_info` in `src/cmd_proc.c` and registered both `INFO` and `STATS` (alias) in `command_list`.
  - Exported structured sectioned telemetry formatted with CRLF line endings:
    - **Server:** Version (`0.2.0`), PID, uptime in seconds, uptime in days (calculated from monotonic clock `get_monotonic_msec()`).
    - **Clients:** Real-time count of connected TCP clients, active pooled connections in Ramora's slab-allocated pool, and pool capacity.
    - **Memory:** Process Resident Set Size (RSS) queried dynamically via POSIX `getrusage(RUSAGE_SELF)`, formatted in exact bytes and human-readable megabytes.
    - **Stats:** Total processed commands counter, live active keyspace count (accounting for progressive rehash across `new_tab` and `old_tab`), expiring keys count (from the min-heap), cache hits, cache misses, and dynamic percentage hit rate.
    - **Security:** Real-time auth status, total successful authentications, failed attempts, trigger count of brute-force lockouts, and active lockout policy parameters.
* **Video Talking Point:** *"We implemented `INFO` and `STATS` commands giving Redis-style observability. Administrators can monitor real-time cache hit rates, memory footprint via `getrusage`, connected clients, and live cybersecurity telemetry like brute-force lockout triggers."*

---

## Part 7: Feature Extension — KEYS <pattern> Glob Pattern Matching

### 1. POSIX Glob-Compliant Keyspace Scanning
* **Implementation:**
  - Implemented `get_keys_Entry(const char* pattern, struct buf* wbuf)` in `src/oper.c` and `handle_keys` in `src/cmd_proc.c`.
  - Registered `{"KEYS", 2, handle_keys}` in `command_list` (total `command_list_len = 16`).
  - **Pattern Matching Support:**
    - Standard wildcard globbing via POSIX `fnmatch(pattern, key, 0)`.
    - Fast path for universal match `KEYS *`.
    - Supports prefix matching (e.g. `user:*`), suffix matching (`*.log`), single-character wildcards (e.g. `user:100?`), character ranges (`[a-z]`), and exact keys.
  - **Two-Pass Allocation-Free Streaming:**
    - Single-threaded deterministic two-pass scan across `new_tab` and `old_tab` (under progressive rehash).
    - Pass 1 computes matching count `count` without heap allocations.
    - Pass 2 streams keys directly into connection write buffer `wbuf`.
  - **TTL Expiration Filtering:**
    - Keys whose monotonic expiration timestamp $\le$ current timestamp are silently skipped, preventing expired keys from leaking into query responses.
  - **Client Compatibility:**
    - Integrated with `ramora-client` wire framing and extended client response parser with `TAG_ARR` resilience.
* **Video Talking Point:** *"We added `KEYS <pattern>` supporting full globbing wildcards via POSIX `fnmatch`. Using a two-pass scan, Ramora computes response counts and streams results without secondary memory allocations, while filtering out expired keys."*

---

## Part 8: Feature Extension — SCAN Cursor-Based Keyspace Iteration

### 1. Incremental, Non-Blocking Keyspace Traversal
* **Problem Solved:** `KEYS *` blocks the single-threaded event loop when iterating large databases. Production systems require non-blocking, cursor-based pagination.
* **Command Syntax:**
  - `SCAN <cursor> [MATCH pattern] [COUNT count]`
  - Also supports positional shorthand `SCAN <cursor> <pattern> <count>`.
* **Implementation Details:**
  - **Bucket-Level Cursor Mechanics:** Cursor maps to hash map bucket indices $b \in [0, \text{cap}-1]$.
  - **Rehash Consistency:** Transparently queries both `new_tab` and `old_tab` (accounting for active `migrating_pos`) to guarantee no missed keys or duplicates during progressive table expansion.
  - **Sparse Bucket Optimization:** Automatically scans up to 512 buckets per iteration to efficiently bypass sparse/empty regions without starving the client or event loop.
  - **TTL-Aware Filtering:** Keys past their monotonic expiration deadline are omitted from scan outputs.
  - **Completion Contract:** When the cursor wraps to `"0"`, the complete keyspace has been inspected.
* **Video Talking Point:** *"Unlike `KEYS` which can block on large datasets, `SCAN` provides an incremental, non-blocking cursor across hash table buckets. It handles progressive rehashing, respects `MATCH` patterns and `COUNT` hints, and skips expired keys without stalling the single-threaded event loop."*

---

## Part 9: Feature Extension — maxmemory Enforcement & Eviction Policies

### 1. Deterministic In-Memory Tracking & Configurable Eviction
* **Config Directives:**
  - `maxmemory <bytes>` — Configurable memory cap (supports raw bytes and suffixes like `10mb`, `100kb`, `1gb`). Set to `0` for unlimited.
  - `maxmemory_policy <policy>` — Eviction algorithm to apply when limit is exceeded.
* **Supported Eviction Policies:**
  - `volatile-ttl` — Evicts keys with an explicit TTL, prioritizing shortest time-to-live first directly from the min-heap root in $O(\log N)$ time.
  - `allkeys-lru` — Evicts least recently used keys using Redis-style sampled approximated LRU (sampling 5 candidate keys per round based on monotonic `last_accessed` timestamps in `struct Entry`).
  - `allkeys-random` — Evicts uniformly sampled random keys across active hash table buckets.
  - `noeviction` — Strictly enforces memory bounds by rejecting write commands (`SET`, `INCR`, `DECR`) with standard Redis-compliant error: `OOM command not allowed when used memory > 'maxmemory'`. Read operations (`GET`, `EXISTS`, `PING`, `INFO`) remain fully functional.
* **Byte-Accurate Telemetry:**
  - Dynamic keyspace memory accounting tracking exact allocation overhead (`sizeof(struct Entry) + bufcap(key) + bufcap(val)`).
  - Integrated into `INFO` and `STATS`:
    - `used_memory` (bytes) and `used_memory_human` (megabytes)
    - `used_memory_rss_bytes` and `used_memory_rss_human` (from OS `getrusage`)
    - `maxmemory` and `maxmemory_policy`
    - `evicted_keys` cumulative counter
* **Video Talking Point:** *"We added production-grade `maxmemory` management with four eviction policies: `volatile-ttl` using our min-heap, sampled approximated `allkeys-lru`, `allkeys-random`, and strict `noeviction` with OOM error responses. Memory consumption is tracked with byte-level precision alongside live `evicted_keys` telemetry in `INFO`."*

---

## Part 10: Usability & Demo Enhancements

### 1. Redis-Compliant SET with Expiration (`EX` / `PX`)
* Extended `SET` command to handle standard 5-argument format:
  - `SET <key> <val> EX <seconds>`
  - `SET <key> <val> PX <milliseconds>`
* Provides seamless interoperability with standard Redis clients and CLI workflows during evaluations.

### 2. Configuration & Network Harmonization
* Configured `ramora.conf` to bind default port `5000`, matching `ramora-client`'s default port.
* Relocated snapshot and logging configurations for seamless non-root, out-of-the-box evaluation.

---

## Part 11: Video Presentation Script (7 Minutes)

**[0:00-1:00] Introduction & Architecture overview**
* "Hello! I'm here to present my solution for the Ramora hackathon."
* "Ramora is a high-performance, single-threaded, in-memory key-value cache built in C. It uses a custom wire protocol, an epoll-based event loop, a custom hash map with progressive rehashing, and a min-heap for TTL expirations."
* "My approach focused heavily on building a robust, production-ready system with a strong emphasis on security and memory safety, drawing from my background in red teaming and VAPT."

**[1:00-2:30] Fixing Intentional Bugs**
* "The first challenge was fixing several intentional bugs left in the initial code."
* "First, I restored the core `DEL` command which was completely missing from the dispatch table."
* "Second, there was a critical timing bug where TTLs given in seconds were treated as milliseconds, causing keys to expire 1,000 times too fast. I fixed the conversion logic."
* "Third, the TTL min-heap had broken invariants in its sift-up and sift-down routines, corrupting the queue under load. I repaired these algorithms to maintain proper heap structure."
* "Another major issue was in the event loop: if no clients were connected, `epoll_wait` would sleep indefinitely, completely halting TTL expirations. I decoupled the TTL timeout calculation from the client connection list."
* "Finally, I fixed protocol response framing errors and a subtle persistence bug where overwriting a key failed to remove its old TTL timer."

**[2:30-4:00] Feature Implementation (SCAN, Persistence, Maxmemory)**
* "After stabilizing the core, I implemented several advanced features."
* "I implemented `SCAN` for cursor-based keyspace iteration. Unlike `KEYS`, which blocks the event loop on large datasets, `SCAN` allows incremental traversal. It smartly handles progressive rehashing by scanning both old and new tables simultaneously, and includes optimizations to skip empty buckets quickly."
* "I built a robust binary persistence engine. It writes snapshots to a temporary file, uses `fsync`, and atomically renames the file to prevent corruption during a crash or power loss. It also uses a 64-bit checksum for integrity verification and stores relative remaining TTLs, not absolute ones."
* "I also implemented exact byte-level tracking and `maxmemory` enforcement with four eviction policies: `volatile-ttl`, an approximated sampled `allkeys-lru`, `allkeys-random`, and strict `noeviction`."

**[4:00-5:30] Security & System-Level Hardening**
* "Given my security background, I proactively hardened the system."
* "I added per-connection authentication with a configuration-driven password. To prevent brute-forcing, I implemented a temporary lockout mechanism after multiple failed attempts."
* "To defend against timing side-channel attacks, I replaced standard `strcmp` password matching with a constant-time XOR comparison (`const_time_streq`)."
* "I also ensured the authentication barrier prevents pipelined command chain bypasses and that pooled connections are fully wiped of their authenticated state when recycled."
* "For `INCR` and `DECR`, I wrote a strict parser using `strtoll` that defends against type confusion, trailing garbage, and 64-bit integer overflow/underflow attacks."
* "Finally, I resolved a prefix collision bug in buffer matching where requesting a short key could accidentally return a longer key that shared the same prefix."

**[5:30-7:00] Live Demonstration**
* *(Start the Ramora server in the background and connect via client)*
* "Let's see it in action."
* *(Demo: AUTH)* "I'll try to execute a command without authenticating. It fails. Now I'll enter the wrong password a few times to trigger the brute-force lockout. Finally, I'll log in successfully."
* *(Demo: SET EX/PX and TTL)* "I'll set a key with a 10-second expiration. We can verify the TTL counts down correctly in real-time."
* *(Demo: SCAN & INFO)* "I'll insert a few keys and run `SCAN 0` to demonstrate cursor-based iteration. We can also run `INFO` to view real-time telemetry: connected clients, used memory bytes from the OS, eviction counts, and security statistics."
* *(Demo: Persistence)* "I'll send a `SAVE` command. The binary snapshot is atomically saved to disk with a checksum. If I restart the server, the state and the exact remaining TTLs are flawlessly recovered."
* "Thank you for watching. Ramora is now a robust, hardened, and feature-complete cache."
