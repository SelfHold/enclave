# Contributing to Enclave

Enclave is a small personal project; issues and pull requests are welcome. Please read the
[README](README.md) first, and [docs/AUDIT.md](docs/AUDIT.md) and
[docs/DECISIONS.md](docs/DECISIONS.md) before changing what is removed or kept.

## Principles a change must keep

- No tracking: nothing goes to Google or third parties unless a feature the user sees needs it.
- Minimal resource use: remove or disable a subsystem rather than leave it idle.
- uBlock Origin stays built in on every platform, Android included.
- Google sign-in, search suggestions and page preloading stay.
- Dark only. UI uses the design tokens (`--hm-*` in WebUI, `enclave::k*` in
  `ui/color/enclave_sys_color_mixer.h`, `hm_*` colors on Android), no raw colors, and has a
  phone layout below 768 px.
- Targets are Linux and Android. Don't build for macOS or iOS yet, but don't close them off.
  Windows is not a target: don't extend the Windows leftovers.

## Rules for changes

- **`chromium/src` is never the source of truth.** Either edit files there and run
  `./enclave.py export` to rewrite the patches, or edit `src_overlay/` and re-run
  `./enclave.py apply`. Don't hand-edit `patches/enclave/*.patch`: `apply` recognizes an applied
  patch by reverse-applying it, so a patch edited outside the tree stops matching a patched
  tree.
- **`./enclave.py apply` must stay idempotent:** running it twice changes nothing.
- **A new patch** starts with `git diff -- <files> > patches/enclave/NNNN-name.patch` in
  `chromium/src` and a line in `patches/series`. Name it after what it does.
- **Keep patches small and prefer new files in `src_overlay/`**, so that moving to a new
  Chromium release is cheap. Rebranding strings is done by `apply` (`rebrand()`), not by a patch.
- **Check every Chromium symbol, GN arg, feature and switch against the pinned version**
  (`chromium_version.txt`) before using it.
- **Before setting a GN arg to false**, run `./enclave.py configure` for every platform: many
  args assert in unrelated targets (see docs/AUDIT.md).
- **Never commit Google API keys** or any other secret. Keys come from the environment.
- **uBlock Origin is Manifest V2.** If upstream Chromium removes MV2 support, that blocks a
  version bump; check it on each one. Don't edit `vendor/ublock/`: refresh it with
  `./enclave.py vendor`.
- **Connector contract:** `src_overlay/components/enclave_shard/proto/connector.proto` and
  the AIDL files are copies of Shard's
  ([Connector guide](https://github.com/SelfHold/shard/blob/main/docs/CONNECTOR.md)). Change
  them in Shard first, then copy them here unchanged. Enclave must keep working against an
  older Shard and without Shard at all.
- **Don't rename persisted ids** (the Connector app id `enclave`, the `Enclave Sync` profile
  directory, `enclave.*` keys in Shard's password store, the HKDF labels): they live in
  users' profiles and in Shard's data.
- After changing `branding/*.svg`, run `./enclave.py icons` and commit the rendered files.
- Record decisions that are not obvious from the code in docs/DECISIONS.md.

## Language and style

- English only in code, comments, identifiers, logs and docs. Other languages only in
  translations; non-English test data is fine.
- No personal paths, emails or local profile paths in committed files.

## Tests

```bash
make test     # unit tests of enclave.py, capped at 1 GB of memory
```

The C++ unit tests (`enclave_shard_unittests`, `enclave_password_mirror_unittests`) are built
in the Chromium tree; see "Order of work" in [docs/SHARD.md](docs/SHARD.md). Run one
test job at a time with a memory cap. Sync behaviour is tested against the fake Connector
server and record store in `src_overlay/components/enclave_shard/`, not a live Shard.

## License

By contributing you agree that your contribution is licensed under the GPLv3
(see [LICENSE](LICENSE)).
