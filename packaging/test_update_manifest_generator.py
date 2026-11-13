#!/usr/bin/env python3
"""Unit tests for the pure parts of packaging/generate-update-manifest.py.

Run with: python -I packaging/test_update_manifest_generator.py
dpkg-deb dependent tests are skipped when dpkg-deb is not installed (for
example on a Windows development host); they run in CI and in the local
Ubuntu 20.04 container verification.
"""

import datetime
import hashlib
import importlib.util
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

# The generator file name contains hyphens, so it cannot be imported as a
# normal module; load it by explicit path (also fine under python -I).
_GENERATOR_PATH = Path(__file__).resolve().parent / "generate-update-manifest.py"
_SPEC = importlib.util.spec_from_file_location("generate_update_manifest", _GENERATOR_PATH)
generator = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(generator)


class StrictVersionTest(unittest.TestCase):
    def test_accepts_three_components(self):
        for value in ("1.2.0", "0.0.1", "10.20.30", "1.0.0", "0.1.2"):
            self.assertTrue(generator.is_strict_version(value), value)

    def test_rejects_leading_zeros(self):
        for value in ("1.02.0", "01.2.3", "1.2.03", "0.01.0", "00.1.2"):
            self.assertFalse(generator.is_strict_version(value), value)

    def test_rejects_wrong_shape_or_junk(self):
        for value in ("1.2", "1.2.3.4", "v1.2.0", "1.2.3-rc1", "1.2.3+build", "",
                      " 1.2.0", "1..2", "1.2.x", "1.2.0\n"):
            self.assertFalse(generator.is_strict_version(value), value)

    def test_rejects_non_strings(self):
        for value in (None, 120, 1.2, ("1", "2", "0")):
            self.assertFalse(generator.is_strict_version(value), value)


class ParseProjectVersionTest(unittest.TestCase):
    CMAKE_SAMPLE = (
        "cmake_minimum_required(VERSION 3.16)\n"
        "\n"
        "project(kylinlist\n"
        "        VERSION 1.2.0\n"
        '        DESCRIPTION "KylinList local tasks"\n'
        "        LANGUAGES C CXX)\n"
        "\n"
        "if(WIN32)\n"
        "  set(KYLINLIST_TEST_ENV \"QT_QPA_PLATFORM=offscreen\")\n"
        "endif()\n"
    )

    def test_reads_project_version(self):
        self.assertEqual(
            generator.parse_project_version(self.CMAKE_SAMPLE), "1.2.0")

    def test_ignores_other_version_occurrences(self):
        text = "cmake_minimum_required(VERSION 3.16)\n" + self.CMAKE_SAMPLE
        self.assertEqual(generator.parse_project_version(text), "1.2.0")

    def test_rejects_non_strict_project_version(self):
        for version in ("1.02.0", "1.2", "1.2.3-rc1"):
            text = "project(kylinlist\n        VERSION {0}\n)\n".format(version)
            self.assertIsNone(generator.parse_project_version(text), version)

    def test_missing_version(self):
        self.assertIsNone(generator.parse_project_version("project(kylinlist)\n"))


class NamingTest(unittest.TestCase):
    def test_linux_deb_filenames(self):
        self.assertEqual(
            generator.linux_deb_filename("1.2.0", "arm64", ""),
            "kylinlist_1.2.0_arm64.deb")
        self.assertEqual(
            generator.linux_deb_filename("1.2.0", "amd64", "_bundle"),
            "kylinlist_1.2.0_amd64_bundle.deb")

    def test_windows_expected_files(self):
        self.assertEqual(
            generator.windows_expected_files("1.2.0"),
            {"setup": "KylinList-1.2.0-Windows-x64-Setup.exe",
             "portable": "KylinList-1.2.0-Windows-x64.zip"})

    def test_target_directory(self):
        self.assertEqual(
            generator.target_directory("linux", "arm64", "system-qt"),
            "linux-arm64-system-qt")
        self.assertEqual(
            generator.target_directory("windows", "amd64", "setup"),
            "windows-amd64-setup")

    def test_artifact_relative_path(self):
        self.assertEqual(
            generator.artifact_relative_path(
                "1.2.0", "linux", "arm64", "system-qt", "kylinlist_1.2.0_arm64.deb"),
            "files/1.2.0/linux-arm64-system-qt/kylinlist_1.2.0_arm64.deb")

    def test_expected_dist_names_full_and_partial(self):
        full = generator.expected_dist_names("1.2.0", True)
        self.assertEqual(len(full), 7)
        self.assertIn(generator.WINDOWS_SIDECAR_NAME, full)
        self.assertIn("KylinList-1.2.0-Windows-x64-Setup.exe", full)
        partial = generator.expected_dist_names("1.2.0", False)
        self.assertEqual(len(partial), 4)
        self.assertTrue(all(name.endswith(".deb") for name in partial))


