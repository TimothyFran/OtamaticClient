# OtamaticClient — triage of open issues (#1..#5)

Working note: difficulty ranking of the five open issues, with dependencies.
Scope analysis based on commit `a5d3447` (library version `0.3.1`).

## Summary table

| Rank | Issue | Difficulty | Surface touched | Blocked by | Risk |
| --- | --- | --- | --- | --- | --- |
| 1 | #2 `PrepareForUpdate` event | Low | 1 enum + 1 emit point + docs | — | Low |
| 2 | #1 `OutOfMemory` + heap pre-check | Low | enum + accessor + `beginUpdate()` | — | Low/Med (semantics of `getError()==0`) |
| 3 | #3 Failure classification in the version guard | Medium | 9 call sites + policy + NVS/log | **#1** (needs the new error code) | Med (behaviour change) |
| 4 | #4 Reduce peak heap of the pipeline | Medium-High | 6 sub-items, buffer/HTTPClient/UpdateState lifetime | — (soft: helps #5) | Med-High (HTTPClient reuse) |
| 5 | #5 Non-blocking / incremental download API | High | full OTA pipeline → state machine | — (soft: benefits from #2 + #4.1) | High (safety-critical path) |

Verification limits: no CI, no tests, no `pio`/`arduino-cli` and no PyPI access in this
environment, so every change must be reviewed by inspection / on hardware. This raises the
relative cost of the invasive issues (#4, #5) more than the additive ones (#1, #2).

## Dependencies

- **#1 → #3 (hard).** #3 wants `Update.begin()` out-of-memory failures classified as
  *transient*, i.e. not counted by the failed-version guard. Without the `OutOfMemory` code
  introduced in #1 there is no reliable way to tell "no contiguous 4 KB" from "real partition
  problem": the Arduino core leaves `Update.getError() == UPDATE_ERROR_OK (0)` in both cases.
  #1's own "Why it matters" states the distinction drives the retry policy of the companion
  issue. Implementing #3 before #1 means duplicating a heap heuristic inside the guard.
- **#2 → (#4, #5) soft.** #2 alone is a complete, useful feature (it removes the
  `setAutoUpdate(false)` + flag + deferred `applyUpdate()` workaround). It is however the
  natural lifecycle hook that #5 needs for an async flow, and it gives applications a window
  to free RAM before `Update.begin()`, which is exactly the failure mode #1/#4 describe.
- **#4 → #5 soft.** #5 makes the download buffer and the `HTTPClient` long-lived by design, so
  #4.1 (`setDownloadBuffer()`) and #4.5 (`UpdateState` as a member) are largely subsumed by
  #5's state ownership. Doing #4 first shrinks #5; conversely, #4.2 (reuse one `HTTPClient`
  for check and download) is much easier *inside* an async state machine and can be deferred
  into #5.
- **#1 and #2 are independent** of everything else and of each other.

## Notes per issue

### #2 — PrepareForUpdate event — Low
Single emit point: both entry flows (`requestCheckNowInternal()` auto path at
`OtamaticClient.cpp:515-519` and `applyUpdate()` at `:534-539`) call `applyUpdateInternal()`,
so one emit at its start covers both. The `_busy`/`_transportBusy` guards are already held
when the event fires, which makes the "must not call OtamaticClient APIs" contract fall out
for free. Enum values are `uint8_t` and `getEventName()` has a `default` branch: append the
new value at the end to avoid shifting existing codes. Portal integration ("if feasible") adds
a small amount of work since `OtamaticWebPortal` is already a friend class. The optional
`onPrepareUpdate(std::function<void()>)` variant costs a `<functional>` include and ~2 words
of RAM per instance: it can be skipped for a minimal first step, or added with the plain
function-pointer overload only.

