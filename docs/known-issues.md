# Known Issues

Seven open problems identified during the April 2026 hardening audit.
Ranked roughly by time-urgency / severity.

> **Status update (July 2026, M1 "survivable in the field" branch):**
> the display policy moved from fail-closed to fail-honest (see
> README "Architecture"), which resolved or restated several entries:
> **#3** the escape hatch is now the ordinary, visible recovery path —
> the first two disagreeing cycles display as holdover with the bound
> inflated to cover the disagreement, and the third snaps with a
> prominent `*** TIME STEP ***` event-log entry. **#4** the
> out-of-process watchdog and its overlay were removed entirely (the
> reciprocal display gate blanked healthy clocks when EDR blocked the
> self-re-exec; in-process generation-token checks around EndDraw
> remain). **#5** `WM_WTSSESSION_CHANGE` is now handled: session
> reconnect/unlock breaks timing continuity the same way resume does
> and the face shows the last verified time until re-anchored.
> **#6** all persistence now uses atomic tmp+rename writes (the MAC
> question stands, accepted as out-of-scope for same-user attackers).
> **#1** and **#7** remain open and tracked below.

---

## 1. SPKI Pin Expiry — Runtime Enrollment Needed

**Severity: High (mitigated in current tree; verify in release)**

The previous design shipped hardcoded SHA-256 SPKI pins for NTS and DoH leaf
certificates. That created a scheduled outage: when providers rotated keys,
Time Actual would reject them until a new binary was built and deployed.

The current implementation removes provider cryptographic material from the
executable. `src/nts.c` and `src/dns.c` now contain endpoint metadata only.
On first run, renewal, or expired-pin recovery, Time Actual validates the endpoint
through Windows certificate-chain and hostname policy APIs, captures the leaf
SPKI SHA-256, and stores it in `%APPDATA%\TimeActual\pins.dat` protected by DPAPI
and a strict current-user/SYSTEM ACL. See `docs/pins.md`.

The NTS concurrence gate was also tightened: a trusted cycle now requires two
operator-diverse NTS samples authenticated by enrolled pins. The old
single-NTS fallback is removed.

**Residual risk:** First enrollment and renewal now depend on the maintained
Windows/Web PKI. A local attacker already running code as the same Windows
user can generally call DPAPI as that user, so the pin cache is not a
same-user code-execution boundary. Revocation responder outages are logged and
soft-failed only when the chain is otherwise valid.

**Follow-up:** Exercise real first-run enrollment on a clean profile before
release, and monitor logs for provider certificate rotations.

---

## 2. `Clock_BeginDisplayCommit` Held `g_cs` Across `EndDraw` — RESOLVED

**Severity: Medium (latency / correctness risk) — resolved**

Previously, `Clock_BeginDisplayCommit()` returned 1 with `g_cs` held, and the
caller (`Paint()` and `RenderClockToBitmap()`) called
`ID2D1RenderTarget_EndDraw()` before releasing the lock via
`Clock_EndDisplayCommit()`. Because `EndDraw` may flush the GPU command queue,
holding a critical section across a GPU submission stalled the NTP aggregator
thread for the entire duration of that flush whenever it needed `g_cs` inside
`Clock_OnPollCycle` → `Clock_OnSyncedNtpUtc` — potentially hundreds of
milliseconds on a slow driver or under GPU load.

**Resolution:** The lock-holding `Clock_BeginDisplayCommit` /
`Clock_EndDisplayCommit` pair was removed. Painters now snapshot the display
time and generation under the lock (`Clock_ReadDisplayTime`), then validate the
generation with `Clock_DisplayGenerationIsCurrent()` — which takes and releases
`g_cs` internally — immediately *before* `EndDraw` and again immediately
*after*. The lock is never held across the present. If the post-present check
fails (the aggregator tripped INOP during the flush), the live window forces an
immediate repaint and the taskbar thumbnail is replaced with a fresh INOP
bitmap, so a stale dial survives at most one frame.

---

## 3. Escape Hatch Overrides the Fail-Closed Guarantee

**Severity: Medium (by design, but should be explicit)**

In `Clock_OnPollCycle`, after `LOCAL_FAULT_ESCAPE_N` (currently 3)
consecutive cycles where all 6 sources agree but the local QPC projection
disagrees by more than 200 ms, the code force-accepts the NTP time as a
snap. The intent is to recover from a corrupted local anchor without user
intervention.

