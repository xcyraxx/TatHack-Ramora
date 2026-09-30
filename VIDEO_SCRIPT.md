# Ramora Hackathon Presentation — Complete Spoken Video Script

> **Target Duration:** 6:00 – 6:45 (Well within the 7:00 minute maximum)  
> **Format:** Screen recording with voiceover (No slides needed; show terminal and code)  
> **Tone:** Confident, technical, clear, systems-focused

---

## [0:00 – 0:45] SECTION 1: Introduction & Architecture Overview

**[SCREEN ACTION]**  
*Show your terminal in the `Ramora` root directory. Run `tree src include` or show `README.md`.*

**SPOKEN SCRIPT:**  
"Hi everyone, I'm Adil, and this is my presentation of the Ramora project for TatHack Tathva.

Ramora is a high-performance, single-threaded in-memory caching engine written from scratch in pure C. The design philosophy behind it is zero external dependencies and total systems-level control. 

Under the hood, its architecture centers on:
1. An event-driven I/O loop built directly on Linux `epoll` using non-blocking sockets.
2. An in-memory hash table that features progressive rehashing across dual buckets to avoid blocking the event loop on table expansion.
3. A binary min-heap dedicated to active, time-priority TTL key expiration.
4. And custom dynamic buffers that handle partial reads and writes without making assumptions about TCP packet boundaries.

My goal in this hackathon wasn't just to make it pass basic tests, but to transform it into a hardened, production-grade engine with persistence, memory eviction, and defensive security controls."

---

## [0:45 – 2:15] SECTION 2: The Original Bugs & How I Solved Them

**[SCREEN ACTION]**  
*Open VS Code / editor to `src/oper.c` and `src/heap.c`, highlighting the lines where the bugs were.*

**SPOKEN SCRIPT:**  
"When I first inspected the codebase, there were several intentional and structural bugs that crippled the engine:

**First, the Missing Command:**  
The `DEL` command was completely missing from the command dispatch table in `src/cmd_proc.c`. Core CRUD functionality was broken. I implemented `handle_del`, wired it up to both the hash map and the heap deletion routine, and registered it in the dispatch array.

**Second, the 1,000x TTL Acceleration Bug:**  
In `src/oper.c`, the function `set_ttl_Entry_sec` was passing its seconds argument directly to `set_ttl_Entry_ms` without multiplying by 1,000. So if you set a 30-second TTL, the key was destroyed in 30 milliseconds. I corrected the scaling factor while preserving `-1` as the sentinel for removing an expiration timer.

**Third, the Min-Heap Corruption:**  
Active expiration relies on a binary min-heap in `src/heap.c`. Both sift-up and sift-down routines were structurally broken. During multi-level sift operations, the loop compared parent elements against already-overwritten array slots rather than the floating element being inserted. Furthermore, deletions failed to update the back-references, causing heap corruption under load. I rewrote the sift invariants and ensured every node swap updates its external index pointer.

**Fourth, the Event Loop Freeze:**  
In `src/conn.c`, the timeout calculation for `epoll_wait` returned `-1` if no clients were connected. That meant if all clients disconnected, the server would sleep indefinitely and never wake up to expire keys! I decoupled the TTL heap deadline from the client list so `epoll_wait` always wakes up precisely when the next key expires, regardless of whether any connection is active."

---

## [02:15 – 03:30] SECTION 3: System-Level Challenges & Subtle Edge Cases

**[SCREEN ACTION]**  
*Show `src/buf.c` around `bufstrcmp` and `src/oper.c` around `set_Entry`.*

**SPOKEN SCRIPT:**  
"Beyond the obvious logical bugs, I hunted down subtle system-level issues that could cause silent failures in production:

**1. The Key Prefix Collision Bug:**  
In `src/buf.c`, the string comparison helper `bufstrcmp` only checked if the stored buffer length was greater than or equal to the query length, and only compared that many bytes. This meant a query for the key `'admin'` would match `'administrator'`! That's a serious data corruption and security vulnerability. I enforced strict length equality checking.

**2. Stale TTL Preservation on Key Overwrite:**  
If a key had an active TTL timer and you subsequently ran a standard `SET` to overwrite its value, the old code updated the value buffer but left the key's index in the min-heap untouched. When the original timer expired, the newly updated key was deleted without warning! I modified `set_Entry` so that any value update cleanly removes any prior heap timer.

**3. Toolchain & Protocol Compliance:**  
On modern GCC with `-Werror`, signal handlers with outdated signatures failed compilation. I modernized the prototypes to ISO C standards. In addition, I fixed protocol wire framing where `handle_set_ttl_ms` sent raw 32-bit integers without proper type tags."

---

## [03:30 – 04:45] SECTION 4: Architecture Extensions — SCAN, Persistence & Maxmemory

**[SCREEN ACTION]**  
*Show `src/persist.c` and `src/oper.c` (`scan_Entry` and `evict_if_needed`).*

**SPOKEN SCRIPT:**  
"Next, I implemented major production features:

**1. Non-Blocking SCAN:**  
While `KEYS *` is fine for tiny tests, running it against millions of keys blocks a single-threaded server. I implemented `SCAN <cursor> [MATCH pattern] [COUNT count]`. The cursor maps directly to hash table bucket indices. Because Ramora uses progressive rehashing, my `SCAN` algorithm seamlessly inspects both `new_tab` and `old_tab` simultaneously, and includes a fast-path optimization that skips up to 512 sparse buckets per cycle so the client never starves.

