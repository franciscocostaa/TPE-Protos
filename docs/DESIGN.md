# Design Document — SOCKS5 Proxy + Management Protocol

> ITBA · Protocolos de Comunicación · TPE 2026/1
> Status: **DRAFT for review** — no implementation code committed against this yet.

This document is the architectural blueprint we agree on *before* writing logic. It is
intentionally implementation-language-precise (C11) for the proxy, and RFC-style /
language-agnostic for the management protocol we design ourselves.

---

## 1. Scope & decisions

**In scope (first entrega):**

- SOCKS5 proxy (RFC 1928) — **CONNECT command only**.
- Username/password authentication (RFC 1929) **and** the "no authentication" method,
  selected by configuration.
- Outbound TCP to **IPv4, IPv6, and FQDN** (resolved to either family).
- Robust multi-address fallback on connect failure.
- Full SOCKS reply codes on success/failure.
- Volatile metrics + access log.
- A **separate management/monitoring protocol** on its own port + a terminal client.
- Graceful shutdown (SIGTERM/SIGINT).

**Explicitly out of scope:**

- **BIND** and **UDP ASSOCIATE** → server replies `REP = X'07'` (command not supported).
  The design keeps the request layer open to adding them later, but we do not build them.
- GSSAPI authentication (not required by the consigna).
- POP3 credential sniffing → **second entrega only**, designed for but not built now.

**Key non-functional constraints that drive the architecture:**

- **Single thread**, **non-blocking** sockets, **multiplexed** via one selector.
  The *only* permitted extra thread is DNS resolution (`getaddrinfo`), which performs no
  other I/O and only hands results back to the main thread.
- **Partial read / partial write correctness is pass/fail.** Every byte path must assume
  `read`/`write` move *fewer* bytes than requested and resume later.
- Bounded memory: never load a whole stream into RAM; fixed-size relay buffers.
- C11 (`-std=c11`), builds with `make`, POSIX 1003.1 CLI conventions.

---

## 2. Repository & build layout

We keep the existing `server / client / shared` split and grow it. Proposed tree:

```
src/
  server/        # SOCKS5 proxy + management server (single process)
    main.c               # arg parsing, signals, listeners, selector loop bootstrap
    socks5/
      socks5.c/.h        # per-connection state machine (the heart)
      negotiation.c/.h   # method negotiation parser (RFC 1928 §3)
      auth.c/.h          # user/pass sub-negotiation parser (RFC 1929)
      request.c/.h       # request parser (RFC 1928 §4) + reply builder
      connect.c/.h       # outbound connect + address-list fallback
      relay.c/.h         # bidirectional copy (the two "halves")
    mgmt/
      mgmt.c/.h          # management protocol state machine
    dns/
      resolver.c/.h      # async getaddrinfo worker + result delivery
    metrics.c/.h         # counters (historical/concurrent conns, bytes)
    access_log.c/.h      # "who connected where and when"
    users.c/.h           # user store (add/remove/auth), runtime-mutable
    config.c/.h          # runtime config (timeouts, buffer sizes, auth on/off)
  client/        # management terminal client (blocking I/O is fine here)
    main.c
  shared/        # code shared by server & client
    selector.c/.h        # non-blocking event loop abstraction
    buffer.c/.h          # partial-read/write-safe byte buffer
    stm.c/.h             # generic state-machine driver
    netutils.c/.h        # sockaddr helpers, sock_blocking_*(), etc.
    args.c/.h            # POSIX-style CLI parsing (reference impl when published)
docs/
  DESIGN.md      # this file
  PROTOCOL.md    # RFC-style spec of our management protocol (separate deliverable)
```

**Build note (action item):** `Makefile.inc` currently has
`COMPILER_FLAGS=-Wall -pedantic -g`. The consigna mandates C11, so we must add
`-std=c11` and a feature-test macro (e.g. `-D_POSIX_C_SOURCE=200809L`) for
`getaddrinfo`, `sigaction`, etc. We'll also want `-Wextra` and a `-fsanitize=address`
debug profile for development (not the graded build).

**On third-party code:** the cátedra publishes course material (notably a non-blocking
`selector` + `buffer` + `stm`). Reusing it is *permitted with attribution*. We will
either use the published versions verbatim (attributed) or write our own to the same
shape — to be confirmed before coding. Either way the **SOCKS5 logic is ours**.

---

## 3. Core architecture — one thread, one selector

Everything is one event loop. There are **no blocking calls** on the data path. The
loop owns a set of file descriptors, each with an interest set (read/write) and a
handler. Pseudocode:

```
selector = selector_new()
register(socks_listen_fd,  READ, accept_socks)
register(mgmt_listen_fd,   READ, accept_mgmt)
register(dns_notify_fd,    READ, on_dns_result)   # self-pipe / eventfd
while running:
    selector_select(selector, timeout)   # the ONE place we block, with a timeout
    for each ready fd: dispatch its handler
    run expired timeouts (idle connection reaping)
```