class HashAndSizeTest(unittest.TestCase):
    def test_known_content(self):
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "artifact.bin"
            payload = b"KylinList update artifact bytes"
            path.write_bytes(payload)
            sha256, size = generator.file_sha256_and_size(path)
            self.assertEqual(sha256, hashlib.sha256(payload).hexdigest())
            self.assertEqual(size, len(payload))
            self.assertEqual(sha256, sha256.lower())
            self.assertEqual(len(sha256), 64)

    def test_empty_file(self):
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "empty.bin"
            path.write_bytes(b"")
            sha256, size = generator.file_sha256_and_size(path)
            self.assertEqual(sha256, hashlib.sha256(b"").hexdigest())
            self.assertEqual(size, 0)

    def test_large_content_streaming(self):
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "large.bin"
            payload = bytes(range(256)) * (1024 * 1024 // 256)  # 1 MiB
            path.write_bytes(payload)
            sha256, size = generator.file_sha256_and_size(path)
            self.assertEqual(sha256, hashlib.sha256(payload).hexdigest())
            self.assertEqual(size, len(payload))


class TimestampAndManifestTest(unittest.TestCase):
    def test_utc_timestamp_format(self):
        moment = datetime.datetime(
            2026, 11, 13, 8, 5, 2, tzinfo=datetime.timezone.utc)
        self.assertEqual(generator.utc_timestamp(moment), "2026-11-13T08:05:02Z")

    def test_build_manifest_structure(self):
        entry = {
            "platform": "linux", "arch": "arm64", "variant": "system-qt",
            "packageVersion": "1.2.0",
            "path": "files/1.2.0/linux-arm64-system-qt/kylinlist_1.2.0_arm64.deb",
            "size": 591548, "sha256": "a" * 64,
        }
        manifest = generator.build_manifest(
            "1.2.0", "更新说明", "2026-11-13T08:05:02Z", [entry])
        self.assertEqual(list(manifest), [
            "schemaVersion", "channel", "version", "publishedAt", "notes", "artifacts"])
        self.assertEqual(manifest["schemaVersion"], 1)
        self.assertEqual(manifest["channel"], "stable")
        self.assertEqual(manifest["version"], "1.2.0")
        self.assertEqual(manifest["publishedAt"], "2026-11-13T08:05:02Z")
        self.assertEqual(manifest["notes"], "更新说明")
        artifact = manifest["artifacts"][0]
        self.assertEqual(list(artifact), [
            "platform", "arch", "variant", "packageVersion", "path", "size", "sha256"])
        self.assertEqual(artifact, entry)
        # The exact example from the manifest protocol must round-trip.
        expected = {
            "schemaVersion": 1, "channel": "stable", "version": "1.2.0",
            "publishedAt": "2026-11-13T08:05:02Z", "notes": "更新说明",
            "artifacts": [dict(entry)],
        }
        self.assertEqual(manifest, expected)

    def test_default_notes(self):
        self.assertEqual(generator.load_notes(None, "1.2.0"), "KylinList 1.2.0。")

    def test_notes_file_overrides(self):
        with tempfile.TemporaryDirectory() as raw:
            notes = Path(raw) / "notes.txt"
            notes.write_text("修复若干问题。\n", encoding="utf-8")
            self.assertEqual(generator.load_notes(str(notes), "1.2.0"), "修复若干问题。")

    def test_empty_notes_file_rejected(self):
        with tempfile.TemporaryDirectory() as raw:
            notes = Path(raw) / "notes.txt"
            notes.write_text("\n", encoding="utf-8")
            with self.assertRaises(generator.UpdateError):
                generator.load_notes(str(notes), "1.2.0")


class DistFileCheckTest(unittest.TestCase):
    def test_require_dist_files_reports_all_missing(self):
        with tempfile.TemporaryDirectory() as raw:
            dist = Path(raw)
            (dist / "kylinlist_1.2.0_arm64.deb").write_bytes(b"deb")
            with self.assertRaises(generator.UpdateError) as caught:
                generator.require_dist_files(dist, [
                    "kylinlist_1.2.0_arm64.deb", "kylinlist_1.2.0_amd64.deb",
                    "windows-update-artifacts.json"])
            message = str(caught.exception)
            self.assertIn("kylinlist_1.2.0_amd64.deb", message)
            self.assertIn("windows-update-artifacts.json", message)

    def test_passes_when_present(self):
        with tempfile.TemporaryDirectory() as raw:
            dist = Path(raw)
            for name in ("a.deb", "b.deb"):
                (dist / name).write_bytes(b"deb")
            generator.require_dist_files(dist, ["a.deb", "b.deb"])

    def test_prepare_output_rejects_nonempty(self):
        with tempfile.TemporaryDirectory() as raw:
            output = Path(raw) / "release"
            output.mkdir()
            (output / "stale.txt").write_text("x")
            with self.assertRaises(generator.UpdateError):
                generator.prepare_output(output)

    def test_prepare_output_creates_missing(self):
        with tempfile.TemporaryDirectory() as raw:
            output = generator.prepare_output(Path(raw) / "nested" / "release")
            self.assertTrue(output.is_dir())


class WriteReleaseTest(unittest.TestCase):
    def test_layout_manifest_and_release_info(self):
        with tempfile.TemporaryDirectory() as raw:
            base = Path(raw)
            dist = base / "dist"
            dist.mkdir()
            payload = b"artifact-bytes"
            sha256 = hashlib.sha256(payload).hexdigest()
            entry = {
                "platform": "linux", "arch": "arm64", "variant": "system-qt",
                "packageVersion": "1.2.0", "filename": "kylinlist_1.2.0_arm64.deb",
                "sha256": sha256, "size": len(payload),
            }
            (dist / entry["filename"]).write_bytes(payload)
            output = base / "release"
            manifest, manifest_path = generator.write_release(
                dist, output, "1.2.0", "b" * 40, "KylinList 1.2.0。", [entry], False)
            self.assertEqual(
                manifest_path, output / "updates" / "v1" / "stable" / "manifest.json")
            copied = (output / "updates" / "files" / "1.2.0" /
                      "linux-arm64-system-qt" / "kylinlist_1.2.0_arm64.deb")
            self.assertTrue(copied.is_file())
            self.assertEqual(copied.read_bytes(), payload)
            self.assertEqual(manifest["artifacts"][0]["path"], "files/1.2.0/linux-arm64-system-qt/kylinlist_1.2.0_arm64.deb")
            self.assertEqual(manifest["artifacts"][0]["sha256"], sha256)
            data = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(data["schemaVersion"], 1)
            self.assertEqual(data["channel"], "stable")
            self.assertEqual(data["version"], "1.2.0")
            self.assertEqual(data["artifacts"][0]["size"], len(payload))
            info = json.loads((output / "RELEASE-INFO.json").read_text(encoding="utf-8"))
            self.assertEqual(info["version"], "1.2.0")
            self.assertEqual(info["gitSha"], "b" * 40)
            self.assertFalse(info["partial"])
            self.assertIn("generatedAt", info)
            self.assertEqual(info["artifacts"], [{
                "target": "linux-arm64-system-qt",
                "file": "kylinlist_1.2.0_arm64.deb",
                "sha256": sha256,
                "size": len(payload),
            }])


class InstallInfoValidationTest(unittest.TestCase):
    def test_valid_info_passes(self):
        info = {"product": "kylinlist", "platform": "linux", "version": "1.2.0",
                "arch": "arm64", "variant": "system-qt"}
        generator.validate_install_info(info, "x.deb", "linux", "arm64", "system-qt", "1.2.0")

    def test_mismatched_field_rejected(self):
        info = {"product": "kylinlist", "platform": "linux", "version": "1.1.0",
                "arch": "arm64", "variant": "system-qt"}
        with self.assertRaises(generator.UpdateError):
            generator.validate_install_info(info, "x.deb", "linux", "arm64", "system-qt", "1.2.0")

    def test_missing_field_rejected(self):
        info = {"product": "kylinlist", "platform": "linux", "version": "1.2.0",
                "arch": "arm64"}
        with self.assertRaises(generator.UpdateError):
            generator.validate_install_info(info, "x.deb", "linux", "arm64", "system-qt", "1.2.0")

    def test_non_object_rejected(self):
        with self.assertRaises(generator.UpdateError):
            generator.validate_install_info([1, 2], "x.deb", "linux", "arm64", "system-qt", "1.2.0")


@unittest.skipUnless(shutil.which("dpkg-deb"), "dpkg-deb is not installed on this host")
class DebInspectionTest(unittest.TestCase):
    def setUp(self):
        self._raw = tempfile.mkdtemp(prefix="kylinlist-debtest-")
        self.base = Path(self._raw)
        self.work = self.base / "work"
        self.work.mkdir()

    def tearDown(self):
        shutil.rmtree(self._raw, ignore_errors=True)

    def build_deb(self, filename, *, version="1.2.0", arch="arm64",
                  info_variant="system-qt", package="kylinlist"):
        pkg = self.base / filename + ".pkg"
        debian = pkg / "DEBIAN"
        debian.mkdir(parents=True)
        (debian / "control").write_text(
            "Package: {0}\nVersion: {1}\nArchitecture: {2}\n"
            "Maintainer: KylinList <kylinlist@localhost>\nDescription: test\n".format(
                package, version, arch))
        info_dir = pkg / "usr" / "share" / "kylinlist"
        info_dir.mkdir(parents=True)
        (info_dir / "install-info.json").write_text(
            '{{"product": "kylinlist", "platform": "linux", "version": "{0}", '
            '"arch": "{1}", "variant": "{2}"}}\n'.format(version, arch, info_variant))
        deb = self.base / filename
        subprocess.run(
            ["dpkg-deb", "--build", "--root-owner-group", str(pkg), str(deb)],
            check=True, capture_output=True, text=True)
        return deb

    def test_inspect_deb_accepts_consistent_package(self):
        filename = "kylinlist_1.2.0_arm64.deb"
        deb = self.build_deb(filename)
        entry = generator.inspect_deb(
            self.base, filename, "1.2.0", "arm64", "system-qt", self.work)
        self.assertEqual(entry["platform"], "linux")
        self.assertEqual(entry["arch"], "arm64")
        self.assertEqual(entry["variant"], "system-qt")
        self.assertEqual(entry["packageVersion"], "1.2.0")
        self.assertEqual(entry["filename"], filename)
        self.assertEqual(entry["sha256"], hashlib.sha256(deb.read_bytes()).hexdigest())
        self.assertEqual(entry["size"], deb.stat().st_size)

    def test_inspect_deb_rejects_variant_mismatch(self):
        filename = "kylinlist_1.2.0_arm64_bundle.deb"
        self.build_deb(filename, info_variant="system-qt")
        with self.assertRaises(generator.UpdateError):
            generator.inspect_deb(
                self.base, filename, "1.2.0", "arm64", "bundled-qt", self.work)

    def test_inspect_deb_rejects_wrong_architecture(self):
        filename = "kylinlist_1.2.0_arm64.deb"
        self.build_deb(filename, arch="amd64", info_variant="system-qt")
        with self.assertRaises(generator.UpdateError):
            generator.inspect_deb(
                self.base, filename, "1.2.0", "arm64", "system-qt", self.work)

    def test_inspect_deb_rejects_wrong_version(self):
        filename = "kylinlist_1.2.0_arm64.deb"
        self.build_deb(filename, version="1.1.0")
        with self.assertRaises(generator.UpdateError):
            generator.inspect_deb(
                self.base, filename, "1.2.0", "arm64", "system-qt", self.work)

    def test_inspect_deb_rejects_foreign_package_name(self):
        filename = "kylinlist_1.2.0_arm64.deb"
        self.build_deb(filename, package="other-package")
        with self.assertRaises(generator.UpdateError):
            generator.inspect_deb(
                self.base, filename, "1.2.0", "arm64", "system-qt", self.work)

    def test_inspect_deb_rejects_missing_install_info(self):
        filename = "kylinlist_1.2.0_arm64.deb"
        pkg = self.base / "no-info.pkg" / "DEBIAN"
        pkg.mkdir(parents=True)
        (pkg / "control").write_text(
            "Package: kylinlist\nVersion: 1.2.0\nArchitecture: arm64\n"
            "Maintainer: KylinList <kylinlist@localhost>\nDescription: test\n")
        deb = self.base / filename
        subprocess.run(
            ["dpkg-deb", "--build", "--root-owner-group", str(self.base / "no-info.pkg"),
             str(deb)], check=True, capture_output=True, text=True)
        with self.assertRaises(generator.UpdateError):
            generator.inspect_deb(
                self.base, filename, "1.2.0", "arm64", "system-qt", self.work)


class WindowsSidecarTest(unittest.TestCase):
    def make_dist(self, raw):
        dist = Path(raw) / "dist"
        dist.mkdir()
        (dist / "KylinList-1.2.0-Windows-x64-Setup.exe").write_bytes(b"setup")
        (dist / "KylinList-1.2.0-Windows-x64.zip").write_bytes(b"portable")
        return dist

    def test_accepts_matching_sidecar(self):
        with tempfile.TemporaryDirectory() as raw:
            dist = self.make_dist(raw)
            (dist / generator.WINDOWS_SIDECAR_NAME).write_text(
                '{"version": "1.2.0", "artifacts": ['
                '{"kind": "setup", "file": "KylinList-1.2.0-Windows-x64-Setup.exe"}, '
                '{"kind": "portable", "file": "KylinList-1.2.0-Windows-x64.zip"}]}',
                encoding="utf-8")
            entries = generator.inspect_windows(dist, "1.2.0")
            self.assertEqual([entry["variant"] for entry in entries], ["setup", "portable"])
            self.assertTrue(all(entry["platform"] == "windows" for entry in entries))
            self.assertTrue(all(entry["arch"] == "amd64" for entry in entries))
            self.assertEqual(entries[0]["size"], len(b"setup"))
            self.assertEqual(entries[1]["sha256"],
                             hashlib.sha256(b"portable").hexdigest())

    def test_rejects_version_mismatch(self):
        with tempfile.TemporaryDirectory() as raw:
            dist = self.make_dist(raw)
            (dist / generator.WINDOWS_SIDECAR_NAME).write_text(
                '{"version": "1.1.0", "artifacts": []}', encoding="utf-8")
            with self.assertRaises(generator.UpdateError):
                generator.inspect_windows(dist, "1.2.0")

    def test_rejects_unexpected_filename(self):
        with tempfile.TemporaryDirectory() as raw:
            dist = self.make_dist(raw)
            (dist / generator.WINDOWS_SIDECAR_NAME).write_text(
                '{"version": "1.2.0", "artifacts": ['
                '{"kind": "setup", "file": "Other-Name.exe"}, '
                '{"kind": "portable", "file": "KylinList-1.2.0-Windows-x64.zip"}]}',
                encoding="utf-8")
            with self.assertRaises(generator.UpdateError):
                generator.inspect_windows(dist, "1.2.0")

    def test_rejects_missing_kind(self):
        with tempfile.TemporaryDirectory() as raw:
            dist = self.make_dist(raw)
            (dist / generator.WINDOWS_SIDECAR_NAME).write_text(
                '{"version": "1.2.0", "artifacts": ['
                '{"kind": "setup", "file": "KylinList-1.2.0-Windows-x64-Setup.exe"}]}',
                encoding="utf-8")
            with self.assertRaises(generator.UpdateError):
                generator.inspect_windows(dist, "1.2.0")

    def test_missing_artifact_file_rejected(self):
        with tempfile.TemporaryDirectory() as raw:
            dist = self.make_dist(raw)
            (dist / "KylinList-1.2.0-Windows-x64.zip").unlink()
            (dist / generator.WINDOWS_SIDECAR_NAME).write_text(
                '{"version": "1.2.0", "artifacts": ['
                '{"kind": "setup", "file": "KylinList-1.2.0-Windows-x64-Setup.exe"}, '
                '{"kind": "portable", "file": "KylinList-1.2.0-Windows-x64.zip"}]}',
                encoding="utf-8")
            with self.assertRaises(generator.UpdateError):
                generator.inspect_windows(dist, "1.2.0")


if __name__ == "__main__":
    unittest.main()
