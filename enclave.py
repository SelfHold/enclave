#!/usr/bin/env python3
"""Build tooling for Enclave, a private and lightweight Chromium distribution."""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
VERSION = (ROOT / "chromium_version.txt").read_text().strip()
SRC = Path(os.environ.get("ENCLAVE_SRC", ROOT / "chromium" / "src"))
VENDOR = ROOT / "vendor" / "ublock"
OVERLAY = ROOT / "src_overlay"
PATCHES = ROOT / "patches"
GN_CONFIG = ROOT / "config" / "gn"
UBLOCK_ID = "cjpalhdlnbpafiamejdnhcphjbkeiagm"
UBLOCK_DEFAULT = (
    Path.home() / ".config/google-chrome/Default/Extensions" / UBLOCK_ID / "1.74.0_0"
)
MANIFEST_INC = "chrome/browser/extensions/enclave_ublock_manifest.inc"
UBLOCK_APK_ZIP = "chrome/android/enclave/enclave_ublock.zip"
PLATFORMS = ("linux", "windows", "android")
BRANDING = ROOT / "branding"
THEME = "chrome/app/theme"
ANDROID_RES = "chrome/android/java/res_chromium_base"
ANDROID_DENSITIES = {"mdpi": 1, "hdpi": 1.5, "xhdpi": 2, "xxhdpi": 3, "xxxhdpi": 4}
PRODUCT = "Enclave"
# Linux .desktop id, Wayland app_id, WM_CLASS and icon name (patch 0016).
DESKTOP_ID = "enclave"
# "Chromium" as the product name. Kept: "The Chromium Authors", the link to the Chromium
# project on the About page, ChromiumOS, and ids such as org.chromium.Chromium.
PRODUCT_NAME_RE = re.compile(
    r'(?<![A-Za-z.])(?<!The )Chromium(?![A-Za-z])(?!\s*<ph name="END_LINK_CHROMIUM")'
)
BRANDED_FILES = ("chrome/app/theme/chromium/BRANDING", f"{ANDROID_RES}/values/channel_constants.xml")
BRANDED_GRDS = ("chrome/app/chromium_strings.grd", "components/components_chromium_strings.grd")
TRANSLATION_RE = re.compile(r'<translation id="(\d+)">(.*?)</translation>', re.S)
DEPOT_TOOLS = ROOT / "chromium" / "depot_tools"
FETCH_MIN_FREE_GB = 50
FETCH_STOP_FREE_GB = 15
KEY_ARGS = {
    "GOOGLE_API_KEY": "google_api_key",
    "GOOGLE_DEFAULT_CLIENT_ID": "google_default_client_id",
    "GOOGLE_DEFAULT_CLIENT_SECRET": "google_default_client_secret",
}


def run(*cmd, cwd=None, check=True):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], cwd=cwd, check=check)


def sanitize_manifest(manifest):
    """Removes store updates: the bundled copy changes only with an Enclave release."""
    clean = dict(manifest)
    clean.pop("update_url", None)
    if "key" not in clean:
        raise ValueError("a component extension manifest needs a 'key'")
    if clean.get("manifest_version") != 2:
        raise ValueError("expected the Manifest V2 build of uBlock Origin")
    return clean


def manifest_inc(manifest):
    body = json.dumps(manifest, ensure_ascii=False, separators=(",", ":"))
    if ')ENCLAVEJSON"' in body:
        raise ValueError("manifest contains the raw string delimiter")
    return f'R"ENCLAVEJSON({body})ENCLAVEJSON"\n'


