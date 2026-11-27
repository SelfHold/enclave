# Contributing to Enclave

Enclave is a small personal project; issues and pull requests are welcome. Please read the
[README](README.md) first, and [docs/AUDIT.md](docs/AUDIT.md) and
[docs/DECISIONS.md](docs/DECISIONS.md) before changing what is removed or kept.

## Principles a change must keep

- No tracking: nothing goes to Google or third parties unless a feature the user sees needs it.
- Minimal resource use: remove or disable a subsystem rather than leave it idle.
- uBlock Origin stays built in on every platform, Android included.
- Google sign-in, search suggestions and page preloading stay.
- Dark only. UI uses the design tokens (`--hm-*` in WebUI, `bezel::k*` in
  `ui/color/bezel_sys_color_mixer.h`, `hm_*` colors on Android), no raw colors, and has a
  phone layout below 768 px.
- Targets are Linux and Android. Don't build for macOS or iOS yet, but don't close them off.
  Windows is not a target: don't extend the Windows leftovers.

## Rules for changes

- **`chromium/src` is never the source of truth.** Either edit files there and run
  `./bezel.py export` to rewrite the patches, or edit `src_overlay/` and re-run
  `./bezel.py apply`. Don't hand-edit `patches/bezel/*.patch`: `apply` recognizes an applied
  patch by reverse-applying it, so a patch edited outside the tree stops matching a patched
  tree.
- **`./bezel.py apply` must stay idempotent:** running it twice changes nothing.
- **A new patch** starts with `git diff -- <files> > patches/bezel/NNNN-name.patch` in
  `chromium/src` and a line in `patches/series`. Name it after what it does.
- **Keep patches small and prefer new files in `src_overlay/`**, so that moving to a new
  Chromium release is cheap. Rebranding strings is done by `apply` (`rebrand()`), not by a patch.
- **Check every Chromium symbol, GN arg, feature and switch against the pinned version**
  (`chromium_version.txt`) before using it.
- **Before setting a GN arg to false**, run `./bezel.py configure` for every platform: many
  args assert in unrelated targets (see docs/AUDIT.md).
- **Never commit Google API keys** or any other secret. Keys come from the environment.
- **uBlock Origin is Manifest V2.** If upstream Chromium removes MV2 support, that blocks a
  version bump; check it on each one. Don't edit `vendor/ublock/`: refresh it with
  `./bezel.py vendor`.
- **Connector contract:** `src_overlay/components/bezel_cloudsync/proto/connector.proto` and
  the AIDL files are copies of Shard's
  ([Connector guide](https://github.com/SelfHold/shard/blob/main/docs/CONNECTOR.md)). Change
  them in Shard first, then copy them here unchanged. Enclave must keep working against an
  older Shard and without Shard at all.
- **Don't rename the internal `bezel` prefix** or persisted ids (the Connector app id
  `enclave`, the `Bezel Sync` profile directory, `bezel.*` keys in Shard's password store):
  they live in users' profiles and in Shard's data.
- After changing `branding/*.svg`, run `./bezel.py icons` and commit the rendered files.
- Record decisions that are not obvious from the code in docs/DECISIONS.md.

## Language and style

- English only in code, comments, identifiers, logs and docs. Other languages only in
  translations; non-English test data is fine.
- No personal paths, emails or local profile paths in committed files.

## Tests

```bash
make test     # unit tests of bezel.py, capped at 1 GB of memory
```

The C++ unit tests (`bezel_cloudsync_unittests`, `bezel_password_mirror_unittests`) are built
in the Chromium tree; see "Order of work" in [docs/CLOUDSYNC.md](docs/CLOUDSYNC.md). Run one
test job at a time with a memory cap. Sync behaviour is tested against the fake Connector
server and record store in `src_overlay/components/bezel_cloudsync/`, not a live Shard.

## License

By contributing you agree that your contribution is licensed under the GPLv3
(see [LICENSE](LICENSE)).