**2. Crash-Resilient Binary Persistence:**  
I built a custom binary snapshot engine in `src/persist.c`. It writes to a temporary file, calls `fsync` to flush OS buffers to physical disk, and then atomically swaps it into place using `rename`. This prevents partial writes if the server crashes or loses power. It also uses a 64-bit checksum for integrity validation, and hooks `SIGINT` and `SIGTERM` for graceful persistence. Crucially, it records *relative remaining TTL*, so if a 30-second key is saved after 5 seconds, it restarts with 25 seconds remaining, not a fresh 30.

**3. Maxmemory & 4 Eviction Policies:**  
I implemented byte-accurate keyspace memory tracking and added four eviction policies:
- `volatile-ttl`: evicts expiring keys in $O(\\log N)$ time directly from the min-heap.
- `allkeys-lru`: uses an approximated sampled LRU based on monotonic access timestamps.
- `allkeys-random`: evicts random keys across active buckets.
- `noeviction`: returns an explicit Redis-compliant OOM error on write."

---

## [04:45 – 05:30] SECTION 5: Security Engineering & Defensive Hardening

**[SCREEN ACTION]**  
*Show `src/cmd_proc.c` around `const_time_streq` and connection lockout logic.*

**SPOKEN SCRIPT:**  
"Coming from a cybersecurity and VAPT background, I prioritized defensive hardening:

**Constant-Time Authentication:**  
Standard `strcmp` terminates early on the first mismatched byte, exposing the server to timing side-channel attacks where an attacker measures response latency to guess password characters. I replaced it with `const_time_streq`, an XOR-based comparison that always executes in constant time regardless of where a mismatch occurs.

**Brute-Force Lockout & Pipelining Defense:**  
I added per-connection failure counters. If a client fails authentication repeatedly, they are placed in a temporary lockout state. I also ensured that unauthenticated clients cannot pipeline commands behind a bad `AUTH` request to bypass verification, and connection pool reuse completely scrubs security state between clients.

**Adversarial Input Validation for INCR/DECR:**  
Instead of naive `atoi()`, I implemented strict parsing with `strtoll` that checks for trailing garbage characters, detects type mismatches (`RES_TY`), and prevents 64-bit integer overflow and underflow attacks."

---

## [05:30 – 06:45] SECTION 6: Live Terminal Demonstration

**[SCREEN ACTION]**  
*Split screen: Server running on the left, client on the right.*

**SPOKEN SCRIPT:**  
"Now let's see everything working live in the terminal.

*(Start the server)*  
`./ramora-server`  
Notice it automatically detects and loads `conf/ramora.conf` with our configured password and settings.

*(In the client terminal, connect)*  
`./ramora-client`

*(1. Test Auth gate)*  
Let's try to run `PING` or `SET foo bar` without logging in:  
`PING`  
-> It immediately returns `ERR NOAUTH Authentication required`.

Let's test our brute-force protection with bad passwords:  
`AUTH wrong1`  
`AUTH wrong2`  
`AUTH wrong3`  
`AUTH wrong4`  
`AUTH wrong5`  
`AUTH wrong6`  
-> Now the server returns `ERR Temporary lockout due to repeated failed authentication attempts`.

Let's authenticate with the correct password:  
`AUTH Pass`  
-> `RES_OK: OK`. We are authenticated.

*(2. Test Expiration with SET EX)*  
Let's set a key with a 5-second TTL:  
`SET tempkey hello EX 5`  
Let's check TTL:  
`TTL tempkey`  
-> It counts down: 4, 3, 2, 1... and now `GET tempkey` returns `RES_NX (nil)`. Expiration works perfectly.

*(3. Test SCAN & Telemetry)*  
Let's add a few keys and test cursor-based traversal:  
`SET user:1 alice`  
`SET user:2 bob`  
`SCAN 0 MATCH user:* COUNT 10`  
-> We get our cursor `'0'` and our matching keys without blocking.

Now let's check our Redis-style observability telemetry:  
`INFO`  
-> Look at that: real-time memory usage in bytes, OS RSS queried directly via `getrusage`, active keys, cache hit rate, and security statistics including successful logins and lockout counts.

*(4. Test Persistence)*  
Finally, let's run `SAVE`:  
`SAVE`  
-> Snapshot written atomically. Let's stop the server with `Ctrl+C` and restart it.  
Now reconnect:  
`AUTH Pass`  
`GET user:1`  
-> `alice`! Data is fully preserved across restarts."

---

## [06:45 – 07:00] SECTION 7: Conclusion & Wrap-Up

**[SCREEN ACTION]**  
*Show the GitHub repository `xcyraxx/TatHack-Ramora` in browser or terminal.*

**SPOKEN SCRIPT:**  
"To wrap up: Ramora has been transformed from a buggy baseline into a hardened, production-ready key-value store. It passes our full 95-test QA suite with zero compiler warnings and zero memory leaks.

All code, full commit history, and technical documentation are available on my public GitHub repository at `xcyraxx/TatHack-Ramora`. 

Thank you for your time and consideration!"
