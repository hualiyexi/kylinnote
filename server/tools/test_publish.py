#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""publish.py 的单元测试（仅标准库 unittest）。

运行方式：
  本机:        python -I server/tools/test_publish.py
  容器内:      docker compose run --rm --entrypoint python publisher -I /tools/test_publish.py
"""

import contextlib
import hashlib
import io
import json
import os
import shutil
import stat
import sys
import tempfile
import unittest
from pathlib import Path

# 隔离模式下脚本目录不在 sys.path，这里显式加入以导入同目录的 publish 模块
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import publish  # noqa: E402


def write_bytes(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write_manifest(root, manifest):
    write_bytes(Path(root) / "updates" / "v1" / "stable" / "manifest.json",
                json.dumps(manifest, ensure_ascii=False, indent=2).encode("utf-8"))


def make_release(root, version="1.0.0", contents=None, release_info=b'{"source":"ci"}'):
    """构造一个与 CI 产物同构的 release 目录，返回清单 dict。

    contents: {(platform, arch, variant): 文件字节内容}
    """
    root = Path(root)
    if contents is None:
        contents = {("linux", "arm64", "system-qt"): b"arm64-package"}
    artifacts = []
    for (platform, arch, variant), data in contents.items():
        filename = "kylinlist_%s_%s.deb" % (version, arch)
        rel = "files/%s/%s-%s-%s/%s" % (version, platform, arch, variant, filename)
        write_bytes(root / "updates" / rel, data)
        artifacts.append({
            "platform": platform,
            "arch": arch,
            "variant": variant,
            "packageVersion": version,
            "path": rel,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
    manifest = {
        "schemaVersion": 1,
        "channel": "stable",
        "version": version,
        "publishedAt": "2026-11-13T08:00:00Z",
        "notes": "测试版本",
        "artifacts": artifacts,
    }
    write_manifest(root, manifest)
    if release_info is not None:
        write_bytes(root / "RELEASE-INFO.json", release_info)
    return manifest


def edit_manifest(root, fn):
    """读取 release 目录清单，应用 fn 修改后写回。"""
    path = Path(root) / "updates" / "v1" / "stable" / "manifest.json"
    manifest = json.loads(path.read_bytes().decode("utf-8"))
    fn(manifest)
    write_manifest(root, manifest)


def quiet(fn, *args, **kwargs):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        return fn(*args, **kwargs)


class PublishTestBase(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="kylin-publish-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.release = self.tmp / "release"
        self.data = self.tmp / "data"

    def new_release(self, name, **kwargs):
        path = self.tmp / name
        make_release(path, **kwargs)
        return path

    def publish(self, release):
        return quiet(publish.do_publish, release, self.data)

    def stable_path(self):
        return self.data / "public" / "updates" / "v1" / "stable" / "manifest.json"

    def stable_bytes(self):
        return self.stable_path().read_bytes()

    def stable_version(self):
        return json.loads(self.stable_bytes().decode("utf-8"))["version"]

    def archive_names(self):
        archive = self.data / "public" / "updates" / "v1" / "archive"
        if not archive.is_dir():
            return []
        return sorted(p.name for p in archive.iterdir())


class PublishFlowTests(PublishTestBase):

    def test_publish_success_layout_and_release_info(self):
        manifest = make_release(self.release, "1.0.0", {
            ("linux", "arm64", "system-qt"): b"arm64-package",
            ("linux", "amd64", "system-qt"): b"amd64-package",
        }, release_info=b"{not-even-json")  # RELEASE-INFO 内容不合法也不影响发布
        self.publish(self.release)

        self.assertEqual(self.stable_bytes(),
                         (self.release / "updates" / "v1" / "stable" / "manifest.json").read_bytes())
        for art in manifest["artifacts"]:
            installed = self.data / "public" / "updates" / art["path"]
            self.assertTrue(installed.is_file(), art["path"])
            self.assertEqual(publish.sha256_file(installed), art["sha256"])
        # 首次发布没有旧 stable 清单，不应产生 manifest-* 归档
        self.assertFalse([n for n in self.archive_names() if n.startswith("manifest-")])
        # RELEASE-INFO 被透传归档
        self.assertTrue(any(n.startswith("release-info-1.0.0-") for n in self.archive_names()))

    def test_publish_without_release_info(self):
        make_release(self.release, "1.0.0", release_info=None)
        self.publish(self.release)
        self.assertEqual(self.stable_version(), "1.0.0")
        self.assertEqual(self.archive_names(), [])

    def test_publish_new_version_archives_old(self):
        make_release(self.release, "1.0.0")
        self.publish(self.release)
        release11 = self.new_release("release11", version="1.1.0",
                                     contents={("linux", "arm64", "system-qt"): b"pkg-1.1.0"})
        self.publish(release11)

        self.assertEqual(self.stable_version(), "1.1.0")
        archives = [n for n in self.archive_names() if n.startswith("manifest-")]
        self.assertEqual(len(archives), 1)
        self.assertTrue(archives[0].startswith("manifest-1.0.0-"))
        self.assertTrue(archives[0].endswith(".json"))
        # 旧版本文件仍然保留
        self.assertTrue((self.data / "public" / "updates" / "files" / "1.0.0").is_dir())

    def test_validate_is_dry_run(self):
        make_release(self.release, "1.0.0")
        quiet(publish.do_validate, self.release)
        self.assertFalse(self.data.exists())

    @unittest.skipIf(os.name == "nt", "Windows 文件权限语义不同")
    def test_published_files_readable_by_others(self):
        # publisher 容器内以 root 写入，nginx worker 以非 root 读取：必须 0644
        manifest = make_release(self.release, "1.0.0")
        self.publish(self.release)
        paths = [self.data / "public" / "updates" / art["path"] for art in manifest["artifacts"]]
        paths.append(self.stable_path())
        for p in paths:
            self.assertEqual(stat.S_IMODE(p.stat().st_mode) & 0o744, 0o644, str(p))


class PublishRejectionTests(PublishTestBase):

    def setUp(self):
        super().setUp()
        make_release(self.release, "1.0.0")
        self.publish(self.release)  # 先有一个正常 stable，验证失败后必须保持不变
        self.stable_before = self.stable_bytes()
        self.next = self.tmp / "next"

    def assert_rejected_and_stable_unchanged(self):
        with self.assertRaises(publish.PublishError):
            self.publish(self.next)
        self.assertEqual(self.stable_bytes(), self.stable_before)

    def test_missing_artifact_file(self):
        manifest = make_release(self.next, "1.1.0",
                                contents={("linux", "arm64", "system-qt"): b"pkg"})
        missing = self.next / "updates" / manifest["artifacts"][0]["path"]
        missing.unlink()
        self.assert_rejected_and_stable_unchanged()
        # 中途失败不得产生半安装状态：1.1.0 的任何文件都不应落盘
        self.assertFalse((self.data / "public" / "updates" / "files" / "1.1.0").exists())

    def test_size_mismatch(self):
        make_release(self.next, "1.1.0", contents={("linux", "arm64", "system-qt"): b"pkg"})
        edit_manifest(self.next, lambda m: m["artifacts"][0].update(size=m["artifacts"][0]["size"] + 1))
        self.assert_rejected_and_stable_unchanged()

    def test_sha_mismatch(self):
        make_release(self.next, "1.1.0", contents={("linux", "arm64", "system-qt"): b"pkg"})
        edit_manifest(self.next, lambda m: m["artifacts"][0].update(
            sha256="0" * 63 + "1"))
        self.assert_rejected_and_stable_unchanged()

    def test_sha_format_rejected(self):
        make_release(self.next, "1.1.0", contents={("linux", "arm64", "system-qt"): b"pkg"})
        real = hashlib.sha256(b"pkg").hexdigest().upper()
        edit_manifest(self.next, lambda m: m["artifacts"][0].update(sha256=real))
        self.assert_rejected_and_stable_unchanged()

    def test_duplicate_target_triple(self):
        make_release(self.next, "1.1.0", contents={("linux", "arm64", "system-qt"): b"pkg"})
        edit_manifest(self.next, lambda m: m["artifacts"].append(dict(m["artifacts"][0])))
        self.assert_rejected_and_stable_unchanged()

    def test_package_version_mismatch(self):
        make_release(self.next, "1.1.0", contents={("linux", "arm64", "system-qt"): b"pkg"})
        edit_manifest(self.next, lambda m: m["artifacts"][0].update(packageVersion="9.9.9"))
        self.assert_rejected_and_stable_unchanged()

    def test_illegal_versions(self):
        for bad in ("1.02.0", "01.0.0", "1.2", "v1.2.0", "1.2.3.4", "1.2.x"):
            make_release(self.next, bad if bad[0].isdigit() else "1.1.0",
                         contents={("linux", "arm64", "system-qt"): b"pkg"})
            edit_manifest(self.next, lambda m, bad=bad: m.update(version=bad))
            with self.assertRaises(publish.PublishError, msg=bad):
                quiet(publish.do_validate, self.next)
            shutil.rmtree(self.next)

    def test_bad_platform_arch_variant(self):
        base = dict(version="1.1.0", contents={("linux", "arm64", "system-qt"): b"pkg"})
        for field, value in (("platform", "macos"), ("arch", "i386"), ("variant", "snap")):
            make_release(self.next, **base)
            edit_manifest(self.next, lambda m, f=field, v=value: m["artifacts"][0].update({f: v}))
            with self.assertRaises(publish.PublishError, msg="%s=%s" % (field, value)):
                quiet(publish.do_validate, self.next)
            shutil.rmtree(self.next)

    def test_path_traversal_and_wrong_shape(self):
        base = dict(version="1.1.0", contents={("linux", "arm64", "system-qt"): b"pkg"})
        bad_paths = (
            "files/1.1.0/linux-arm64-system-qt/../../evil.deb",
            "/etc/passwd",
            "files/1.1.0/linux-arm64-system-rt/other.deb",
            "files/1.1.0/linux-arm64-system-qt/",
            "files/9.9.9/linux-arm64-system-qt/kylinlist_1.1.0_arm64.deb",
        )
        for bad in bad_paths:
            make_release(self.next, **base)
            edit_manifest(self.next, lambda m, bad=bad: m["artifacts"][0].update(path=bad))
            with self.assertRaises(publish.PublishError, msg=bad):
                quiet(publish.do_validate, self.next)
            shutil.rmtree(self.next)


class IdempotencyTests(PublishTestBase):

    def test_same_release_repeated_is_idempotent(self):
        make_release(self.release, "1.0.0")
        self.publish(self.release)
        first = self.stable_bytes()
        self.publish(self.release)
        self.assertEqual(self.stable_bytes(), first)
        self.assertEqual([n for n in self.archive_names() if n.startswith("manifest-")], [])

    def test_same_version_different_hash_rejected(self):
        make_release(self.release, "1.0.0", contents={("linux", "arm64", "system-qt"): b"content-A"})
        self.publish(self.release)
        stable_before = self.stable_bytes()
        installed = self.data / "public" / "updates" / "files" / "1.0.0" / \
            "linux-arm64-system-qt" / "kylinlist_1.0.0_arm64.deb"

        conflicting = self.new_release("conflicting", version="1.0.0",
                                       contents={("linux", "arm64", "system-qt"): b"content-B"})
        with self.assertRaises(publish.PublishError):
            self.publish(conflicting)
        # 已安装文件与 stable 清单都保持原样
        self.assertEqual(publish.sha256_file(installed),
                         hashlib.sha256(b"content-A").hexdigest())
        self.assertEqual(self.stable_bytes(), stable_before)


class RollbackTests(PublishTestBase):

    def setUp(self):
        super().setUp()
        self.r10 = self.new_release("r10", version="1.0.0",
                                    contents={("linux", "arm64", "system-qt"): b"pkg-1.0.0"})
        self.r11 = self.new_release("r11", version="1.1.0",
                                    contents={("linux", "arm64", "system-qt"): b"pkg-1.1.0"})
        self.r12 = self.new_release("r12", version="1.2.0",
                                    contents={("linux", "arm64", "system-qt"): b"pkg-1.2.0"})
        for r in (self.r10, self.r11, self.r12):
            self.publish(r)

    def test_rollback_to_previous_version(self):
        quiet(publish.do_rollback, self.data)
        self.assertEqual(self.stable_version(), "1.1.0")
        self.assertEqual(self.stable_bytes(),
                         (self.r11 / "updates" / "v1" / "stable" / "manifest.json").read_bytes())

    def test_rollback_to_explicit_version(self):
        quiet(publish.do_rollback, self.data, "1.0.0")
        self.assertEqual(self.stable_version(), "1.0.0")

    def test_rollback_unknown_version_rejected(self):
        with self.assertRaises(publish.PublishError):
            quiet(publish.do_rollback, self.data, "9.9.9")

    def test_rollback_requires_files_still_present(self):
        shutil.rmtree(self.data / "public" / "updates" / "files" / "1.1.0")
        before = self.stable_bytes()
        with self.assertRaises(publish.PublishError):
            quiet(publish.do_rollback, self.data)
        self.assertEqual(self.stable_bytes(), before)

    def test_rollback_without_archives(self):
        empty = self.tmp / "empty-data"
        make_release(self.tmp / "solo", version="1.0.0")
        quiet(publish.do_publish, self.tmp / "solo", empty)
        with self.assertRaises(publish.PublishError):
            quiet(publish.do_rollback, empty)


class StatusTests(PublishTestBase):

    def test_status_reports_versions(self):
        make_release(self.release, "1.0.0")
        self.publish(self.release)
        self.new_release("r11", version="1.1.0")
        quiet(publish.do_publish, self.tmp / "r11", self.data)

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            publish.do_status(self.data)
        out = buf.getvalue()
        self.assertIn("1.1.0", out)
        self.assertIn("manifest-1.0.0-", out.replace("\n", ""))

    def test_status_empty_data(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            publish.do_status(self.data)
        self.assertIn("尚未发布", buf.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