def vendor_ublock(source):
    source = Path(source)
    manifest = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
    clean = sanitize_manifest(manifest)
    if VENDOR.exists():
        shutil.rmtree(VENDOR)
    shutil.copytree(source, VENDOR, ignore=shutil.ignore_patterns("_metadata"))
    (VENDOR / "manifest.json").write_text(
        json.dumps(clean, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Vendored uBlock Origin {clean['version']} to {VENDOR}")


def render_args(platform, env=None):
    env = os.environ if env is None else env
    parts = [(GN_CONFIG / "common.gn").read_text(), (GN_CONFIG / f"{platform}.gn").read_text()]
    for variable, arg in KEY_ARGS.items():
        value = env.get(variable)
        if value:
            if not re.fullmatch(r"[A-Za-z0-9._~-]+", value):
                raise ValueError(f"{variable} has unsupported characters")
            parts.append(f'{arg} = "{value}"\n')
    return "".join(parts)


def require_src():
    if not (SRC / "chrome" / "VERSION").exists():
        sys.exit(f"Chromium source not found at {SRC}. Run: ./enclave.py fetch")


def free_gb(path):
    return shutil.disk_usage(path).free / 1024**3


def gclient_config(version=VERSION):
    solution = {
        "name": "src",
        "url": f"https://chromium.googlesource.com/chromium/src.git@refs/tags/{version}",
        "managed": False,
        "custom_deps": {},
        "custom_vars": {"checkout_pgo_profiles": False},
    }
    return f"solutions = [{solution!r}]\ntarget_os = ['linux', 'android']\n"


def cmd_fetch(_args):
    base = SRC.parent
    base.mkdir(parents=True, exist_ok=True)
    if free_gb(base) < FETCH_MIN_FREE_GB:
        sys.exit(f"Need {FETCH_MIN_FREE_GB} GB free at {base}, have {free_gb(base):.1f} GB.")
    if not (DEPOT_TOOLS / "gclient").exists():
        run(
            "git", "clone", "--depth=1",
            "https://chromium.googlesource.com/chromium/tools/depot_tools.git", DEPOT_TOOLS,
        )
    (base / ".gclient").write_text(gclient_config())
    print("+ gclient sync --no-history", flush=True)
    sync = subprocess.Popen(["gclient", "sync", "--no-history"], cwd=base)
    while True:
        try:
            code = sync.wait(timeout=15)
            break
        except subprocess.TimeoutExpired:
            if free_gb(base) < FETCH_STOP_FREE_GB:
                sync.terminate()
                sync.wait()
                sys.exit(f"Stopped: less than {FETCH_STOP_FREE_GB} GB left. Rerun to resume.")
    if code:
        sys.exit(code)


def series():
    lines = (PATCHES / "series").read_text().splitlines()
    return [PATCHES / l.strip() for l in lines if l.strip() and not l.startswith("#")]


def apply_patches(src):
    for patch in series():
        reverse = subprocess.run(
            ["git", "apply", "--reverse", "--check", str(patch)], cwd=src, capture_output=True
        )
        if reverse.returncode == 0:
            print(f"Already applied: {patch.name}")
            continue
        run("git", "apply", patch, cwd=src)


def copy_overlay(src, overlay=OVERLAY):
    """Copies changed overlay files. A copy gets the current mtime (not the
    overlay's), so ninja rebuilds even when the tree's copy was newer; files
    that are already identical are left alone and cause no rebuild."""
    for file in overlay.rglob("*"):
        if file.is_file():
            target = Path(src) / file.relative_to(overlay)
            if target.is_file() and target.read_bytes() == file.read_bytes():
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(file, target)
            shutil.copymode(file, target)


def write_manifest_inc(src):
    manifest = json.loads((VENDOR / "manifest.json").read_text(encoding="utf-8"))
    (Path(src) / MANIFEST_INC).write_text(manifest_inc(manifest), encoding="utf-8")


def write_ublock_zip(source, target):
    """Writes a reproducible zip: same input, same bytes (no timestamps)."""
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    files = sorted(p for p in Path(source).rglob("*") if p.is_file())
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for file in files:
            info = zipfile.ZipInfo(file.relative_to(source).as_posix(), (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, file.read_bytes())


def rename_product(text):
    return PRODUCT_NAME_RE.sub(PRODUCT, text)


def grit_message_ids(src, grd):
    """Translation ids of every message in a .grd and its parts, in file order (all <if>
    branches)."""
    grit = str(Path(src) / "tools" / "grit")
    if grit not in sys.path:
        sys.path.insert(0, grit)
    from grit import grd_reader
    from grit.node import message

    root = grd_reader.Parse(str(grd), str(Path(grd).parent), skip_validation_checks=True)
    return [n.GetCliques()[0].GetId() for n in root.Preorder() if isinstance(n, message.MessageNode)]


def rebrand_xtb(text, renamed):
    """Moves translations to their new message ids and renames the product inside them."""

    def translation(match):
        new_id = renamed.get(match.group(1))
        if new_id is None:
            return match.group(0)
        return f'<translation id="{new_id}">{rename_product(match.group(2))}</translation>'

    return TRANSLATION_RE.sub(translation, text)


def rebrand_grd(src, grd):
    """Renames the product in a .grd, its .grdp parts and its .xtb files. A translation id is
    a hash of the English text, so without the id rewrite every renamed string would lose its
    translations."""
    path = Path(src) / grd
    base = path.parent
    parts = re.findall(r'<part file="([^"]+)"', path.read_text(encoding="utf-8"))
    sources = [base / part for part in parts] + [path]
    old = {file: file.read_text(encoding="utf-8") for file in sources}
    new = {file: rename_product(text) for file, text in old.items()}
    if new == old:
        return
    # New ids come from a renamed copy, so the tree is untouched until the .xtb files are done.
    with tempfile.TemporaryDirectory() as tmp:
        for file, text in new.items():
            copy = Path(tmp) / file.relative_to(base)
            copy.parent.mkdir(parents=True, exist_ok=True)
            copy.write_text(text, encoding="utf-8")
        new_ids = grit_message_ids(src, Path(tmp) / path.name)
    old_ids = grit_message_ids(src, path)
    renamed = {o: n for o, n in zip(old_ids, new_ids, strict=True) if o != n}
    for xtb in re.findall(r'<file path="([^"]+\.xtb)"', old[path]):
        xtb_path = base / xtb
        xtb_path.write_text(
            rebrand_xtb(xtb_path.read_text(encoding="utf-8"), renamed), encoding="utf-8"
        )
    # The .grd goes last: an interrupted run sees the old text and redoes the rest.
    for file in sources:
        file.write_text(new[file], encoding="utf-8")


def rebrand(src):
    for name in BRANDED_FILES:
        path = Path(src) / name
        path.write_text(rename_product(path.read_text(encoding="utf-8")), encoding="utf-8")
    for grd in BRANDED_GRDS:
        rebrand_grd(src, grd)


def patch_files(patch):
    return re.findall(r"^diff --git a/(\S+) b/", Path(patch).read_text(), re.M)


def cmd_export(_args):
    """Rewrites each patch from the current tree, keeping its list of files."""
    require_src()
    for patch in series():
        files = patch_files(patch)
        diff = subprocess.run(
            ["git", "diff", "--", *files], cwd=SRC, check=True, capture_output=True, text=True
        ).stdout
        if not diff:
            sys.exit(f"{patch.name}: no changes in {files}; is the tree patched?")
        patch.write_text(diff)
        print(f"Exported {patch.name} ({len(files)} files)")


def cmd_vendor(args):
    vendor_ublock(args.source)


def icon_targets():
    """Maps each upstream icon path to (source svg, size in px, padding in px)."""
    def desktop(size, pad=0):
        source = "enclave_icon_16.svg" if size - 2 * pad <= 16 else "enclave_icon.svg"
        return (source, size, pad)

    targets = {}
    for size in (16, 24, 48, 64, 128, 256):
        targets[f"{THEME}/chromium/product_logo_{size}.png"] = desktop(size)
    for size in (24, 48, 64, 128, 256):
        targets[f"{THEME}/chromium/linux/product_logo_{size}.png"] = desktop(size)
    targets[f"{THEME}/chromium/linux/product_logo_32.xpm"] = desktop(32)
    for scale, folder in ((1, "default_100_percent"), (2, "default_200_percent")):
        for size in (16, 32):
            targets[f"{THEME}/{folder}/chromium/product_logo_{size}.png"] = desktop(size * scale)
    for size in (16, 32):
        targets[f"{THEME}/default_100_percent/chromium/linux/product_logo_{size}.png"] = (
            desktop(size)
        )
    for name in ("chromium", "app_list"):
        targets[f"{THEME}/chromium/win/{name}.ico"] = [desktop(s) for s in (16, 32, 48, 256)]
    targets[f"{THEME}/chromium/win/tiles/Logo.png"] = desktop(600, 190)
    targets[f"{THEME}/chromium/win/tiles/SmallLogo.png"] = desktop(176, 29)
    targets[f"{THEME}/chromium/product_logo_22_mono.png"] = ("enclave_icon_mono.svg", 22, 3)
    for density, scale in ANDROID_DENSITIES.items():
        folder = f"{ANDROID_RES}/mipmap-{density}"
        targets[f"{folder}/app_icon.png"] = desktop(round(48 * scale), round(2 * scale))
        targets[f"{folder}/layered_app_icon.png"] = ("enclave_icon_android.svg", round(108 * scale), 0)
        targets[f"{folder}/layered_app_icon_background.png"] = (
            "enclave_icon_android_background.svg", round(108 * scale), 0
        )
    return targets


def svg_env():
    """rsvg-convert crashes on the libraries a snap (VS Code) puts into the environment."""
    return {k: v for k, v in os.environ.items() if "/snap/" not in v}


def render_png(source, size, pad, target):
    inner = size - 2 * pad
    png = subprocess.run(
        ["rsvg-convert", "-w", str(inner), "-h", str(inner), str(BRANDING / source)],
        check=True, capture_output=True, env=svg_env(),
    ).stdout
    subprocess.run(
        ["magick", "png:-", "-background", "none", "-gravity", "center",
         "-extent", f"{size}x{size}", "-strip", "-define", "png:exclude-chunks=date,time",
         str(target)],
        input=png, check=True, env=svg_env(),
    )


def cmd_icons(_args):
    """Renders branding/*.svg into src_overlay at the upstream icon paths."""
    for path, spec in icon_targets().items():
        target = OVERLAY / path
        target.parent.mkdir(parents=True, exist_ok=True)
        print("icon", path, flush=True)
        if isinstance(spec, list):
            frames = [target.with_name(f"{target.stem}_{s[1]}.png") for s in spec]
            for frame, (source, size, pad) in zip(frames, spec):
                render_png(source, size, pad, frame)
            subprocess.run(["magick", *map(str, frames), str(target)], check=True, env=svg_env())
            for frame in frames:
                frame.unlink()
        elif target.suffix == ".xpm":
            frame = target.with_suffix(".png")
            render_png(*spec, frame)
            subprocess.run(["magick", str(frame), str(target)], check=True, env=svg_env())
            frame.unlink()
        else:
            render_png(*spec, target)
    shutil.copy2(BRANDING / "enclave_icon.svg", OVERLAY / THEME / "chromium/product_logo.svg")


def cmd_apply(_args):
    require_src()
    if not (VENDOR / "manifest.json").exists():
        sys.exit("uBlock Origin is not vendored. Run: ./enclave.py vendor")
    copy_overlay(SRC)
    write_manifest_inc(SRC)
    write_ublock_zip(VENDOR, SRC / UBLOCK_APK_ZIP)
    apply_patches(SRC)
    rebrand(SRC)


def out_dir(platform):
    return SRC / "out" / f"Enclave-{platform}"


def cmd_configure(args):
    require_src()
    out = out_dir(args.platform)
    out.mkdir(parents=True, exist_ok=True)
    (out / "args.gn").write_text(render_args(args.platform))
    run("gn", "gen", out.relative_to(SRC), cwd=SRC)


def stage_desktop(out):
    resources = out / "resources" / "enclave_ublock"
    if resources.exists():
        shutil.rmtree(resources)
    shutil.copytree(VENDOR, resources)
    shutil.copy2(ROOT / "config" / "initial_preferences.json", out / "initial_preferences")


def cmd_build(args):
    require_src()
    out = out_dir(args.platform)
    if not (out / "build.ninja").exists():
        cmd_configure(args)
    target = "chrome_public_apk" if args.platform == "android" else "chrome"
    run("autoninja", "-C", out.relative_to(SRC), target, cwd=SRC)
    if args.platform != "android":
        stage_desktop(out)


def desktop_entry(binary):
    """The launcher GNOME/KDE match to the window by app_id (Wayland) or WM_CLASS (X11).
    Both are "enclave" (patch 0016); CHROME_DESKTOP gives builds without it the same id."""
    return "\n".join([
        "[Desktop Entry]",
        "Version=1.0",
        "Type=Application",
        f"Name={PRODUCT}",
        "GenericName=Web Browser",
        f"Exec=env CHROME_DESKTOP={DESKTOP_ID}.desktop {binary} %U",
        f"Icon={DESKTOP_ID}",
        f"StartupWMClass={DESKTOP_ID}",
        "StartupNotify=true",
        "Terminal=false",
        "Categories=Network;WebBrowser;",
        "MimeType=text/html;application/xhtml+xml;x-scheme-handler/http;x-scheme-handler/https;",
        "Actions=new-window;new-private-window;",
        "",
        "[Desktop Action new-window]",
        "Name=New Window",
        f"Exec=env CHROME_DESKTOP={DESKTOP_ID}.desktop {binary}",
        "",
        "[Desktop Action new-private-window]",
        "Name=New Incognito Window",
        f"Exec=env CHROME_DESKTOP={DESKTOP_ID}.desktop {binary} --incognito",
        "",
    ])


def cmd_desktop(args):
    """Installs the launcher and icons for builds/linux into the user's XDG data dir."""
    binary = Path(args.binary).resolve()
    if not binary.is_file():
        sys.exit(f"{binary} not found. Run: make build-linux")
    data = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    icons = data / "icons" / "hicolor"
    linux_logos = OVERLAY / THEME / "chromium" / "linux"
    for png in sorted(linux_logos.glob("product_logo_*.png")):
        size = png.stem.rsplit("_", 1)[1]
        target = icons / f"{size}x{size}" / "apps" / f"{DESKTOP_ID}.png"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(png, target)
    scalable = icons / "scalable" / "apps" / f"{DESKTOP_ID}.svg"
    scalable.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(BRANDING / "enclave_icon.svg", scalable)
    entry = data / "applications" / f"{DESKTOP_ID}.desktop"
    entry.parent.mkdir(parents=True, exist_ok=True)
    entry.write_text(desktop_entry(binary))
    tools = [["update-desktop-database", entry.parent]]
    if (icons / "index.theme").is_file():
        tools.append(["gtk-update-icon-cache", "-q", icons])
    for tool in tools:
        if shutil.which(tool[0]):
            run(*tool, check=False)
    print(f"Installed {entry}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(required=True)
    commands.add_parser("fetch", help="download Chromium").set_defaults(func=cmd_fetch)
    vendor = commands.add_parser("vendor", help="copy the installed uBlock Origin")
    vendor.add_argument("--source", default=UBLOCK_DEFAULT)
    vendor.set_defaults(func=cmd_vendor)
    commands.add_parser("apply", help="apply Enclave to the source").set_defaults(func=cmd_apply)
    commands.add_parser("export", help="rewrite patches from the source").set_defaults(
        func=cmd_export
    )
    commands.add_parser("icons", help="render branding/ into src_overlay").set_defaults(
        func=cmd_icons
    )
    desktop = commands.add_parser("desktop", help="install the Linux launcher and icons")
    desktop.add_argument("--binary", default=ROOT / "builds" / "linux" / "chrome")
    desktop.set_defaults(func=cmd_desktop)
    for name, func in (("configure", cmd_configure), ("build", cmd_build)):
        command = commands.add_parser(name)
        command.add_argument("platform", choices=PLATFORMS)
        command.set_defaults(func=func)
    args = parser.parse_args()
    os.environ["PATH"] = f"{DEPOT_TOOLS}{os.pathsep}{os.environ['PATH']}"
    args.func(args)


if __name__ == "__main__":
    main()