### #1 — OutOfMemory + heap pre-check — Low
Adding `OtamaticClientError::OutOfMemory` at the **end** of the enum keeps `BeginFailed == 5`
(the value quoted by the externally documented error catalogue) stable. The core change is a
handful of lines in `beginUpdate()` (`:669-715`): heap pre-check via
`heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)` + `ESP.getFreeHeap()`, a configurable
threshold (`setMinUpdateHeap()`, default 8-12 KB), and mapping a failed `Update.begin()` with
`Update.getError() == UPDATE_ERROR_OK` to `OutOfMemory` instead of `BeginFailed`.
Two things to decide, both cheap:
1. placement — `beginUpdate()` is called at `:597`, *after* the download connection is
   already open (`:569`..`:591`), while the issue asks ideally to fail before the connection:
   the pre-check should therefore be hoisted into `applyUpdateInternal()` (or exported as a
   small helper) as well as kept in `beginUpdate()`;
2. because `beginUpdate()` is the shared pipeline entry, the portal path
   (`OtamaticWebPortal.cpp:491`) is covered by the same check with no extra work — a real
   advantage of this issue.
The heuristic (`getError() == 0` + low heap) is not a proof of OOM, but it is the best signal
the Arduino core exposes; it is a pure refinement of an already-failing path, so the blast
radius is small. Only user-visible caveat: callers switching on `BeginFailed` may need to
handle the new value (documented behaviour change).

### #3 — Failure classification in the failed-version guard — Medium
The change is mechanical but wide: `recordAttemptFailure()` is called from nine sites
(`:470, :479, :495, :502, :571, :588, :599, :649, :659`), each of which must be classified as
*firmware-related* or *environment/transient*. Open design points to settle first:
- keep the current default (`All`) for compatibility and add
  `setFailedVersionGuardMode(FirmwareOnly)`, or change the default outright;
- whether a transient failure must leave the persisted counter untouched (it should, or three
  low-heap moments still blacklist a good image through the NVS path);
- interaction with `recordFailedVersion()`'s "a different version resets the counter" policy
  when the failure is transient;
- the log split requested as a bonus: `storeIgnoredVersion()` (`:134-150`) currently logs
  "Ignoring version 69 after 1 failed attempt(s)" although the ignore only activates at
  `_ignoredFailures >= _maxFailedAttempts` — needs the informational/actual split.
Also touches the README section that documents the guard ("every failed attempt ... is

### #4 — Reduce peak heap usage — Medium-High
Six sub-items of very different weight:
- **trivial**: #4.3 `http.header("Transfer-Encoding") == String("chunked")` → `equalsIgnoreCase`
  on the `const char*` (two sites, `:440`, `:593`); #4.6 README memory contract.
- **low**: #4.4 construct `ChunkDecodingStream` only when chunked (`:594-595`) — the object
  must outlive the call, so it needs an `if` branch around the whole download or a small
  indirection; #4.1 `setDownloadBuffer()` replacing the 1 KB stack array at `:609` (public
  API + ownership/outliving rules; a `static` default trades stack for permanent RAM).
- **medium**: #4.5 `_updateState` as a preallocated member. `_updateState == nullptr` is the
  current "no update in progress" sentinel used by `writeUpdateChunk()`, `finalizeUpdate()`,
  `abortUpdate()` **and** by the portal (`OtamaticWebPortal.cpp:491,517,566`), so it must be
  replaced by an explicit flag in shared code.
- **risky**: #4.2 reuse one `HTTPClient` for check and download. `HTTPClient` is not a small
  object, so a member instance trades *peak* heap for *permanent* heap, partly against the
  goal; keeping the connection warm instead touches keep-alive/proxy behaviour. This item
  should be descoped or deferred into #5, where the transport is state-owned anyway.
Net: no single hard problem, but lifetime/ownership decisions and a benefit that can only be
observed on real, fragmented heaps (no tests here).

