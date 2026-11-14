#!/usr/bin/env python3
"""Tests for deploy/ci_release.py and deploy/fetch_ci_release.py.

Run with: python -I deploy/test_fetch_ci_release.py

No network access: the gh CLI calls in fetch_ci_release.py are redirected
to a local fake gh (a small Python script in a per-test temporary
directory) that answers from a state file and copies synthetic bundle
zips. Everything else is real: argument checks, the GitHub API answer
verification, safe zip extraction, output handling and cleanup.
"""

import contextlib
import importlib.util
import io
import json
import shutil
import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


def _load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_HERE = Path(__file__).resolve().parent
ci_release = _load_module("ci_release", _HERE / "ci_release.py")
fetch = _load_module("fetch_ci_release_under_test", _HERE / "fetch_ci_release.py")

RUN_ID = "43470023611"
OTHER_RUN_ID = "98765432100"
HEAD_SHA = "0123456789abcdef0123456789abcdef01234567"
OTHER_SHA = "fedcba9876543210fedcba9876543210fedcba98"
VERSION = "1.3.0"

ALL_TARGETS = list(ci_release.EXPECTED_TARGETS)


def make_release_tree(parent, *, version=VERSION, run_id=RUN_ID, head_sha=HEAD_SHA,
                      git_sha=None, partial=False, targets=None, artifacts=None,
                      manifest_version=None, ci_extra=None,
                      drop_release_info=False, drop_manifest=False):
    """Write a synthetic release directory with the given properties."""
    if targets is None:
        targets = ALL_TARGETS
    release = Path(parent)
    release.mkdir(parents=True, exist_ok=True)
    if artifacts is None:
        artifacts = [
            {"target": target,
             "file": "kylinlist-{0}-{1}.bin".format(version, target),
             "sha256": "0" * 64,
             "size": 1}
            for target in targets]
    info = {
        "version": version,
        "gitSha": git_sha if git_sha is not None else head_sha,
        "generatedAt": "2026-11-14T00:00:00Z",
        "partial": partial,
        "ci": {
            "repository": "hualiyexi/kylinnote",
            "workflow": "Build and package",
            "workflowPath": ".github/workflows/build.yml",
            "runId": run_id,
            "runUrl": "https://github.com/hualiyexi/kylinnote/actions/runs/" + run_id,
            "ref": "refs/tags/v" + version,
            "eventName": "push",
            "headSha": head_sha,
        },
        "artifacts": artifacts,
    }
    if ci_extra:
        info["ci"].update(ci_extra)
    if not drop_release_info:
        (release / "RELEASE-INFO.json").write_text(
            json.dumps(info, indent=2) + "\n", encoding="utf-8")
    if not drop_manifest:
        manifest = {
            "schemaVersion": 1,
            "channel": "stable",
            "version": manifest_version if manifest_version is not None else version,
            "publishedAt": "2026-11-14T00:00:00Z",
            "notes": "KylinList {0}。".format(version),
            "artifacts": [],
        }
        manifest_path = release / "updates" / "v1" / "stable" / "manifest.json"
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    for target in targets:
        directory = release / "updates" / "files" / version / target
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "kylinlist-{0}-{1}.bin".format(version, target)).write_bytes(b"bin")
    return release


def zip_release_tree(tree, zip_path, extra_members=(), special_members=()):
    """Zip a release tree; optionally add crafted members.

    special_members items are (name, unix-mode, content) triples so tests
    can plant symlinks or fifos via external_attr.
    """
    with zipfile.ZipFile(str(zip_path), "w") as archive:
        for path in sorted(tree.rglob("*")):
            arcname = path.relative_to(tree).as_posix()
            if path.is_dir():
                archive.writestr(arcname + "/", b"")
            else:
                archive.write(str(path), arcname)
        for name, content in extra_members:
            archive.writestr(name, content)
        for name, mode, content in special_members:
            info = zipfile.ZipInfo(name)
            info.external_attr = (mode & 0xFFFF) << 16
            archive.writestr(info, content)


