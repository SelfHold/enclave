import importlib.util
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("enclave", ROOT / "enclave.py")
enclave = importlib.util.module_from_spec(spec)
sys.modules["enclave"] = enclave
spec.loader.exec_module(enclave)


class ManifestTest(unittest.TestCase):
    def test_removes_store_updates(self):
        clean = enclave.sanitize_manifest(
            {"key": "k", "manifest_version": 2, "update_url": "https://example.test"}
        )
        self.assertNotIn("update_url", clean)

    def test_requires_key_and_mv2(self):
        with self.assertRaises(ValueError):
            enclave.sanitize_manifest({"manifest_version": 2})
        with self.assertRaises(ValueError):
            enclave.sanitize_manifest({"key": "k", "manifest_version": 3})

    def test_inc_is_a_raw_string_with_json(self):
        text = enclave.manifest_inc({"name": "uBlock", "key": "k"}).strip()
        prefix, suffix = 'R"ENCLAVEJSON(', ')ENCLAVEJSON"'
        self.assertTrue(text.startswith(prefix) and text.endswith(suffix))
        self.assertEqual(json.loads(text[len(prefix) : -len(suffix)])["name"], "uBlock")


class ZipTest(unittest.TestCase):
    def test_zip_is_reproducible_and_complete(self):
        import tempfile, zipfile

        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp, "src")
            (src / "js").mkdir(parents=True)
            (src / "manifest.json").write_text("{}")
            (src / "js" / "a.js").write_text("x")
            one, two = Path(tmp, "1.zip"), Path(tmp, "2.zip")
            enclave.write_ublock_zip(src, one)
            enclave.write_ublock_zip(src, two)
            self.assertEqual(one.read_bytes(), two.read_bytes())
            self.assertEqual(
                zipfile.ZipFile(one).namelist(), ["js/a.js", "manifest.json"]
            )


class OverlayTest(unittest.TestCase):
    def test_changed_files_get_a_new_mtime_and_same_files_stay(self):
        import os, tempfile

        with tempfile.TemporaryDirectory() as tmp:
            overlay, src = Path(tmp, "overlay"), Path(tmp, "src")
            (overlay / "a").mkdir(parents=True)
            (overlay / "a" / "x.cc").write_text("new")
            (overlay / "a" / "same.cc").write_text("same")
            os.utime(overlay / "a" / "x.cc", (1000, 1000))
            (src / "a").mkdir(parents=True)
            (src / "a" / "x.cc").write_text("edited in the tree")
            (src / "a" / "same.cc").write_text("same")
            os.utime(src / "a" / "same.cc", (2000, 2000))

            enclave.copy_overlay(src, overlay)

            self.assertEqual((src / "a" / "x.cc").read_text(), "new")
            self.assertGreater((src / "a" / "x.cc").stat().st_mtime, 1000)
            self.assertEqual((src / "a" / "same.cc").stat().st_mtime, 2000)


class ArgsTest(unittest.TestCase):
    def test_every_platform_renders(self):
        for platform in enclave.PLATFORMS:
            self.assertIn("target_os", enclave.render_args(platform, {}))

    def test_keys_come_from_environment(self):
        args = enclave.render_args("linux", {"GOOGLE_API_KEY": "abc-123"})
        self.assertIn('google_api_key = "abc-123"', args)

    def test_rejects_gn_injection(self):
        with self.assertRaises(ValueError):
            enclave.render_args("linux", {"GOOGLE_API_KEY": 'x"\nis_debug = true'})