The selector is backed by `select(2)` for portability (the consigna targets a POSIX
environment; `select` is fine for the required 500 connections, and the course selector
uses it). If profiling demands it we can swap the backend behind the same interface.

**Why a self-pipe / `eventfd` for DNS:** the DNS worker thread cannot touch our sockets.
When it finishes `getaddrinfo`, it writes one byte to a pipe that the selector watches;
the main thread wakes, reads the result from a queue, and resumes that connection's
state machine. This is the single, carefully-bounded use of threading.

---

## 4. The buffer abstraction (partial I/O correctness)

A `buffer` is a fixed byte array with `read`/`write` pointers, exposing:

```
buffer_write_ptr(b, &n)  -> where to recv() into, and how much room (n)
buffer_write_adv(b, k)   -> we actually received k bytes
buffer_read_ptr(b, &n)   -> where to send() from, and how much is pending (n)
buffer_read_adv(b, k)    -> we actually sent k bytes
buffer_can_read/write()  -> predicates
buffer_compact()         -> reclaim consumed space
```

Every parser consumes from a buffer and **must tolerate running out of bytes mid-message**
— it returns "need more data" and is re-entered when the next chunk arrives. Every writer
**must tolerate a short `send`** — it advances the read pointer by the bytes actually
written and re-arms `WRITE` interest for the rest. This is the mechanism that makes
partial I/O correct *by construction* rather than by ad-hoc patching.

---

## 5. SOCKS5 connection state machine

Each accepted client connection is one state machine instance (driven by `stm`). States,
mapping directly to RFC 1928 / 1929:

```
                       (read)            (read/write)
NEGOTIATION_READ ───▶ NEGOTIATION_WRITE ───▶ AUTH_READ ───▶ AUTH_WRITE
   |  RFC1928 §3 method list      reply X'05',method        RFC1929 sub-neg
   |                                                              |
   ▼                                                              ▼
REQUEST_READ ───▶ RESOLVING ───▶ CONNECTING ───▶ REQUEST_WRITE ───▶ RELAY
  RFC1928 §4    async DNS      try addr list,    reply REP,...      copy both
  parse req     (if FQDN)      fallback on fail   bind addr         directions
                                                                      |
                                                                      ▼
                                                                    DONE / ERROR
```

- **NEGOTIATION:** read `VER, NMETHODS, METHODS`; pick `NO_AUTH (0x00)` or
  `USER_PASS (0x02)` per config; reply `VER=0x05, METHOD`. If none acceptable → `0xFF`
  and close.
- **AUTH (only if USER_PASS chosen):** read `VER=0x01, ULEN, UNAME, PLEN, PASSWD`;
  validate against the user store; reply `VER=0x01, STATUS` (`0x00` ok, else close).
- **REQUEST:** read `VER, CMD, RSV, ATYP, DST.ADDR, DST.PORT`. If `CMD != CONNECT` →
  reply `REP=0x07`. Branch on `ATYP`: IPv4 / IPv6 → connect directly; DOMAINNAME →
  enter `RESOLVING`.
- **RESOLVING:** hand the FQDN to the DNS worker; park the connection (no fd interest)
  until the self-pipe wakes us with an addrinfo list.
- **CONNECTING:** non-blocking `connect()` to the first address; on `EINPROGRESS` watch
  `WRITE`; on failure (`SO_ERROR`) advance to the **next address** in the list
  (robustness requirement). When the list is exhausted, reply with the most specific
  `REP` we can (host unreachable `0x04`, connection refused `0x05`, network unreachable
  `0x03`, general `0x01`).
- **REQUEST_WRITE:** send the reply (`REP=0x00` + bound address/port) then enter relay.
- **RELAY:** see §6.

Each state owns: `on_arrival`, `on_read_ready`, `on_write_ready`, `on_block`, `on_departure`.

---

## 6. The relay — two coupled halves

Once established, a session has two sockets (client `C`, origin `O`) and **two buffers**:
`C→O` and `O→C`. Interest registration is data-driven:

- We want to **read** from `C` only if the `C→O` buffer has room.
- We want to **write** to `O` only if the `C→O` buffer has pending bytes.
- Symmetrically for the `O→C` direction.

This naturally applies **backpressure**: a slow origin stops us reading from a fast
client (no unbounded buffering — satisfies the bounded-memory requirement). Bytes
transferred are tallied here for metrics. Half-close (`EOF` on one side) shuts down the
corresponding direction with `shutdown()` and tears the session down once both
directions drain.

---

## 7. DNS resolution (the one allowed thread)

- FQDN requests are queued to a small worker (a dedicated thread, or `getaddrinfo_a`).
- The worker calls `getaddrinfo` (which may block) and pushes `(conn_id, struct
  addrinfo*)` onto a result queue, then writes one byte to the selector's self-pipe.
