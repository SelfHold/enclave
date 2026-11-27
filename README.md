# Enclave

Enclave is a personal Chromium distribution: Chromium without tracking, with uBlock Origin
built in, a dark theme of its own, and sync through [Shard](https://github.com/SelfHold/shard)
instead of Google's servers. It is built from source on top of a pinned Chromium release
(`chromium_version.txt`, currently 158.0.8127.88) by a small Python tool, a series of patches
and a tree of new files.

Enclave is part of [SelfHold](https://github.com/SelfHold), a set of independent, self-hosted
apps:

- [Shard](https://github.com/SelfHold/shard) (formerly CloudSync): serverless, encrypted file
  sync with a password safe. Its Connector API carries Enclave's sync and passwords.
- [Parley](https://github.com/SelfHold/parley): one client for chats, mail and calendars.
- [Postern](https://github.com/SelfHold/postern): SSH terminal and two-panel file manager.
- [Cellar](https://github.com/SelfHold/cellar): database manager.
- [Bezel](https://github.com/SelfHold/bezel): a GNOME/GTK theme in the same visual language.

**About the name "Bezel" in this repo.** "Bezel" was Enclave's working name and is still the
internal prefix here (`bezel.py`, `bezel_*` files, `patches/bezel/`, GN targets, C++ and Java
symbols). It has nothing to do with the separate Bezel theme repo. Users only see "Enclave".

Builds are not distributed: Enclave is built by whoever uses it.

## Principles

- **No tracking.** Nothing is sent to Google or third parties unless a feature the user sees
  needs it.
- **Minimal resource use.** A subsystem we don't use is removed or disabled, not left idle.
- **uBlock Origin built in** on every platform, Android included, as a component extension
  that cannot be removed and does not update from the store.
- **Kept on purpose:** Google account sign-in (for Google web services), search suggestions
  and page preloading. They are deliberate exceptions to "no tracking".
- **Dark only.** The browser UI, the Android UI and Enclave's own pages use one dark palette
  of design tokens; web pages see `prefers-color-scheme: dark`.

## What is removed or turned off

Checked against Chromium 158; details in [docs/AUDIT.md](docs/AUDIT.md), reasons in
[docs/DECISIONS.md](docs/DECISIONS.md).

- Already off in an unbranded build: UMA/UKM upload, crash upload, RLZ, the Google updater.
- Finch: variations seed fetch and bundled field trial configs.
- Gemini in Chrome (Glic), optimization guide hints, model downloads, server-side AI and page
  text extraction, the on-device model service.
- Local UKM and the ML ranking that feeds on it (segmentation platform), Topics and Attribution
  Reporting.
- Network time queries to Google, the Discover feed and NTP promos, autofill server votes.
- Remoting, WebXR, Network Error Logging / Reporting API, the Hangouts services extension.
- First-run defaults (desktop): Safe Browsing, hyperlink auditing (`<a ping>`), alternate
  error pages and the default-browser check are off; no first-run UI, no bookmark import.

Kept because sign-in and sync need them: DICE, bound session credentials, GCM. Enclave never
uses `--disable-background-networking`, which would also stop extension and sync traffic.

## Sync through Shard

Google's sync servers are not available to this build. Enclave keeps Chromium's own sync
engine and replaces only its server: Chromium's local sync backend runs on each device and its
entities are replicated through Shard's Connector API (Connector app id `enclave`). Bookmarks,
history, open tabs, settings and the other synced types go to Shard records; passwords go to
Shard's own password store. Sync is opt-in (chrome://cloudsync) and the browser works without
Shard. Design, record layout and status: [docs/CLOUDSYNC.md](docs/CLOUDSYNC.md); the API
contract: [Shard's Connector guide](https://github.com/SelfHold/shard/blob/main/docs/CONNECTOR.md).

## Status

Work in progress, used by its author.

- Linux: builds and runs. The sync code has unit tests; a two-device run through Shard is
  still to do.
- Android: the code is written and the first builds are in progress; not run on a device yet.
- See "Open issues" in [docs/CLOUDSYNC.md](docs/CLOUDSYNC.md).

## Platforms

- **Linux** (x86-64) and **Android** (arm64). Both are built on a Linux host.
- macOS and iOS may come later.
- Windows is not a target. Some Windows leftovers remain in the tooling
  (`config/gn/windows.gn`, `make build-windows`); Windows builds are not maintained.

## Requirements

- A Linux x86-64 host that meets
  [Chromium's own build requirements](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md)
  (and, for Android,
  [the Android ones](https://chromium.googlesource.com/chromium/src/+/main/docs/android_build_instructions.md)):
  plenty of RAM and CPU time, and Chromium's build dependencies
  (`chromium/src/build/install-build-deps.sh`, with `--android` for Android).
- **Disk space:** `./bezel.py fetch` refuses to start with less than **50 GB** free and stops
  when free space falls below 15 GB (rerun it to resume). Builds need a lot more on top of
  that.
- Python 3.10 or newer and git. `depot_tools` is cloned into `chromium/depot_tools` by
  `fetch` and put on `PATH` by `bezel.py`.
- Only to re-render the icons: `rsvg-convert` (librsvg) and ImageMagick 7 (`magick`).
- Optional: Google API keys for Google sign-in, passed through the environment
  (`GOOGLE_API_KEY`, `GOOGLE_DEFAULT_CLIENT_ID`, `GOOGLE_DEFAULT_CLIENT_SECRET`). They are
  never stored in this repo.

## Building from source

```bash
git clone https://github.com/SelfHold/enclave.git
cd enclave
./bezel.py fetch          # Chromium at the pinned tag into chromium/src (50+ GB)
make build-linux          # apply + build -> builds/linux/, installs the launcher and icons
make build-apk            # apply + build -> builds/android/*.apk
```

`make build-linux` runs `./bezel.py desktop` at the end, which installs `enclave.desktop` and
the icons into `~/.local/share` (or `$XDG_DATA_HOME`). The browser is `builds/linux/chrome`.

The steps one by one:

```bash
./bezel.py apply                    # overlay + uBlock manifest/zip + patches + rebranding
./bezel.py configure linux|android  # args.gn from config/gn, then gn gen (out/Bezel-<platform>)
./bezel.py build linux|android      # autoninja chrome / chrome_public_apk
./bezel.py desktop                  # Linux launcher + icons
./bezel.py icons                    # render branding/*.svg into src_overlay/
./bezel.py vendor [--source DIR]    # re-copy uBlock Origin (see below)
make test                           # unit tests of bezel.py
```

## Repository layout

| Path | What it is |
|---|---|
| `bezel.py` | Build tool: fetch, vendor, apply, export, configure, build, icons, desktop |
| `Makefile` | `build-linux`, `build-apk`, `apply`, `test`, `clean-builds` |
| `chromium_version.txt` | The pinned Chromium tag |
| `patches/series`, `patches/bezel/` | Changes to existing Chromium files, in apply order |
| `src_overlay/` | New files, copied into the Chromium tree at the same relative paths |
| `config/gn/` | GN args: `common.gn` plus one file per platform |
| `config/initial_preferences.json` | First-run defaults (desktop) |
| `branding/` | Icon sources (SVG); rendered icons are committed under `src_overlay/` |
| `vendor/ublock/` | uBlock Origin, vendored (third-party, not edited) |
| `tests/` | Unit tests of `bezel.py` |
| `docs/` | Audit, decisions, sync design |
| `chromium/`, `builds/` | Upstream checkout and finished builds; git-ignored |

## How patches work

- `./bezel.py apply` copies `src_overlay/` into `chromium/src`, writes the uBlock manifest and
  the Android uBlock zip, applies every patch listed in `patches/series` in order and renames
  the product to Enclave in the branded strings (with their translations). It is idempotent: a
  patch that already applies in reverse is skipped, and unchanged overlay files are left alone.
- `chromium/src` is never the source of truth. After editing files there, `./bezel.py export`
  rewrites every patch from `git diff` of the files that patch already touches.
- A new patch starts with `git diff -- <files> > patches/bezel/NNNN-name.patch` in
  `chromium/src` and a line in `patches/series`.
- New code goes into `src_overlay/` whenever possible; patches stay small so that moving to a
  new Chromium release is cheap.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the rules for changes.

## uBlock Origin

`vendor/ublock/` holds uBlock Origin 1.74.0, the Manifest V2 build, as installed from the
Chrome Web Store (its `_metadata` folder dropped and `update_url` removed from the manifest,
so it never updates from the store). `./bezel.py vendor` refreshes it from an installed copy;
the default source is the Chrome profile under `~/.config/google-chrome/Default/Extensions/`,
and `--source DIR` points it elsewhere. On desktop it ships next to the binary and loads as a
component extension; on Android it is an asset of the APK.

## License

Enclave's own files (build tooling, patches, overlay, configs, branding, docs) are licensed
under the GNU General Public License v3.0; see [LICENSE](LICENSE).

- Chromium is under its own BSD-style license (`chromium/src/LICENSE` in the checkout) and
  third-party licenses of its dependencies. Enclave does not include Chromium's source; the
  patches apply to it.
- uBlock Origin, vendored in `vendor/ublock/`, is under the GPLv3
  ([vendor/ublock/LICENSE.txt](vendor/ublock/LICENSE.txt)), © Raymond Hill and contributors.

Builds are not distributed. If you distribute one, the license obligations of Chromium, its
dependencies, uBlock Origin and Enclave are yours to meet. Codecs are built with
`proprietary_codecs = true`, which may need patent licenses in some countries.

Security issues: see [SECURITY.md](SECURITY.md).
