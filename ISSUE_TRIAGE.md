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

