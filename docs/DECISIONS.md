# Decisions

## uBlock Origin stays an MV2 extension, not a rewrite in the engine (2026-11-24)

- Chromium 158 still loads MV2 and allows `webRequestBlocking` for MV2 extensions on
  every platform, including Android.
- uBlock has about 67k lines of its own JS plus WASM. A C++ rewrite would fork the filter
  engine and lose upstream filter-syntax updates.
- If phone RAM turns out to be a problem, move only network-request matching into the
  engine and keep the rest as the extension.

## Android extensions: `is_desktop_android = true` (2026-11-24)

- `enable_desktop_android_extensions` alone fails `gn gen`: WebUI components assert on
  `is_desktop_android`.
- Side effects on phones: WebUI NTP, native URL overriding and WebUI theming on by default
  (`ChromeFeatureList.java`), plus about 30 C++ sites. Revisit after the first device run.

## uBlock loading

- Desktop: files next to the binary in `resources/bezel_ublock`, loaded as a component
  extension (no store updates, cannot be removed).
- Android: one uncompressed `assets/bezel_ublock.zip` in the APK, unpacked once per version
  into app data on the extension file thread. A `.pak` does not work: extension locales are
  read from disk.

## Theme

- Palette overrides go into the `kColorSys*` tokens right after the ui sys mixers
  (`ui/color/bezel_sys_color_mixer.h`), so Views and WebUI derive from it. A later mixer
  cannot cascade: recipes resolve against the mixers before them.
- `force-dark-mode` at startup: Enclave's design is dark only. Web pages also see
  `prefers-color-scheme: dark`.

## Sync through Shard, on Chromium's own sync engine (2026-11-26)

- Google's sync servers are unavailable to this build. Chromium's local sync backend
  (`LoopbackServer`) runs on each device and its entities are replicated through the
  Connector API of [Shard](https://github.com/SelfHold/shard) (formerly CloudSync). All
  data-type logic and merging stays upstream.
- Passwords go to Shard's own password store, not to our records, through the
  `passwords` scope Shard added for us ([Connector guide](https://github.com/SelfHold/shard/blob/main/docs/CONNECTOR.md) § 12).
- Details and order of work: `docs/CLOUDSYNC.md`.

## Shard replication details (2026-11-26)

- Last writer wins per entity, by `(changed_ms, device)`, kept by the replicator (tombstones
  in LoopbackServer have no mtime). Records are rewritten only when their content hash
  changes, because every Shard write stays in the journals forever.
- History records are per week, so a flush rewrites one record, not every history shard.
- Passwords: a mirror between the login DB and Shard's password store instead of our
  own `PasswordStoreBackend`: far less code, and the login DB is the offline cache that Connector
  guide § 12.3 asks for anyway.
- chrome://cloudsync is a string-served WebUI page, not a settings page in TS/Lit: no grit
  resources, cheap to rebase.

## App icon (2026-11-26)

- A monitor bezel around a dark `--hm-bg` screen with the cyan grid, HUD corner brackets and a
  glowing `--hm-accent` dot, all from the dark-only design palette. Square, small radius: the
  design has no rounded "app" feel.
- Rendered files replace the upstream Chromium icons at their own paths through the overlay,
  so no patch and no GN change. 16 px uses a pixel-aligned variant without the grid.
- Android: the foreground layer is opaque (upstream `ic_launcher.xml` puts it on white), and
  the screen stays inside the 66 dp safe zone so every launcher mask keeps the bezel.
- Not replaced: document, PDF, incognito and web store icons, and macOS (not a target yet).
- Linux desktop identity (2026-11-26): GNOME/KDE on Wayland take the dock icon from the
  `.desktop` file whose id equals the window's `app_id`, not from the PNGs in the binary.
  Upstream sends `chromium-browser`, which has no desktop file here, so the dock showed a gear.
  Patch 0016 makes the default id `enclave` (app_id, WM_CLASS, fallback shortcut icon);
  `./bezel.py desktop` installs `enclave.desktop` and hicolor icons into `~/.local/share`.
  `CHROME_DESKTOP` in `Exec` keeps builds without the patch matching too.

## Name: Enclave (2026-11-26)

- Users see "Enclave": a browser that keeps to itself and pairs only with its own sync,
  Shard (formerly CloudSync).
- "Bezel" stays as the internal prefix (files, GN targets, the `Bezel Sync` profile directory):
  renaming it only costs rebase work and would move existing profile data.
- `./bezel.py apply` renames the product (`rebrand()`) instead of carrying a patch: a patch over
  `chromium_strings.grd`, its `.grdp` and 160+ `.xtb` files would conflict on every rebase.
  A translation id is a hash of the English text, so the step reads the old and new ids from grit
  and moves every translation to its new id. The share of untranslated strings stays as upstream.
- Kept as "Chromium": "The Chromium Authors" (copyright), the link to the Chromium project on the
  About page, ChromiumOS strings, `org.chromium.*` ids and the Android package name.
- Not renamed yet: the user data directory (`~/.config/chromium` on Linux). Changing it moves
  existing profiles. (The Linux app_id / WM_CLASS is `enclave` since patch 0016.)

## Profiles in Shard: one space per profile, matched by name (2026-11-26)

- The owner's choice: every browser profile syncs into its own space; profiles with the same
  name on different devices are one profile; passwords belong to a profile.
- The pairing is per device (Shard allows one per app and device), shared by profiles.
- Slots have their own ids, so a rename does not move data; a rename onto another slot's
  name merges the two.
- Untagged logins go to the oldest slot ("primary"): logins made in Shard's own UI
  must land somewhere, and only one profile may own them.