### #5 — Non-blocking / incremental download API — High
Largest change by far. The blocking loop at `:614-643` must become a state machine driven from
`loop()`, which forces the download state to survive across calls: `HTTPClient`, the
(de-chunking) `Stream`, the `_checkData` snapshot at `:484-487`, the SHA-256 context and
counter already in `UpdateState`, plus stall timers and error state. The subtle part is the
concurrency contract: `_busy`/`_transportBusy` (`:515-541`, `OtamaticWebPortal.cpp:468`) are
currently scoped to a single synchronous call, and they must be held across many `loop()`
iterations, otherwise a periodic check or a portal upload can grab the same `Client` while the
async download still owns it. `requestCheckNowInternal()` must also be made aware that an
async update is in flight. Add the new API surface (`applyUpdateAsync()`, `updatePhase()`,
`updateProgress()`, `setDownloadBudget()`), keep the blocking path working for simple
sketches, and update README + examples. This is the most safety-critical code in the library
and the hardest to verify without hardware: highest risk of silent regressions.

## Recommended order

1. **#2** — smallest, self-contained, unblocks the workaround-free `autoUpdate(true)` flow.
2. **#1** — small and additive; also the prerequisite of #3; protects the portal path too.
3. **#3** — policy work, but only after #1 gives it the failure taxonomy it needs.
4. **#4** — independent; do it here so its buffer/`UpdateState` ownership decisions are already
   settled for #5, and defer #4.2 (shared `HTTPClient`) into #5.
5. **#5** — last: needs #2 as lifecycle hook and inherits #4's state ownership; largest and
   riskiest, so it should start from the most stabilised pipeline possible.

counted"), so docs must move with the code.


## Design review of #2 — PrepareForUpdate

Verdict: **do not implement as written** (redundant, wrong emit point); optionally re-scope to the
pre-`Update.begin()` position, which is the only genuinely missing hook.

Premises of the issue, checked against the code:

| Claim | Reality |
| --- | --- |
| "No usable window before the download" | `UpdateAvailable` is emitted at `:508`, the automatic download starts at `:519`: the callback runs *before* the connection and before `Update.begin()`. Nothing is allocated in between, and `_autoUpdate` is read at `:510`, i.e. **after** the callback, so the callback can even veto this cycle with `setAutoUpdate(false)`. |
| "`applyUpdate()` is a no-op inside the callback" | True (`_transportBusy` is held by the check at `:530`), but that is the supported deferred pattern, and it is already documented in `examples/AdvancedExample/AdvancedExample.ino:8-11, 62-66, 119, 132-147` and linked from the README "Next steps". |
| "`std::function` variant makes the hook usable with `autoUpdate(true)`" | False. Both forms are equally synchronous: the wrapper changes syntax, not capability. What `autoUpdate(true)` cannot do is *defer*, and no hook fired inside a synchronous library call can provide a loop turn — the tool for that is `setAutoUpdate(false)`. |
| Proposed emit point (start of `applyUpdateInternal()`, `:545`) | Functionally identical to `UpdateAvailable`: only `_busy.exchange()`, `isValid()` and `isVersionIgnored()` run in between, no allocation. Zero added capability, plus public API surface and a second "before the download" event to confuse integrators. |

The real gap (why the need behind the issue is legitimate, but located elsewhere): the 4 KB
contiguous block must exist at `Update.begin()` (`:691`), which runs **after** `http.GET()`
(`:584`), i.e. after the download's own TCP+TLS handshake. Memory freed early — in the
`UpdateAvailable` callback — can therefore be re-consumed by our own transport before the
allocation that matters. The field data of #1 is consistent with exactly this: an app-side gate
at 12,288 B (`#1`, "Workaround used today") and, seconds before the failure,
`free heap 24,136 / largest block 8,692` — the margin evaporates in the window that contains
the transport setup, and `Update.begin()` still failed. Freeing *late*, immediately before
`beginUpdate()`, is the only position where the freed block is still available to the 4 KB
allocation — and today there is no hook there in any flow (`autoUpdate(false)` does not help:
`applyUpdate()` runs handshake → `beginUpdate()` with no callback in between).