FAKE_GH_SCRIPT = '''#!/usr/bin/env python3
"""Fake gh CLI for tests: answers from state.json, never touches the network."""
import json
import shutil
import sys
from pathlib import Path

state = json.loads(
    (Path(__file__).parent / "state.json").read_text(encoding="utf-8"))
args = sys.argv[1:]


def fail(message):
    print(message, file=sys.stderr)
    raise SystemExit(1)


if args[:1] == ["api"]:
    if len(args) != 2:
        fail("fake gh: unsupported api arguments: %r" % (args,))
    prefix = "repos/hualiyexi/kylinnote/actions/runs/"
    endpoint = args[1]
    if not endpoint.startswith(prefix):
        fail("fake gh: unexpected endpoint: " + endpoint)
    run_id = endpoint[len(prefix):]
    record = state.get("runs", {}).get(run_id)
    if record is None:
        fail("HTTP 404: Not Found (%s)" % endpoint)
    if "error" in record:
        fail(record["error"])
    print(json.dumps(record))
    raise SystemExit(0)

if args[:2] == ["run", "download"]:
    if "-D" not in args:
        fail("fake gh: run download without -D")
    dest = Path(args[args.index("-D") + 1])
    if state.get("download_error"):
        fail(state["download_error"])
    dest.mkdir(parents=True, exist_ok=True)
    for name in state.get("download_files", ["kylinlist-update-bundle.zip"]):
        shutil.copyfile(str(Path(state["download_root"]) / name), str(dest / name))
    raise SystemExit(0)

fail("fake gh: unsupported arguments: %r" % (args,))
'''


class ValidateCiReleaseTest(unittest.TestCase):
    def setUp(self):
        self._root = tempfile.mkdtemp(prefix="kylinlist-civalidtest-")
        self.base = Path(self._root)

    def tearDown(self):
        shutil.rmtree(self._root, ignore_errors=True)

    def make(self, **options):
        return make_release_tree(self.base / "release", **options)

    def test_valid_release_returns_version_and_info(self):
        release = self.make()
        version, info = ci_release.validate_ci_release(release)
        self.assertEqual(version, VERSION)
        self.assertEqual(info["ci"]["runId"], RUN_ID)
        self.assertIsInstance(info["ci"]["runId"], str)
        self.assertIs(info["partial"], False)
        self.assertEqual(info["gitSha"], HEAD_SHA)

    def test_missing_release_info_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(self.make(drop_release_info=True))

    def test_partial_release_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(self.make(partial=True))

    def test_partial_flag_missing_rejected(self):
        # partial absent behaves exactly like partial != false.
        release = self.make()
        info = json.loads((release / "RELEASE-INFO.json").read_text(encoding="utf-8"))
        del info["partial"]
        (release / "RELEASE-INFO.json").write_text(
            json.dumps(info), encoding="utf-8")
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(release)

    def test_git_sha_mismatch_with_ci_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(self.make(git_sha=OTHER_SHA))

    def test_git_sha_malformed_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(self.make(git_sha="not-a-sha"))

    def test_missing_ci_object_rejected(self):
        release = self.make()
        info = json.loads((release / "RELEASE-INFO.json").read_text(encoding="utf-8"))
        del info["ci"]
        (release / "RELEASE-INFO.json").write_text(
            json.dumps(info), encoding="utf-8")
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(release)

    def test_ci_field_missing_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(self.make(ci_extra={"headSha": None}))

    def test_wrong_repository_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(
                self.make(ci_extra={"repository": "someone/else"}))

    def test_wrong_workflow_path_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(
                self.make(ci_extra={"workflowPath": ".github/workflows/other.yml"}))

    def test_invalid_run_id_rejected(self):
        for bad in ("not-numeric", "0", "0123"):
            with self.subTest(run_id=bad):
                release = self.make(run_id=bad)
                with self.assertRaises(ValueError):
                    ci_release.validate_ci_release(release)

    def test_run_url_mismatch_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(
                self.make(ci_extra={
                    "runUrl": "https://github.com/someone/else/actions/runs/" + RUN_ID}))

    def test_duplicate_target_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(
                self.make(targets=[ALL_TARGETS[0]] + ALL_TARGETS))

    def test_missing_target_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(self.make(targets=ALL_TARGETS[:5]))

    def test_extra_target_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(
                self.make(targets=ALL_TARGETS + ["linux-riscv64-system-qt"]))

    def test_missing_manifest_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(self.make(drop_manifest=True))

    def test_manifest_version_mismatch_rejected(self):
        with self.assertRaises(ValueError):
            ci_release.validate_ci_release(self.make(manifest_version="9.9.9"))


