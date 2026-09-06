# Glossary

## AEAD

Authenticated Encryption with Associated Data. The NTS UDP reply is authenticated with AEAD
so Time Actual can detect forged or modified time packets.

## ALPN

Application-Layer Protocol Negotiation. TLS extension used by Time Actual to require `ntske/1` for
NTS-KE and `http/1.1` for DoH.

## CA

Certificate Authority. A trusted issuer in the Web PKI. Time Actual uses Windows CA validation for
first-run and renewal enrollment, then stores local SPKI pins.

## DACL

Discretionary Access Control List. Windows file permission list. Time Actual applies a protected
DACL to `%APPDATA%\TimeActual\pins.dat` for the current user and SYSTEM.

## DNS

Domain Name System. Maps names to IP addresses.

## DoH

DNS over HTTPS. Time Actual uses DoH over TLS 1.3 to resolve NTS provider hostnames without trusting
plain DNS for bootstrap.

## DPAPI

Data Protection API. Windows API used to encrypt local secrets to a user or machine profile.
Time Actual uses user-profile DPAPI for the local pin cache.

## INOP

Inoperative. Time Actual's fail-closed state: it refuses to display a trusted clock value when the
sync/trust mechanism cannot prove one.

## NTP

Network Time Protocol. Internet protocol for synchronizing clocks.

## SNTP

Simple Network Time Protocol. A simpler one-shot subset of NTP used by Time Actual for individual
time samples.

## NTS

Network Time Security. Adds cryptographic authentication to NTP by combining TLS key
establishment with authenticated UDP time packets.

## NTS-KE

NTS Key Establishment. TLS 1.3 handshake on the NTS-KE port that gives Time Actual cookies and keys
for authenticated SNTP.

## PLL

Phase-Locked Loop. Time Actual's local clock discipline logic that adjusts the QPC-based clock rate
from trusted samples.

## QPC

QueryPerformanceCounter. Windows monotonic high-resolution counter. Time Actual uses QPC, not the
Windows wall clock, for local elapsed time.

## SPKI

SubjectPublicKeyInfo. The public-key structure inside a certificate. Time Actual stores SHA-256 of
the leaf certificate SPKI as the local continuity pin.

## TLS

Transport Layer Security. The encrypted/authenticated transport used for DoH and NTS-KE.

## UTC

Coordinated Universal Time. The time scale Time Actual displays after trusted synchronization.

## Web PKI

The public certificate-authority ecosystem used by browsers and Windows certificate-chain
validation. Time Actual uses it only for enrollment and renewal of local pins.