Options:
- **A (minimal)** — close #2 as works-as-intended; the flow exists and is documented; at most
  add a README pointer to `AdvancedExample`.
- **B (re-scope, preferred if accepted)** — emit the event between `:595` and `:597` (after the
  GET / de-chunking setup, before `beginUpdate()`), with a positional name
  (`BeforeUpdateBegin`), contract: synchronous, no API calls, instantaneous frees only. It
  composes with #1 into pre-check → hook → re-check → `Update.begin()`. It must not be sold as
  a way to do graceful multi-turn shutdown: that remains `autoUpdate(false)`.

Posted as a comment on #2 (concise version + the three log points):
https://github.com/TimothyFran/OtamaticClient/issues/2#issuecomment-5820547562


## The 4 KB block: lifecycle and the two free windows

Verified against arduino-esp32 master (`libraries/Update/src/Updater.cpp`); the issue's log line
`Updater.cpp:184` is an older core, same mechanism.

### The block itself

```cpp
// UpdateClass::begin()  (Updater.cpp:319 on master)
_buffer = new (std::nothrow) uint8_t[SPI_FLASH_SEC_SIZE];   // 4096 bytes
if (!_buffer) {
  log_e("_buffer allocation failed");
  return false;          // NOTE: _error is NOT set -> Update.getError() stays 0
}
```

- It is the **flash staging buffer**: it must be one **contiguous** 4096-byte block, and it is
  held for the **whole OTA window** (`_size` is assigned only after it succeeds, and it is the
  first thing `Update.write()` needs).
- Freed only in `_reset()` (`delete[] _buffer`), which is called by `begin()` at the start of a
  session, by `abort()` and by `end()` (both success paths) — i.e. at the end of the OTA window,
  not "right after".
- Confirms #1 from the core side: the failure path returns `false` **without** setting `_error`,
  so `Update.getError() == UPDATE_ERROR_OK (0)` — a partition error and an allocation failure
  look identical from the application.
- Also useful for #3: `_size` is set **after** the allocation, so a failed `begin()` leaves
  `Update` in the "not running" state and a retry is clean.

### Timeline of one OTA cycle (allocation points)

| # | Point | What is allocated |
| --- | --- | --- |
| 0 | `UpdateAvailable` callback — `OtamaticClient.cpp:508` | **the only app-visible free window today** |
| 1 | `http.begin()` — `:569` | negligible (uri `String`, header map) |
| 2 | **`http.GET()` — `:584`** → `HTTPClient::connect()` → `_client->connect()` (`HTTPClient.cpp:1175`) | **the download's own TCP+TLS session: mbedtls contexts + contiguous in/out buffers (tens of KB)** |
| 3 | `collectHeaders`/`addHeader`, `ChunkDecodingStream` — `:579-594` | few hundred bytes |
| 4 | **`Update.begin()` — `:691`** | **the contiguous 4096 bytes** — where the field failure happens |
| 5 | `new UpdateState()` `:697` + SHA-256 context `:701` | ~100-200 bytes (#4.5) |

TLS sessions are allocated in `connect()` and freed in `stop()`
(`HTTPClient::disconnect()`, `HTTPClient.cpp:435`, reached via `http.end()`). The check's own
session is already released at `:446`, so the download re-allocates one **inside the window**
between the only available free hook and `Update.begin()`.

### Consequence: freeing early is necessary but not sufficient

- **necessary**: if there is no room for the download's TLS session, `GET()` fails first (a
  different error: ConnectionFailed/HttpFailed) and we never even reach `Update.begin()`;
- **not sufficient**: the freed block sits in the pool and the download's TLS session — allocated
  *after* the free — can take it (or fragment around it), so contiguity for the 4 KB is not
  guaranteed. Consistent with the field data of #1: app-level gate at 12,288 B, telemetry at
  `free 24,136 / largest 8,692` shortly before, and `_buffer allocation failed` anyway.