- The main thread drains the queue on wakeup and resumes each parked connection.
- The worker touches **no sockets** and does **no other I/O** — exactly as the consigna
  permits. We hint `AI_ADDRCONFIG` and request both families so the fallback list (§5
  CONNECTING) is populated.

---

## 8. Management / monitoring protocol (our design — separate deliverable)

A second passive socket on its own port, same process, same selector. **Not** a SOCKS
extension. Full byte-level spec goes in `docs/PROTOCOL.md`; the high-level design intent:

- **Transport:** TCP, multiplexed in the same non-blocking loop as the proxy.
- **Encoding:** *proposed* compact **binary** request/response (length-prefixed frames)
  for the wire, with the **client** translating ergonomic commands
  (`client add-user pablito pass1234`) into frames. We will justify binary-vs-text in the
  report. (Open for discussion — a line-based text protocol is simpler to debug; binary
  is more "in the spirit" of the course. Decision pending.)
- **Capabilities:** authenticate; list/add/remove proxy users; toggle auth method; read
  metrics (historical conns, concurrent conns, bytes); get/set runtime config (timeouts,
  buffer sizes); tail/query access log.
- **Auth:** the management channel has its own admin credentials (decision pending:
  shared-secret token vs. admin user table). The client handles the auth handshake; it is
  *not* netcat-friendly by design (per the consigna's explicit prohibition).
- The client **may use blocking I/O** (it's simple and sequential).

---

## 9. Cross-cutting modules

- **users:** in-memory store, runtime-mutable via the mgmt protocol. Passwords compared
  constant-time. (Persistence optional — metrics may be volatile; users likely seeded by
  CLI + mgmt additions.)
- **metrics:** plain counters updated on the data path; read by mgmt. Volatile by spec.
- **access_log:** append a structured record per established connection — timestamp,
  username, client addr, target (FQDN/IP + port), bytes, outcome. Designed to answer the
  "external complaint" query.
- **config:** a single struct read by the data path, mutated only by the main thread
  (no locks needed — single-threaded), exposed via mgmt.

---

## 10. Signals & graceful shutdown

- `SIGTERM` / `SIGINT` handled via `sigaction`; the handler only sets a `volatile
  sig_atomic_t` flag (and/or writes the self-pipe to break `select`). No real work in the
  handler.
- On flag: stop accepting (close/unregister the listen fds), let in-flight sessions
  drain, then exit cleanly (free buffers, close fds, join the DNS worker).
- A **second** signal may force immediate exit.

---

## 11. CLI & POSIX conventions

Both binaries follow IEEE 1003.1 utility conventions (the reference arg parser, once
published, drops into `shared/args`). Anticipated server options: SOCKS port, management
port, bind address, initial users, log destination, buffer size, `-h`/`-v`. The client
takes the management host/port + a subcommand verb.

---

## 12. Error-handling philosophy (the "no weak patches" rule)

- Every syscall return is checked; `EAGAIN/EWOULDBLOCK/EINPROGRESS/EINTR` are *expected
  control flow*, not errors, and handled explicitly.
- Failures propagate to the SOCKS state machine, which emits the **most specific REP code**
  rather than a generic one (the consigna asks us to use "the full power of the protocol").
- No silent `catch-and-continue`; no fixed-size assumptions that "usually" hold. Parsers
  are length-driven and bounded.
- Resource ownership is explicit per connection; teardown frees everything exactly once.

---

## 13. Testing & stress

- Unit tests for the pure parsers (negotiation/auth/request) — they're table-driven and
  partial-input-driven, so we can feed byte-at-a-time to prove partial-read correctness.
- Interop tests against a real SOCKS5 client (`curl --socks5`, browsers).
- Stress: ramp concurrent connections past 500, measure throughput degradation and max
  sustained connections for the report's required stress section.

---

## 14. Suggested build order (milestones)

Each milestone is independently reviewable; we run `ecc:cpp-reviewer` after each.

1. **Foundation:** fix `Makefile.inc` (C11 + flags), bring in `selector` + `buffer` +
   `stm`, a trivial echo server proving the non-blocking loop works.
2. **SOCKS negotiation + auth:** method negotiation and RFC 1929 user/pass against a
   static user store. No relay yet.
3. **Request + CONNECT (IPv4/IPv6) + relay:** full data path to literal IP targets.
4. **DNS + fallback:** async resolver thread, FQDN targets, multi-address robustness.
5. **Metrics + access log + graceful shutdown.**
6. **Management protocol + client** (and `docs/PROTOCOL.md`).
7. **Hardening + stress tests + report.**

---

## 15. Open questions (need your call before/at coding time)

1. **Selector/buffer source:** reuse the cátedra's published `selector`/`buffer`/`stm`
   (attributed) or write our own to the same interface?
2. **Management wire format:** binary length-prefixed frames vs. line-based text?
3. **Management auth model:** shared admin token vs. admin user table?
4. **User persistence:** purely volatile + CLI-seeded, or persisted to a file?
