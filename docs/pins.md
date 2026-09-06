# Lunar Pin Enrollment

Lunar no longer ships provider cryptographic material in `Lunar.exe`.
The executable contains endpoint metadata only: host names, bootstrap IPs for DoH,
labels, ports, and operator-family names. The trust data needed for DoH and NTS is
created locally on first use.

## Trust Model

First run, renewal, and expired-pin recovery use the Windows Web PKI:

1. Lunar opens TLS 1.3 to the configured DoH resolver or NTS-KE server.
2. mbedTLS performs the TLS transport with certificate verification disabled inside mbedTLS.
3. Lunar passes the peer certificate chain to Windows certificate-chain APIs.
4. Windows builds a server-auth chain from the current machine/user certificate stores.
5. Windows applies hostname validation with `CERT_CHAIN_POLICY_SSL`.
6. Lunar hashes the validated leaf certificate's SubjectPublicKeyInfo (SPKI) with SHA-256.
7. Lunar stores that SPKI digest locally as a continuity pin for the endpoint.

## Multi-SPKI Sets

Each endpoint keeps a SET of up to four enrolled SPKIs, not a single pin. Anycast and
multi-POP providers legitimately present different leaf keys per point of presence, so a
roaming laptop would otherwise flap between "pinned" and "mismatch" as it changes POPs.

- A TLS leaf matching ANY stored, un-expired SPKI in the set authenticates as an enrolled
  pin.
- Every new SPKI observed through a CA-validated connection is appended to the set. When
  the set is full, the oldest observation is evicted.
- Re-observing a known key refreshes its validity metadata and makes it the newest member
  (so eviction order tracks how recently a key was confirmed, not just first enrollment).
- Expired members are excluded from matching but remain in the set until evicted.

After enrollment, ordinary operation is pin-first. If the leaf matches the set, the TLS
connection is accepted without needing a fresh CA decision. When the newest pin reaches its
renewal window, Lunar keeps accepting still-matching pins while it attempts a fresh Windows
CA validation. If every stored pin has passed its certificate `notAfter` time, the set is no
longer usable by itself; Lunar must obtain a fresh Windows CA validation or the endpoint
fails closed.

External network failures can therefore cause INOP, but they cannot force Lunar to replace
a valid local pin outside the renewal/expiry path. For NTS endpoints, a pin mismatch before
renewal is no longer an unconditional hard reject; see "Corroborated Rotation Acceptance"
below. A mismatch that also fails Windows CA validation is always rejected.

## Local Storage

Pins are stored in `%APPDATA%\Lunar\pins.dat`.

The plaintext format is line-oriented, version 2 (`LUNAR_PINSTORE|2`): one record line per
enrolled SPKI, where repeated endpoint keys accumulate into that endpoint's SPKI set in
file order (oldest first). Version-1 files (one line per endpoint) load without error as
single-entry sets and are upgraded to version 2 on the next write.

The file is plaintext only inside the process. On disk it is protected with Windows DPAPI
via `CryptProtectData` / `CryptUnprotectData`, using the current user's Windows profile as
the cryptographic protection boundary. Lunar writes through a temporary file, flushes it,
and atomically replaces the old cache with `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)`.

After each write, Lunar applies a protected DACL that grants access to the current user and
SYSTEM. This blocks accidental edits and ordinary cross-user tampering.

Important limitation: Windows DPAPI cannot prove that only this exact executable is reading
the file. Code already running as the same Windows user can usually call the same DPAPI
unprotect operation. Lunar's local storage protects against accidental edits, casual local
tampering, and other users on the same machine; it is not a same-user arbitrary-code
execution boundary.

## Renewal Policy

Lunar stores certificate validity metadata next to each SPKI digest:

- endpoint kind: DoH or NTS
- endpoint label, host, port, and operator family
- SPKI SHA-256 digest and hex form
- certificate `notBefore` and `notAfter`
- Unix timestamps for validity and computed renewal time
- last enrollment status

The renewal margin is adaptive:

    margin = min(30 days, (notAfter - notBefore) / 3)

The renewal window starts `margin` before `notAfter`. Long-lived certificates keep the
classic 30-day margin; with the CA/B-Forum trend toward short-lived leaves the margin
scales down proportionally (a 47-day leaf renews ~15.7 days early, a 6-day leaf 2 days
early) instead of degenerating to "always renewing". Unknown or garbled validity metadata
falls back to the 30-day cap. The chosen margin and which rule produced it are recorded in
the pin-store log line for every save.

During the renewal window, a matching pin can continue to authenticate the endpoint while
CA renewal is attempted. After every stored pin's `notAfter`, CA validation is mandatory.

If CA validation succeeds, Lunar saves the observed leaf SPKI into the endpoint's set with
one of these statuses:

- `first-run-enrollment`
- `scheduled-renewal`
- `expired-renewal`
- `pin-rotation` (mismatch inside the renewal window)
- `rotation-corroborated` (NTS: mismatch outside the renewal window, promoted after
  corroboration by the other slot; see below)
- `pin-rotation-corroborated` (DoH: mismatch outside the renewal window, promoted after
  the same key is seen again ten minutes later; see below)

## Corroborated Rotation Acceptance (NTS)

Providers occasionally rotate keys EARLY -- emergency re-issuance, CA mass-revocation,
infrastructure moves -- outside any renewal window. Hard-rejecting such a leaf would brick
that operator family until the stored pin's window opened. Instead, for NTS endpoints:

1. The presented leaf matches no stored, un-expired SPKI and the endpoint is not in its
   renewal window.
2. Lunar runs the full Windows CA + hostname validation path anyway. If that fails, the
   connection is rejected as before.
3. If CA validation passes, the NTS exchange completes, but the sample is marked with the
   `ROTATED_PIN` auth mode and the new SPKI is NOT persisted yet.
4. In the trust gate, a `ROTATED_PIN` slot may count toward the two-NTS requirement ONLY
   if the other NTS slot is a continuous enrolled pin from a different operator family.
   An attacker exploiting a rotation must therefore simultaneously defeat a still-pinned
   independent operator (plus the core source majority). Two rotated slots in one cycle
   have no continuous corroborator and hard-fail the cycle (INOP) -- that is exactly the
   shape of a CA-level machine-in-the-middle against both anchors.
5. Only when the cycle passes the gate with the rotated slot counting is the new SPKI
   promoted into the pin set (status `rotation-corroborated`), with a loud line in both
   the in-memory event log and the on-disk audit log:

       pin rotation accepted for <host>: <old-prefix>-><new-prefix>, corroborated by <other-family>

   If the cycle does not pass, nothing is persisted and the next cycle starts from the
   same stored state.

## Corroborated Rotation Acceptance (DoH)

DoH resolvers have no second slot to corroborate against inside one exchange, and the
large operators rotate their front-end leaf keys far more often than their certificates'
validity suggests (Google and Mullvad were both observed rotating roughly every 19 days on
90-day leaves, always outside the renewal window). Hard-rejecting such a leaf until the
stored pin's window opened kept those resolvers dead for weeks and filled the event log with
per-connection mismatch lines. DoH therefore corroborates a rotation over TIME instead:

1. The presented leaf matches no stored, un-expired SPKI and the endpoint is not in its
   renewal window.
2. Lunar runs the full Windows CA + hostname validation path. If that fails, the connection
   is rejected as before (`CA validation rejected`).
3. If CA validation passes, the answers from this connection ARE used (DNS answers are not
   trusted anyway: every NTS-KE handshake authenticates the address it resolves to), but the
   new SPKI is only remembered as a *candidate* for that resolver, with the tick of its first
   sighting.
4. When the SAME key is presented again at least 10 minutes after that first sighting, the
   candidate is promoted into the pin set (status `pin-rotation-corroborated`), with a
   `PIN ROTATION ACCEPTED` line in the event and audit logs. Each resolver keeps two
   candidate slots (an anycast provider can alternate between two POP keys); a further key
   evicts the oldest candidate, so a flapping interposer never accumulates corroboration,
   and a transient interposer that is gone ten minutes later is never enrolled.

Independently of pinning, a resolver that fails three consecutive queries is backed off for
one hour and skipped by the resolver walk unless every resolver in the pool is backed off.
Only transport, TLS, HTTP and malformed replies count as failures: a well-formed DNS answer
of any kind -- records, NODATA, NXDOMAIN, even SERVFAIL -- is a resolver in service (the
AAAA lookup of an IPv4-only NTP host returns NODATA every time and must not look like a dead
resolver). The first successful query afterwards puts it back in service; both edges are
logged once, and a non-200 HTTP status is named in the log the first time (then hourly) so a
resolver that refuses the request cannot hide behind the failure count. DoH runs over HTTP/2
when the resolver negotiates it via ALPN (Quad9 and Mullvad accept nothing else) and over
HTTP/1.1 otherwise; a resolver whose HTTP/2 exchange fails at the protocol level is spoken
to over HTTP/1.1 for the rest of the run, and every `resolve` line names the resolver that
answered.

## NTS Concurrence

Because first-run trust now depends on maintained public CA infrastructure, Lunar does not
allow a single NTS source to anchor the clock. A trusted cycle requires both NTS slots to
succeed, both to be pin-authenticated (enrolled pins, or at most ONE pending-rotation
sample riding on a continuous enrolled peer), and the two NTS providers to come from
different operator families. They must agree within 200 ms, and at least three of the four
plain SNTP core sources must agree with their midpoint.

If fewer than two operator-diverse NTS samples are available, the cycle is INOP: the
certainty interval simply keeps growing from the last authenticated anchor, and a tight
unauthenticated core cluster can only *widen* it, never sustain a claim (see `src/ntp.h`).

## Logging

Lunar logs the enrollment path in detail:

- missing, expired, matching, mismatching, and renewal-due local pin states
- loaded, saved, and used pin records with recorded validity windows, set sizes, the
  adaptive renewal-margin decision, and the next scheduled CA run
- deferred (corroboration-pending) rotations, and accepted rotations in both the event log
  and `audit.log`
- Windows CA validation attempts and outcome
- certificate subject, issuer, `notBefore`, `notAfter`, SPKI digest, chain status, policy status, and revocation status
- DPAPI decrypt/protect failures
- pin-cache parse/schema failures
- atomic cache writes and endpoint save status
- NTP cycle gates, including the two-operator NTS requirement

Revocation checking is requested. If Windows reports only offline or unknown revocation
status, Lunar records that fact and retries the chain build without revocation so that a
temporary responder outage does not permanently brick first-run enrollment. Hard chain or
hostname policy errors still fail the enrollment.