class ConfigTest(unittest.TestCase):
    def test_initial_preferences_are_valid(self):
        prefs = json.loads((ROOT / "config/initial_preferences.json").read_text())
        self.assertFalse(prefs["enable_a_ping"])

    def test_gclient_pins_tag_and_android(self):
        scope = {}
        exec(enclave.gclient_config("1.2.3.4"), scope)
        self.assertTrue(scope["solutions"][0]["url"].endswith("@refs/tags/1.2.3.4"))
        self.assertIn("android", scope["target_os"])

    def test_series_files_exist(self):
        for patch in enclave.series():
            self.assertTrue(patch.is_file(), patch)

    def test_icons_are_rendered_into_overlay(self):
        for path, spec in enclave.icon_targets().items():
            self.assertTrue((enclave.OVERLAY / path).is_file(), path)
            for source, _size, _pad in spec if isinstance(spec, list) else [spec]:
                self.assertTrue((enclave.BRANDING / source).is_file(), source)


    def test_desktop_entry_matches_the_window_id(self):
        entry = enclave.desktop_entry(Path("/opt/enclave/chrome"))
        self.assertIn("Name=Enclave\n", entry)
        self.assertIn("Icon=enclave\n", entry)
        self.assertIn("StartupWMClass=enclave\n", entry)
        self.assertIn("Exec=env CHROME_DESKTOP=enclave.desktop /opt/enclave/chrome %U\n", entry)
        patch = (enclave.PATCHES / "enclave/0016-enclave-desktop-id.patch").read_text()
        self.assertIn('+  return "enclave.desktop";', patch)
        self.assertTrue(list((enclave.OVERLAY / enclave.THEME / "chromium/linux").glob("*.png")))

class BrandingTest(unittest.TestCase):
    def test_renames_only_the_product(self):
        cases = {
            "Relaunch Chromium?": "Relaunch Enclave?",
            "Chromium を再起動": "Enclave を再起動",
            "Chromium-Menü": "Enclave-Menü",
            "The Chromium Authors": "The Chromium Authors",
            "ChromiumOS": "ChromiumOS",
            "org.chromium.Chromium": "org.chromium.Chromium",
            'the <ph name="BEGIN"/>Chromium<ph name="END_LINK_CHROMIUM"/> project':
                'the <ph name="BEGIN"/>Chromium<ph name="END_LINK_CHROMIUM"/> project',
        }
        for before, after in cases.items():
            self.assertEqual(enclave.rename_product(before), after)

    def test_xtb_moves_only_renamed_messages(self):
        xtb = '<translation id="1">О Chromium</translation>\n<translation id="2">Chromium</translation>'
        self.assertEqual(
            enclave.rebrand_xtb(xtb, {"1": "9"}),
            '<translation id="9">О Enclave</translation>\n<translation id="2">Chromium</translation>',
        )

    @unittest.skipUnless((enclave.SRC / "tools/grit/grit").is_dir(), "no Chromium checkout")
    def test_grd_keeps_translations_and_is_idempotent(self):
        import tempfile

        grd = """<?xml version="1.0" encoding="UTF-8"?>
<grit latest_public_release="0" current_release="1" source_lang_id="en">
  <translations><file path="s_ru.xtb" lang="ru" /></translations>
  <release seq="1"><messages>
    <message name="IDS_A" desc="d">About Chromium</message>
    <part file="part.grdp" />
  </messages></release>
</grit>
"""
        part = """<?xml version="1.0" encoding="utf-8"?>
<grit-part><message name="IDS_B" desc="d">The Chromium Authors</message>
<message name="IDS_C" desc="d">Chromium help</message></grit-part>
"""
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "tools").mkdir()
            Path(tmp, "tools/grit").symlink_to(enclave.SRC / "tools/grit")
            Path(tmp, "s.grd").write_text(grd)
            Path(tmp, "part.grdp").write_text(part)
            old_ids = enclave.grit_message_ids(tmp, Path(tmp, "s.grd"))
            Path(tmp, "s_ru.xtb").write_text(
                "".join(
                    f'<translation id="{i}">{t}</translation>\n'
                    for i, t in zip(old_ids, ("О Chromium", "Авторы Chromium", "Справка Chromium"))
                )
            )
            enclave.rebrand_grd(tmp, "s.grd")
            once = Path(tmp, "s_ru.xtb").read_text()
            enclave.rebrand_grd(tmp, "s.grd")
            self.assertEqual(Path(tmp, "s_ru.xtb").read_text(), once)
            new_ids = enclave.grit_message_ids(tmp, Path(tmp, "s.grd"))
            self.assertEqual(new_ids[1], old_ids[1])
            self.assertEqual(
                once,
                f'<translation id="{new_ids[0]}">О Enclave</translation>\n'
                f'<translation id="{old_ids[1]}">Авторы Chromium</translation>\n'
                f'<translation id="{new_ids[2]}">Справка Enclave</translation>\n',
            )


if __name__ == "__main__":
    unittest.main()
