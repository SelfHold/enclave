# Sync through Shard

Enclave syncs browser data and passwords through [Shard](https://github.com/SelfHold/shard)
(formerly CloudSync), a serverless, encrypted file sync with a password safe. Shard's code
and UI still use the old name in places (`cloudsyncd`, `dev.cloudsync.*`), and so do
Enclave's identifiers (`bezel_cloudsync`, chrome://cloudsync) and the sync page's UI strings.

Status (2026-11-26): steps 1–9 written. Linux: unit tests pass, `chrome` not rebuilt since
step 2. Android: first full build running, not run on a device yet. See "Order of work" and "Open issues".
Contract: the [Connector guide](https://github.com/SelfHold/shard/blob/main/docs/CONNECTOR.md) (Connector API v1 with the `passwords` scope,
§ 12; "CONNECTOR.md § N" below and in code comments means that file). Shard's
`proto/connector.proto` is copied into `src_overlay/components/bezel_cloudsync/proto/`.
Pinned Chromium: 158.0.8127.88.

## Decisions taken by the owner (2026-11-26)

- Chrome Sync with Google servers does not work in this build. Sync goes through Shard.
- We keep Chromium's own sync engine and replace only its server: the local sync backend
  (`LoopbackServer`) runs on every device and copies its entities to Shard.
- Data: every type the engine can sync, history included.
- Passwords are **not** kept in our records. They go to Shard's own password store
  (see "Passwords").
- Desktop transport: our own minimal gRPC client over h2c, built on the HTTP/2 code that
  is already in the tree (quiche). No `third_party/grpc`.

## How it fits together

```
 Chromium sync engine (SyncServiceImpl, unchanged data-type code)
        │  ClientToServerMessage (commit / get_updates)
        ▼
 LoopbackConnectionManager ──▶ LoopbackServer   (local replica, profile.pb)
                                   │ entity committed / imported
                                   ▼
                          BezelCloudSyncReplicator  (new, src_overlay)
                                   │ Put / Get / Watch (app id "enclave")
                                   ▼
             CloudSyncConnector: desktop = h2c gRPC; Android = AIDL via JNI
                                   ▼
                              cloudsyncd / Shard app
```

Patch 0009 (`local-sync-backend`) does the engine side:

- `kEnableLocalSyncBackend` defaults to true on desktops (Android: false until step 6;
  `sync_service_factory.cc` only honours it on Win/Mac/Linux anyway).
- Backend file on Linux/Mac: `<user data>/<profile>/Bezel Sync/profile.pb` (upstream has
  a default only on Windows, the roaming profile).
- `kSyncMigrateLoopbackServerBookmarksToClientTagHash` on by default.
- In local mode the configure context reports a consumer account
  (`kConsumerNotWellKnown`); upstream leaves it `kPending`, which keeps HISTORY and
  CONTACT_INFO stopped forever.
- Local-sync support (`data_type.cc`): HISTORY, HISTORY_DELETE_DIRECTIVES and CONTACT_INFO
  turned on; PASSWORDS and WEBAUTHN_CREDENTIAL turned off, so passwords never enter
  `profile.pb` (they go to Shard's password store, step 8).
- Local sync starts by itself: `GetSyncAccountStateForPrefs()` treats it as a syncing user
  and `ConfigureDataTypeManager` skips the "setup complete" check for it.
- In local mode `SyncServiceImpl` gets no `IdentityManager`: sync does not depend on the
  Google account. Google sign-in stays for Google web services only.
- Payment cards: Chromium never syncs local cards (only Google Pay's server cards, which
  need Google). Syncing them would be a data type of our own; not planned yet.
- Android: the Java sync settings expect an account. Patch them to show the Shard
  state. Check this first, before any other Android work.

## Replicating LoopbackServer through Shard

Each device has its own LoopbackServer. Store birthday, progress markers and version
counters stay per device and are never uploaded. Only entities are uploaded.

- **Entity ids are the same everywhere.** Client-tagged types have deterministic ids
  (client tag hash); bookmarks too, with the feature above (patch 0009).
- **Keystore key** is the same on every device: `HKDF-SHA256(AppKey, "bezel-keystore-v1")`
  instead of `RandBytes` in `LoopbackServer::GenerateNewKeystoreKey`. The Nigori entity is
  replicated like any other, so every device decrypts the same data. Before pairing the
  server has a random key; when sync via Shard is switched on, the replicator stops
  the engine, recreates `profile.pb` with the derived key (new store birthday) and lets
  the client merge its local data into it, as after a server reset (step 3).
- **Import:** an entity from another device gets a new local version (`version_ + 1`), so
  the client receives it in the next GetUpdates. Then `SyncService::TriggerRefresh` for
  its type: a push without GCM.
- **Merge of concurrent writes (§6.2):** union by entity id; for one id the one with the
  larger `(mtime, version id)` wins. Tombstones are kept 90 days.
- **Vault id changed:** stop replicating, keep the local replica, ask the user.

### Browser profiles (2026-11-26)

Each browser profile syncs into its own space; profiles with the same name on different
devices share it.

- One pairing per device for all profiles (`<user data>/Enclave Sync/pairing.json`, 0600,
  `SharedPairing`): Shard keeps one pairing per app and device, so a second pairing
  would revoke the first. Pairing in one profile pairs the device; the pairing is given up
  (`Unpair`) only when the last profile with sync on switches it off.
- Record `profiles` (`ProfileSlots`): slot id (8 hex), name, created / renamed times.
  Switching sync on in a profile takes the slot with the same name (trimmed, case-folded),
  or makes a new one. A rename here renames the slot, or, if another slot already has
  the new name, joins it (the local server is reset with that slot's key and merges in).
  A rename on another device is kept as it is. Concurrent versions merge by id; the newest
  name wins.
- A profile's records are `p/<slot>/meta`, `p/<slot>/t/...`, `p/<slot>/h/...`,
  `p/<slot>/s/...`. The keystore key is per slot: `HKDF(AppKey, "bezel-keystore-v1/" + slot)`.
- The 256-record limit is shared: the replicator counts the other profiles' records.
- Passwords: Shard's password store is one per vault, so every login the browser
  writes carries `extra["bezel.profile"] = <slot>`. A profile mirrors only its own logins;
  untagged logins (made in Shard, other apps) belong to the primary profile, the slot
  created first.
- chrome://cloudsync shows the profile's slot ("Work (primary)") and its share of records.

### Records (limit: 256 records, 256 KiB each, every write kept forever)

Every write stays in Shard's journals forever, so records are rewritten rarely and
only when their content changed. Values are a `BezelShard` proto (list of `SyncEntity` +
mtime + device), zlib-compressed.

| Key | Contents | Written |
|---|---|---|
| `profiles` | profile slots (`ProfileSlots`), not under a prefix | on enable / rename |
| `meta` | format version, list of shards | on shard layout change |
| `t/<type>/<n>` | entities of one type, `n = hash(id) % shards(type)` | 60 s after a change |
| `h/<yyyy>-W<ww>` | history visits of one ISO week | at most every 4 h and at exit |
| `s/<device>` | this device's open tabs (sessions) | at most every 15 min and at exit |

- The number of shards per type doubles when one would pass 192 KiB (rehash, one write per
  shard).
- History: 12 weeks are kept in Shard (at most ~12 records). Older weeks are deleted
  from Shard only; local history stays as it is. A week bigger than 256 KiB keeps the
  latest visits and drops the oldest.
- Budget: about 200 records for `t/` and `h/`, the rest for `s/` and `meta`. If the budget
  runs out, stop writing new shards and show an error in settings. Never drop data quietly.

## Connector client

- App id `enclave` (the browser is called Enclave in Shard; it was `bezel` until
  2026-11-26, before any data was synced). App name `Enclave`, pairing description
  `"Enclave <version> on <os>"`, e.g. "Enclave 158.0.8127.88 on Linux".
- Desktop: the token goes into `<profile>/Bezel Sync/cloudsync_state.json` (mode 0600),
  never synced. The daemon is found through `connector.json`, read again on every
  connect; Hello is retried every 30 s while Shard is not running.
- h2c: unix socket (`net::UnixDomainClientSocket`) or `127.0.0.1:port`, HTTP/2 through
  quiche `OgHttp2Adapter`, gRPC framing (5-byte prefix) and `grpc-status` trailers by hand.
  Unary calls plus the one server stream (`Watch`).
- Android: AIDL files from the Connector guide, appendix B, a Java bridge bound to
  `dev.cloudsync.cloudsync`, and JNI into the same C++ `CloudSyncConnector` interface.
  `<queries>` entry in the manifest.
- States (§4): `READY`: replicate; `OFFLINE`: queue writes; `LOCKED`: settings show
  "Unlock CloudSync" (the UI still uses the old name); `NO_VAULT` / not running: local sync only, nothing leaves the device.

## UI

chrome://cloudsync (desktop and Android): status per vault state, the pairing code
(`123 456`) while Shard waits for the user, records used, last write, passwords scope,
"Stop sync" (`Unpair`) and "Remove synced data from CloudSync" (`Delete` of every record).
Plain HTML/JS served from strings (no grit), with Enclave's dark-only design tokens and a
phone layout below 768 px. UI strings are English only. Reached from a
row in chrome://settings/people and in Android main settings.

## Passwords

Passwords live in Shard's own password store ([Connector guide](https://github.com/SelfHold/shard/blob/main/docs/CONNECTOR.md) § 12), never in Bezel
records. Pairing asks for the `passwords` scope next to the records.

- The replicator ignores `PASSWORDS`, `INCOMING_PASSWORD_SHARING_INVITATION`,
  `OUTGOING_PASSWORD_SHARING_INVITATION` and `WEBAUTHN_CREDENTIAL`.
- Browser side: our own `PasswordStoreBackend` on the login methods, with the local login
  DB as the offline cache, kept in step through `ListChanges` and `Watch` (§ 12.3).
- Chromium-only fields (`scheme`, password issues, form signatures) go into `extra` under
  `bezel.*` keys.
- `TouchLogin` on every fill: Shard keeps the use counters per device.
- A conflict copy (`<id>~<op id>`) is an ordinary second login: the user sees both.

### Passwords on Android (decided 2026-11-26)

Chromium 158 compiles no login DB on Android: the profile store is Google Play Services
or, without them, `PasswordStoreEmptyBackend`. So the mirror has nothing to mirror, and
the owner chose a backend directly on Shard instead of bringing the login DB back.

- `CloudSyncPasswordBackend` (`bezel_cloudsync_password_backend.{h,cc}`) is the profile
  store's `PasswordStoreBackend` on Android (patch 0015, `password_store_backend_factory.cc`).
  It loads the profile's logins with `ListLogins(data)` into memory, follows `Watch` through
  `ListChanges` + `GetLogins`, and writes every change with `PutLogin` / `DeleteLogin`
  (CAS on the revision; one retry on top of a re-read after `ABORTED`). Fills only move the
  use counters: `TouchLogin`, no new revision.
- No copy of the secrets on disk: Shard keeps its own copy on the device and answers
  while offline (§ 12.4). Without a usable connection (sync off, no `passwords` scope,
  Shard locked or not installed) the store is empty and writes fail.
- Profiles as on desktop: the backend sees its slot's logins (plus the untagged ones in the
  primary profile) and tags every write with `bezel.profile`.
- Two Shard entries with the same Chromium key (a conflict copy): one is shown, the
  other stays hidden until the first is gone.
- Joining: `BezelCloudSyncService` cannot depend on the password store on Android
  (SyncService already depends on it), so the controller and the backend meet in
  `CloudSyncPasswordLink`, keyed by profile directory.
- The account store (Google account) uses the empty backend.
- Saving needs a settings service: patch 0015 uses the desktop, pref-based
  `PasswordManagerSettingsServiceImpl` instead of the Play Services one.
- Managing passwords happens in Shard: "Passwords" in settings opens the Shard app
  (`PasswordManagerHelper.showPasswordSettings`); the "Google Password Manager stopped
  working" subtitle is gone.
- Tests: `CloudSyncPasswordBackendTest.*` in `bezel_password_mirror_unittests` (Linux).

## Order of work

1. Connector client on desktop (h2c + gRPC) with a test against a fake server. **Done**
   (2026-11-26): `components/bezel_cloudsync` (`ConnectorClient`, `GrpcTransport`,
   `H2cConnection`), tests in `bezel_cloudsync_unittests` (target added to `gn_all` by
   patch 0008):
   ```bash
   autoninja -C out/Bezel-linux bezel_cloudsync_unittests
   systemd-run --user --scope -p MemoryMax=1G -p MemorySwapMax=0 --quiet \
     out/Bezel-linux/bezel_cloudsync_unittests --test-launcher-jobs=1
   ```
   `ConnectorInteropTest.DISABLED_RealDaemon` runs against a real cloudsyncd (set
   `BEZEL_CLOUDSYNC_CONNECTOR_JSON`, add `--gtest_also_run_disabled_tests`). Checked
   against cloudsyncd `dev` without a vault: Hello, UNAUTHENTICATED on Get and Watch.
2. Local sync backend on Linux: prefs, profile path, bookmark ids. **Done** (2026-11-26,
   patch 0009). Checked with a headless build on a fresh profile: sync starts by itself,
   `Default/Bezel Sync/profile.pb` gets device info, prefs, Nigori, a seeded bookmark with
   a client-tag-hash id, open tabs (SESSIONS) and visits (HISTORY). The shared keystore
   key moved to step 3: it needs AppKey, so it is set when sync via Shard is switched on.
3. Replicator. **Written**: `replicator.{h,cc}`, `loopback_bridge.{h,cc}` (patch 0010
   makes LoopbackServer a `LoopbackReplica`), `proto/bezel_records.proto`. The keystore key
   comes from AppKey; on the first start with it the server is reset and takes the vault's
   entities in the same task, before the client syncs again. Change times live in
   `Bezel Sync/cloudsync_index.pb`. Tests: `ReplicatorTest.*` (two devices on an in-memory
   store, `fake_record_store.{h,cc}`).
4. Pairing and UI. **Written**: `cloudsync_controller.{h,cc}` (Hello, pairing with the
   `passwords` scope, Watch with backoff, vault id check, state file
   `Bezel Sync/cloudsync_state.json` 0600), `chrome/browser/bezel/cloudsync/` (KeyedService,
   registered by patch 0011), chrome://cloudsync (`chrome/browser/ui/webui/bezel_cloudsync/`,
   patch 0012, also a "Sync via CloudSync" row in chrome://settings/people).
5. History and sessions limits: in the replicator (weekly `h/w<week>` records, 12 weeks,
   written every 4 h; `s/<device>` every 15 min; other records 60 s after a change).
6. Android. **Written, not compiled**: `chrome/browser/bezel/cloudsync/android/` (AIDL copies,
   `CloudSyncConnectorBridge.java`, JNI transport), patch 0013 (manifest `<queries>`, local
   sync enabled in `sync_service_factory.cc`, settings row opening chrome://cloudsync),
   0004 (Java target in `chrome_java`), 0009 (local sync on by default everywhere).
7. Windows: dropped. Windows is no longer a target; leftover Windows code paths are not
   maintained.
8. Passwords. **Written**: `bezel_password_mirror.cc` + `bezel_password_conversion.{h,cc}`
   (tests: `bezel_password_mirror_unittests`). Not a `PasswordStoreBackend`: the login DB
   stays the backend and the mirror keeps it in step with § 12 (ListChanges cursor,
   CAS, TouchLogin for fills, Chromium fields in `extra` as `bezel.*`). Mapping in
   `Bezel Sync/cloudsync_logins.json` (no passwords in it).

9. Android passwords. **Written**: `CloudSyncPasswordBackend`, `CloudSyncPasswordLink`,
   patch 0015 (see "Passwords on Android"). Unit tests pass on Linux; not run on a device.

## Open issues

- **Android passwords:** with Shard locked there is no autofill and saving fails (the
  prompt is lost); a pending-writes queue would need secrets kept somewhere. Passwords
  exported to CSV by the login-DB deprecation flow of upstream are not imported.
- **Android sync toggles:** per-type toggles in Android sync settings call
  `SetSelectedType` without an account and do nothing (`DUMP_WILL_BE_NOTREACHED` in DCHECK
  builds). Type choice should move to chrome://cloudsync.
- **Passkeys** (WEBAUTHN_CREDENTIAL) are not synced: § 12 has no passkeys.
- **Password mirror limits:** a password changed while the mirror is off without a date
  change is missed; a login deleted while the mirror is off comes back from Shard.
- **Watch gaps:** after a reconnect the replicator re-reads every record (up to 64 MiB);
  fine for a local daemon, but slow on Android if records are large.
- Not run yet: a real two-device test through cloudsyncd, Android builds on a device,
  `components_unittests` for the touched upstream tests (`data_type_unittest.cc`).
