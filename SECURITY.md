# Security policy

## Reporting a vulnerability

Please report security issues privately through GitHub:
<https://github.com/SelfHold/enclave/security/advisories/new>.

Do not open a public issue for a vulnerability. Include the Enclave commit, the platform
(Linux or Android), steps to reproduce and what an attacker gains. You will get an answer as
soon as possible; a fix and an advisory follow once the issue is confirmed.

## Scope

In scope: Enclave's own code and changes, that is, `enclave.py`, `patches/`, `src_overlay/`
(in particular the sync code that talks to Shard's Connector API and handles the pairing token
and passwords) and the build configuration in `config/`.

Out of scope here:

- **Chromium bugs** that also affect upstream Chromium: report them to the Chromium project
  (<https://issues.chromium.org/>, see
  [Chromium's security page](https://www.chromium.org/Home/chromium-security/reporting-security-bugs/)).
  Enclave picks up fixes when it moves to a newer Chromium release.
- **uBlock Origin** bugs: report them to the [uBlock Origin project](https://github.com/gorhill/uBlock).
- **Shard** bugs (the Connector service, vault, password store): report them in the
  [Shard repository](https://github.com/SelfHold/shard/security/advisories/new).

## Supported versions

Only the current `main` branch, built on the Chromium release in `chromium_version.txt`.
Enclave does not distribute builds; anyone who builds it should rebuild after fixes and
Chromium updates.