This is the one code path where the fail-closed policy is explicitly
overridden. An adversary who could simultaneously control both NTS operators
(pinned leaf certs) plus 4 randomly-drawn metrology institutes for 3
consecutive cycles could use this path to inject attacker-controlled time.
Practically this is extremely difficult, but it is a real, documented
exception to the hardening model.

**Mitigation needed:** At minimum, log a prominent warning on each escape-hatch
activation that is visible in the UI event log. Consider whether 3 cycles is
the right threshold, and whether the snap should also trip a persistent alarm
state that requires an explicit user acknowledgement to clear.

---

## 4. Watchdog INOP Overlay Can Be Z-Order Defeated

**Severity: Medium (environmental)**

The watchdog's INOP overlay window is created with
`WS_EX_TOPMOST | WS_EX_TOOLWINDOW`. Any other `WS_EX_TOPMOST` application
(a full-screen game, a screen recorder, another always-on-top utility) can
place itself on top of the overlay, silently concealing the INOP state from
the user.

The INOP state does propagate correctly to the taskbar thumbnail via
`DwmInvalidateIconicBitmaps`, so the indication is not *completely* lost. But
the primary visible window indicator can be obscured without any notification.

**Mitigation needed:** If the overlay is being used as a safety-critical
display, consider also raising an audible or system-tray notification when the
watchdog detects a stale state, independent of the visual overlay.

---

## 5. No `WM_WTSSESSION_CHANGE` Handling

**Severity: Low–Medium**

`WM_POWERBROADCAST` (suspend/resume) correctly trips INOP and immediately
kicks an NTP cycle. However, there is no handler for
`WM_WTSSESSION_CHANGE`, which covers:

- RDP session disconnect and reconnect
- Fast user switching (console session handoff)
- Remote session lock/unlock

In these scenarios the NTP polling thread presumably continues running in the
background, so in practice the clock likely recovers on its own. But it is an
untested gap: a remote session could reconnect after a long disconnection with
a stale anchor and a display lease that hasn't yet expired, briefly showing a
confident-looking dial that is actually unverified.

**Mitigation needed:** Register for `WM_WTSSESSION_CHANGE` via
`WTSRegisterSessionNotification`, and on `WTS_SESSION_UNLOCK` or
`WTS_REMOTE_CONNECT`, call `Clock_TripInop` + `Ntp_Start` the same way
`WM_POWERBROADCAST` does.

---

## 6. `discipline.dat` Has No Integrity Protection

**Severity: Low**

The persisted discipline rate is stored as plaintext in
`%APPDATA%\TimeActual\discipline.dat`. Any process running as the same Windows
user can modify or replace it. The ±200 ppm absolute clamp and the
interval-scaled per-cycle change clamp (at most ±20 ppm) limit the damage, and re-verification happens
on the first successful NTP sync. So the worst-case outcome is a brief period
after startup where the clock displays with a subtly wrong rate, corrected
within one poll cycle.

This is a low-severity real issue: the on-disk trust state is not part of the
hardening model, and an attacker with local user access has easier paths to
compromise the display anyway.

**Mitigation needed:** Either accept this as out-of-scope (document it
explicitly in the threat model), or HMAC-protect `discipline.dat` with a key
derived from a machine-bound secret (DPAPI-protected) and reject the file if
the MAC fails.

---

## 7. IPv4 Only

**Severity: Low (usability gap)**

All DNS resolution, SNTP sockets, and NTS-KE connections use `AF_INET`. In
an IPv6-only network environment (increasingly common in enterprise and mobile
networks), every source lookup fails and the clock goes permanently INOP.
This is correctly fail-closed behaviour, but it also makes the application
completely unusable on those networks without a workaround (NAT64 or a
dual-stack gateway).

The limitation is acknowledged in `dns.h` ("IPv4 only in this cut. AAAA can
follow").

**Mitigation needed:** Add `AAAA` record resolution to `dns.c` /
`Dns_Resolve()`, and update `NtpQueryHost` and the NTS socket code to accept
and use IPv6 addresses. This is purely additive and does not affect any
existing hardening logic.