class FetchCiReleaseTest(unittest.TestCase):
    def setUp(self):
        self._root = tempfile.mkdtemp(prefix="kylinlist-fetchtest-")
        self.base = Path(self._root)

    def tearDown(self):
        shutil.rmtree(self._root, ignore_errors=True)

    def make_bundle(self, *, extra_members=(), special_members=(),
                    zip_name="kylinlist-update-bundle.zip", **options):
        tree = self.base / ("tree-of-" + zip_name)
        make_release_tree(tree, **options)
        zip_path = self.base / zip_name
        zip_release_tree(tree, zip_path, extra_members, special_members)
        return zip_path

    def fake_gh(self, state):
        fake_dir = Path(tempfile.mkdtemp(prefix="kylinlist-fakegh-"))
        self.addCleanup(shutil.rmtree, str(fake_dir), True)
        (fake_dir / "state.json").write_text(json.dumps(state), encoding="utf-8")
        script = fake_dir / "fake_gh.py"
        script.write_text(FAKE_GH_SCRIPT, encoding="utf-8")
        return (sys.executable, str(script))

    def success_state(self, zip_path, **run_overrides):
        run = {
            "id": int(RUN_ID),
            "path": ".github/workflows/build.yml",
            "status": "completed",
            "conclusion": "success",
            "head_sha": HEAD_SHA,
        }
        run.update(run_overrides)
        return {
            "runs": {RUN_ID: run},
            "download_root": str(zip_path.parent),
            "download_files": [zip_path.name],
        }

    def run_fetch(self, state, run_id=RUN_ID, output=None):
        if output is None:
            output = self.base / "out"
        gh = self.fake_gh(state)
        with contextlib.redirect_stdout(io.StringIO()):
            code = fetch.main([run_id, str(output)], gh=gh)
        return code, output

    def test_success_downloads_extracts_and_validates(self):
        zip_path = self.make_bundle()
        code, output = self.run_fetch(self.success_state(zip_path))
        self.assertEqual(code, 0)
        self.assertTrue((output / "RELEASE-INFO.json").is_file())
        self.assertTrue(
            (output / "updates" / "v1" / "stable" / "manifest.json").is_file())
        for target in ALL_TARGETS:
            self.assertTrue(
                (output / "updates" / "files" / VERSION / target).is_dir(), target)
        version, info = ci_release.validate_ci_release(output)
        self.assertEqual(version, VERSION)
        self.assertEqual(info["ci"]["runId"], RUN_ID)
        self.assertEqual(info["ci"]["headSha"], HEAD_SHA)

    def test_success_creates_missing_output_parents(self):
        zip_path = self.make_bundle()
        output = self.base / "nested" / "deeper" / "out"
        gh = self.fake_gh(self.success_state(zip_path))
        with contextlib.redirect_stdout(io.StringIO()):
            code = fetch.main([RUN_ID, str(output)], gh=gh)
        self.assertEqual(code, 0)
        self.assertTrue((output / "RELEASE-INFO.json").is_file())

    def test_rejects_non_numeric_run_id_argument(self):
        self.assertEqual(fetch.main(["12x45", str(self.base / "out")]), 2)
        self.assertFalse((self.base / "out").exists())

    def test_rejects_wrong_argument_count(self):
        self.assertEqual(fetch.main([RUN_ID]), 2)
        self.assertEqual(fetch.main([RUN_ID, "a", "b"]), 2)

    def test_unknown_run_id_rejected_without_creating_output(self):
        zip_path = self.make_bundle()
        state = self.success_state(zip_path)
        state["runs"] = {}
        code, output = self.run_fetch(state)
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_run_from_other_workflow_rejected(self):
        zip_path = self.make_bundle()
        code, output = self.run_fetch(
            self.success_state(zip_path, path=".github/workflows/nightly.yml"))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_incomplete_run_rejected(self):
        zip_path = self.make_bundle()
        code, output = self.run_fetch(
            self.success_state(zip_path, status="in_progress", conclusion=None))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_failed_run_rejected(self):
        zip_path = self.make_bundle()
        code, output = self.run_fetch(
            self.success_state(zip_path, conclusion="failure"))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_api_run_id_echo_mismatch_rejected(self):
        zip_path = self.make_bundle()
        code, output = self.run_fetch(
            self.success_state(zip_path, id=int(OTHER_RUN_ID)))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_head_sha_mismatch_rejected_and_output_cleaned(self):
        zip_path = self.make_bundle()  # bundle records HEAD_SHA
        code, output = self.run_fetch(
            self.success_state(zip_path, head_sha=OTHER_SHA))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_bundle_run_id_mismatch_rejected_and_output_cleaned(self):
        zip_path = self.make_bundle(run_id=OTHER_RUN_ID)
        code, output = self.run_fetch(self.success_state(zip_path))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_zip_slip_member_rejected(self):
        zip_path = self.make_bundle(
            extra_members=[("../escaped.txt", b"evil"),
                           ("updates/good.txt/../../escaped2.txt", b"evil")])
        code, output = self.run_fetch(self.success_state(zip_path))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())
        self.assertFalse((self.base / "escaped.txt").exists())
        self.assertFalse((self.base / "escaped2.txt").exists())

    def test_absolute_path_member_rejected(self):
        zip_path = self.make_bundle(extra_members=[("/etc/passwd", b"evil")])
        code, output = self.run_fetch(self.success_state(zip_path))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_drive_letter_member_rejected(self):
        zip_path = self.make_bundle(extra_members=[("C:/evil.txt", b"evil")])
        code, output = self.run_fetch(self.success_state(zip_path))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_unsafe_member_names_rejected_directly(self):
        # zipfile normalizes os.sep to "/" on Windows for both writing and
        # reading, so a stored backslash name cannot survive to the check
        # there; exercise the name guard directly (it matters on Linux).
        for name in ("updates\\evil.txt", "/etc/passwd", "C:/evil.txt",
                     "../up.txt", "a/../../up.txt", "c:absolute", ""):
            with self.assertRaises(fetch.FetchError, msg=name):
                fetch._check_member_name(name)

    def test_symlink_member_rejected(self):
        zip_path = self.make_bundle(
            special_members=[("updates/evil-link", stat.S_IFLNK | 0o777, "/etc/passwd")])
        code, output = self.run_fetch(self.success_state(zip_path))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_fifo_member_rejected(self):
        zip_path = self.make_bundle(
            special_members=[("updates/evil-fifo", stat.S_IFIFO | 0o600, b"")])
        code, output = self.run_fetch(self.success_state(zip_path))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_existing_nonempty_output_untouched(self):
        zip_path = self.make_bundle()
        output = self.base / "out"
        output.mkdir()
        (output / "keep.txt").write_text("keep me", encoding="utf-8")
        code, _ = self.run_fetch(self.success_state(zip_path), output=output)
        self.assertEqual(code, 1)
        self.assertEqual(
            (output / "keep.txt").read_text(encoding="utf-8"), "keep me")
        self.assertEqual(sorted(entry.name for entry in output.iterdir()),
                         ["keep.txt"])

    def test_existing_file_output_rejected(self):
        zip_path = self.make_bundle()
        output = self.base / "out"
        output.write_text("i am a file", encoding="utf-8")
        code, _ = self.run_fetch(self.success_state(zip_path), output=output)
        self.assertEqual(code, 1)
        self.assertEqual(output.read_text(encoding="utf-8"), "i am a file")

    def test_partial_bundle_rejected_and_output_cleaned(self):
        zip_path = self.make_bundle(partial=True)
        code, output = self.run_fetch(self.success_state(zip_path))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_bundle_with_missing_target_rejected(self):
        zip_path = self.make_bundle(targets=ALL_TARGETS[:5])
        code, output = self.run_fetch(self.success_state(zip_path))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_bundle_with_duplicate_target_rejected(self):
        zip_path = self.make_bundle(targets=[ALL_TARGETS[0]] + ALL_TARGETS)
        code, output = self.run_fetch(self.success_state(zip_path))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_corrupt_inner_zip_rejected_and_cleaned(self):
        zip_path = self.base / "kylinlist-update-bundle.zip"
        zip_path.write_bytes(b"this is not a zip file")
        code, output = self.run_fetch(self.success_state(zip_path))
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_multiple_inner_zips_rejected(self):
        first = self.make_bundle(zip_name="first.zip")
        second = self.make_bundle(zip_name="second.zip")
        state = self.success_state(first)
        state["download_files"] = ["first.zip", "second.zip"]
        code, output = self.run_fetch(state)
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())
        self.assertTrue(second.is_file())

    def test_no_inner_zip_rejected(self):
        plain = self.base / "note.txt"
        plain.write_text("not a zip", encoding="utf-8")
        state = {
            "runs": {RUN_ID: {"id": int(RUN_ID),
                              "path": ".github/workflows/build.yml",
                              "status": "completed",
                              "conclusion": "success",
                              "head_sha": HEAD_SHA}},
            "download_root": str(self.base),
            "download_files": ["note.txt"],
        }
        code, output = self.run_fetch(state)
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())

    def test_download_failure_rejected(self):
        zip_path = self.make_bundle()
        state = self.success_state(zip_path)
        state["download_error"] = "no artifact matches kylinlist-update-bundle"
        code, output = self.run_fetch(state)
        self.assertEqual(code, 1)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