Therefore two hooks with different jobs, not one:

| Window | Where | Purpose | Exists today? |
| --- | --- | --- | --- |
| **A — before the transport** | `UpdateAvailable` (`:508`), or the manual `applyUpdate()` from `loop()` | let the download's TLS session fit (stop BLE/MQTT/web server), or decide to postpone | ✅ yes (`setAutoUpdate(false)` + flag, documented in `AdvancedExample`) |
| **B — before the OTA buffer** | inside `beginUpdate()`, immediately before `Update.begin()` (`:691`) | secure the contiguous 4 KB when the download session is already up and can no longer steal the RAM | ❌ **no, in no flow** |

Window B is the genuinely missing capability behind #2, and placing it inside `beginUpdate()`
covers the portal upload path too (`OtamaticWebPortal.cpp:491`). Contract: synchronous, few
instructions, no library API calls (`_busy` is held), **instantaneous frees only** — it cannot
replace window A when the application needs a loop turn. Cost: a wasted connection + request
headers (the body has not been read yet, so no bandwidth).

### Measure before designing

Three log points are enough to decide which window matters, and they are also half of #1's
pre-flight check:

```cpp
static void logHeap(const char* tag) {
    log_i("[%s] free=%u largest=%u", tag, ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}
```

called in the `UpdateAvailable` callback, immediately before `beginUpdate()` (`:597`), and in the
`Update.begin()` failure path (`:692`). If the value collapses between the first two points, the
transport is what eats the margin and window B is the only mitigation available without #4.


## Diagnostic patch implemented

Commit `4a453fd` on `cline/3trejwat`: `src/OtamaticClient.cpp` only, no header change, no API
change. One file-local helper in the existing anonymous namespace:

```cpp
void logHeapGauge(const char* tag) {                    // OtamaticClient.cpp:35
    log_i("[heap] %s: free=%u largest_block=%u", tag, (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}
```

plus `#include <esp_heap_caps.h>` and the three call sites:

| Point | Line | Tag |
| --- | --- | --- |
| 1 | `:519` (right before `transmitEvent(UpdateAvailable)`) | `1-update-available/pre-transport` |
| 2 | `:610` (right before the `beginUpdate()` call, after the GET) | `2-pre-beginUpdate/post-handshake` |
| 3 | `:708` (in the `Update.begin()` failure path) | `3-update-begin-failed` |

Expected output:

```
[I][OtamaticClient.cpp:36] logHeapGauge(): [heap] 1-update-available/pre-transport: free=24136 largest_block=8692
[I][OtamaticClient.cpp:36] logHeapGauge(): [heap] 2-pre-beginUpdate/post-handshake: free=...
[I][OtamaticClient.cpp:36] logHeapGauge(): [heap] 3-update-begin-failed: free=...
```

Note for reading the log: `file:line` is always the same (`OtamaticClient.cpp:36`, the `log_i`
inside the helper), so the three points are identified by the `N-...` tag, not by the position in
the file. Points 1 and 2 appear on every update; point 3 only on a failure.

Verified without a target toolchain: `heap_caps_get_largest_free_block(uint32_t)` is declared in
`esp_heap_caps.h:253` of esp-idf, `MALLOC_CAP_DEFAULT` at `:43` of the same header,
`ESP.getFreeHeap()` in arduino-esp32 `cores/esp32/Esp.h:68`; the helper was compiled standalone
with `g++ -std=gnu++17 -Wall -Wextra -Wformat=2` (stubs mirroring those signatures) with no
warnings. `log_i` is `CORE_DEBUG_LEVEL >= 1` in practice, so the lines are visible in any default
build. (This branch is based on `a5d3447`, where `platformio.ini` exists and builds with
`-DCORE_DEBUG_LEVEL=4`; upstream `main` at `2c1dd982` later removed that file and added it to
`.gitignore`. The diagnostic does not depend on it.)

