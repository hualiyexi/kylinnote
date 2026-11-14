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


def manifest_dict(version, notes, published_at="2026-01-01T00:00:00Z"):
    """构造一份结构完整、可通过校验的最小清单 dict（供手工放置归档使用）。"""
    return {
        "schemaVersion": 1,
        "channel": "stable",
        "version": version,
        "publishedAt": published_at,
        "notes": notes,
        "artifacts": [{
            "platform": "linux",
            "arch": "arm64",
            "variant": "system-qt",
            "packageVersion": version,
            "path": "files/%s/linux-arm64-system-qt/kylinlist_%s_arm64.deb" % (version, version),
            "size": 3,
            "sha256": hashlib.sha256(b"pkg").hexdigest(),
        }],
    }


def write_archive(data_dir, name, manifest):
    """把清单 dict 写成数据目录归档文件（不校验文件是否存在，与真实归档同构）。"""
    write_bytes(Path(data_dir) / "public" / "updates" / "v1" / "archive" / name,
                json.dumps(manifest, ensure_ascii=False, indent=2).encode("utf-8"))


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

    def stable_manifest(self):
        return json.loads(self.stable_bytes().decode("utf-8"))

    def stable_history(self):
        return self.stable_manifest()["history"]

    def history_entry(self, version):
        for item in self.stable_history():
            if item["version"] == version:
                return item
        self.fail("history 中没有版本 %s: %r" % (version, self.stable_history()))

    def publish_versions(self, *specs):
        """依次发布若干版本。specs: (name, version, notes)，publishedAt 按序号生成。"""
        published = []
        for i, (name, version, notes) in enumerate(specs, 1):
            release = self.new_release(
                name, version=version,
                contents={("linux", "arm64", "system-qt"): ("pkg-" + version).encode()})
            edit_manifest(release, lambda m, i=i, notes=notes: m.update(
                notes=notes, publishedAt="2026-02-%02dT00:00:00Z" % i))
            self.publish(release)
            published.append(release)
        return published

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

        # stable 是 release 清单注入完整 history 后的产物，逐字段等价（而非字节原样）
        stable = self.stable_manifest()
        for key in ("schemaVersion", "channel", "version", "publishedAt", "notes", "artifacts"):
            self.assertEqual(stable[key], manifest[key], key)
        self.assertEqual(stable["history"], [{
            "version": "1.0.0",
            "publishedAt": manifest["publishedAt"],
            "notes": manifest["notes"],
        }])
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
        archived = json.loads(
            (self.r11 / "updates" / "v1" / "stable" / "manifest.json").read_bytes().decode("utf-8"))
        stable = self.stable_manifest()
        # artifacts 与顶层公告取归档目标；history 保留全部已知版本（含更高的 1.2.0）
        self.assertEqual(stable["artifacts"], archived["artifacts"])
        self.assertEqual(stable["publishedAt"], archived["publishedAt"])
        self.assertEqual(stable["notes"], archived["notes"])
        self.assertEqual([h["version"] for h in stable["history"]],
                         ["1.0.0", "1.1.0", "1.2.0"])

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
        self.assertIn("历史公告 (2 条): 1.0.0, 1.1.0", out)
        self.assertIn("manifest-1.0.0-", out.replace("\n", ""))

    def test_status_empty_data(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            publish.do_status(self.data)
        self.assertIn("尚未发布", buf.getvalue())


class HistoryValidationTests(PublishTestBase):
    """validate_manifest 对可选 history 的结构校验。"""

    def test_history_optional(self):
        make_release(self.release, "1.0.0")
        quiet(publish.do_validate, self.release)  # 新输入不带 history 依然合法

    def test_valid_history_accepted(self):
        make_release(self.release, "1.1.0")
        edit_manifest(self.release, lambda m: m.update(history=[
            {"version": "1.0.0", "publishedAt": "2026-01-01T00:00:00Z", "notes": ""},
            {"version": "1.1.0", "publishedAt": "2026-02-01T00:00:00Z", "notes": "x"},
        ]))
        quiet(publish.do_validate, self.release)

    def check_history_rejected(self, history):
        make_release(self.release, "1.1.0")
        edit_manifest(self.release, lambda m: m.update(history=history))
        with self.assertRaises(publish.PublishError):
            quiet(publish.do_validate, self.release)

    def test_history_not_array(self):
        self.check_history_rejected({"version": "1.0.0"})

    def test_item_not_object(self):
        self.check_history_rejected(["1.0.0"])

    def test_item_bad_version(self):
        self.check_history_rejected([
            {"version": "1.2", "publishedAt": "t", "notes": ""}])

    def test_item_missing_published_at(self):
        self.check_history_rejected([
            {"version": "1.0.0", "publishedAt": "", "notes": ""}])

    def test_item_notes_not_string(self):
        self.check_history_rejected([
            {"version": "1.0.0", "publishedAt": "t", "notes": 5}])

    def test_duplicate_versions(self):
        self.check_history_rejected([
            {"version": "1.0.0", "publishedAt": "t1", "notes": ""},
            {"version": "1.0.0", "publishedAt": "t2", "notes": ""}])

    def test_top_level_published_at_required(self):
        make_release(self.release, "1.0.0")
        edit_manifest(self.release, lambda m: m.pop("publishedAt"))
        with self.assertRaises(publish.PublishError):
            quiet(publish.do_validate, self.release)

    def test_top_level_notes_must_be_string(self):
        make_release(self.release, "1.0.0")
        edit_manifest(self.release, lambda m: m.update(notes=None))
        with self.assertRaises(publish.PublishError):
            quiet(publish.do_validate, self.release)


class HistoryPublishTests(PublishTestBase):
    """publish 的 history 汇总、数值升序与幂等。"""

    def test_first_publish_history_contains_only_self(self):
        manifest = make_release(self.release, "1.0.0")
        self.publish(self.release)
        self.assertEqual(self.stable_history(), [{
            "version": "1.0.0",
            "publishedAt": manifest["publishedAt"],
            "notes": manifest["notes"],
        }])

    def test_history_accumulates_in_numeric_order(self):
        specs = (("r19", "1.9.0", "notes-1.9.0"),
                 ("r110", "1.10.0", "notes-1.10.0"),
                 ("r20", "2.0.0", "notes-2.0.0"))
        self.publish_versions(*specs)
        # 数值升序：1.9.0 < 1.10.0（字符串序会排错）
        self.assertEqual([h["version"] for h in self.stable_history()],
                         ["1.9.0", "1.10.0", "2.0.0"])
        for item, (_, _, notes) in zip(self.stable_history(), specs):
            self.assertEqual(item["notes"], notes)
            self.assertEqual(sorted(item), ["notes", "publishedAt", "version"])

    def test_republish_same_release_idempotent(self):
        self.publish_versions(("r10", "1.0.0", "n10"), ("r11", "1.1.0", "n11"))
        first = self.stable_bytes()
        archives_before = self.archive_names()
        self.publish(self.tmp / "r11")  # 同一 release 目录重复发布
        self.assertEqual(self.stable_bytes(), first)
        # 幂等：不改写 stable，也不把当前 stable 归档
        self.assertEqual(self.archive_names(), archives_before)

    def test_conflict_new_release_overrides_stable_announcement(self):
        # 同版本重发（文件同哈希）：新 release 的公告覆盖当前 stable 的旧公告
        make_release(self.release, "1.0.0")
        edit_manifest(self.release, lambda m: m.update(notes="first-notes"))
        self.publish(self.release)
        again = self.new_release("again", version="1.0.0")  # 默认内容一致 → 同哈希
        edit_manifest(again, lambda m: m.update(notes="second-notes"))
        self.publish(again)
        self.assertEqual(self.stable_manifest()["notes"], "second-notes")
        self.assertEqual(self.history_entry("1.0.0")["notes"], "second-notes")

    def test_conflict_release_history_overrides_stable_history(self):
        make_release(self.release, "1.0.0")
        edit_manifest(self.release, lambda m: m.update(notes="stable-notes"))
        self.publish(self.release)
        r11 = self.new_release("r11", version="1.1.0",
                               contents={("linux", "arm64", "system-qt"): b"pkg-1.1.0"})
        edit_manifest(r11, lambda m: m.update(history=[
            {"version": "1.0.0", "publishedAt": "2026-06-01T00:00:00Z", "notes": "from-release"},
        ]))
        self.publish(r11)
        self.assertEqual(self.history_entry("1.0.0")["notes"], "from-release")


class ArchiveMergeConflictTests(PublishTestBase):
    """归档来源的同版本公告冲突：取最近归档，且与文件遍历顺序无关。"""

    def new_1_2_0(self):
        return self.new_release("r12", version="1.2.0",
                                contents={("linux", "arm64", "system-qt"): b"pkg-1.2.0"})

    def test_latest_timestamp_archive_wins(self):
        self.publish_versions(("r11", "1.1.0", "notes-1.1.0"))
        write_archive(self.data, "manifest-1.0.0-20260101T000000Z.json",
                      manifest_dict("1.0.0", notes="old-1.0.0"))
        write_archive(self.data, "manifest-1.0.0-20260201T000000Z.json",
                      manifest_dict("1.0.0", notes="new-1.0.0"))
        self.publish(self.new_1_2_0())
        self.assertEqual(self.history_entry("1.0.0")["notes"], "new-1.0.0")

    def test_same_timestamp_higher_numeric_suffix_wins(self):
        # 同秒序号按数值比较：-10 晚于 -2（字典序会得到相反结论）
        self.publish_versions(("r11", "1.1.0", "notes-1.1.0"))
        write_archive(self.data, "manifest-1.0.0-20260101T000000Z-2.json",
                      manifest_dict("1.0.0", notes="suffix-two"))
        write_archive(self.data, "manifest-1.0.0-20260101T000000Z-10.json",
                      manifest_dict("1.0.0", notes="suffix-ten"))
        self.publish(self.new_1_2_0())
        self.assertEqual(self.history_entry("1.0.0")["notes"], "suffix-ten")

    def test_stable_announcement_beats_archives(self):
        # 当前 stable 已知的公告优先于任何归档（归档时间戳更晚也一样）
        self.publish_versions(("r10", "1.0.0", "notes-stable"),
                              ("r11", "1.1.0", "notes-1.1.0"))
        write_archive(self.data, "manifest-1.0.0-20990101T000000Z.json",
                      manifest_dict("1.0.0", notes="notes-from-archive"))
        self.publish(self.new_1_2_0())
        self.assertEqual(self.history_entry("1.0.0")["notes"], "notes-stable")


class CorruptArchiveTests(PublishTestBase):
    """损坏的 stable/归档清单必须让发布明确失败，不允许静默丢历史。"""

    def setUp(self):
        super().setUp()
        make_release(self.release, "1.1.0")
        self.publish(self.release)
        self.stable_before = self.stable_bytes()
        self.next = self.new_release("next", version="1.2.0",
                                     contents={("linux", "arm64", "system-qt"): b"pkg-1.2.0"})

    def archive_dir(self):
        return self.data / "public" / "updates" / "v1" / "archive"

    def assert_publish_blocked_before_mutation(self):
        with self.assertRaises(publish.PublishError) as ctx:
            self.publish(self.next)
        self.assertEqual(self.stable_bytes(), self.stable_before)
        # 先整体校验后安装：1.2.0 的任何文件都不应落盘
        self.assertFalse((self.data / "public" / "updates" / "files" / "1.2.0").exists())
        return str(ctx.exception)

    def test_corrupt_archive_json_blocks_publish(self):
        write_bytes(self.archive_dir() / "manifest-0.9.0-20260101T000000Z.json", b"{not-json")
        msg = self.assert_publish_blocked_before_mutation()
        self.assertIn("manifest-0.9.0-20260101T000000Z.json", msg)

    def test_corrupt_archive_structure_blocks_publish(self):
        bad = manifest_dict("0.9.0", notes="bad")
        bad["schemaVersion"] = 2
        write_archive(self.data, "manifest-0.9.0-20260101T000000Z.json", bad)
        self.assert_publish_blocked_before_mutation()

    def test_corrupt_stable_blocks_publish(self):
        self.stable_path().write_bytes(b"{broken")
        with self.assertRaises(publish.PublishError):
            self.publish(self.next)
        # 不动损坏现场，交由人工处理
        self.assertEqual(self.stable_bytes(), b"{broken")

    def test_corrupt_non_manifest_files_ignored(self):
        # release-info-* 与不匹配归档命名的文件不属于清单归档，不阻塞发布
        write_bytes(self.archive_dir() / "release-info-0.9.0-20260101T000000Z.json", b"junk")
        write_bytes(self.archive_dir() / "README.txt", b"hello")
        self.publish(self.next)
        self.assertEqual(self.stable_version(), "1.2.0")


class RollbackHistoryTests(PublishTestBase):
    """rollback 保留全部已知历史（含高于目标版本），目标公告以归档为准。"""

    def setUp(self):
        super().setUp()
        self.publish_versions(
            ("r10", "1.0.0", "notes-1.0.0"),
            ("r11", "1.1.0", "notes-1.1.0"),
            ("r12", "1.2.0", "notes-1.2.0"))

    def test_rollback_keeps_all_history_including_newer(self):
        quiet(publish.do_rollback, self.data)  # 默认回退到 1.1.0
        stable = self.stable_manifest()
        self.assertEqual(stable["version"], "1.1.0")
        self.assertEqual(stable["notes"], "notes-1.1.0")
        self.assertTrue(all(a["path"].startswith("files/1.1.0/")
                            for a in stable["artifacts"]))
        self.assertEqual([h["version"] for h in stable["history"]],
                         ["1.0.0", "1.1.0", "1.2.0"])

    def test_rollback_deterministic(self):
        quiet(publish.do_rollback, self.data, "1.1.0")
        first = self.stable_bytes()
        quiet(publish.do_rollback, self.data, "1.1.0")
        self.assertEqual(self.stable_bytes(), first)

    def test_rollback_target_announcement_wins(self):
        # 1.1.0 曾以 notes-1.1.0 归档；同版本重发换公告后回退到 1.1.0，
        # 顶层公告与 history 条目都应以归档目标为准，而不是重发版本
        rerelease = self.new_release("r11b", version="1.1.0",
                                     contents={("linux", "arm64", "system-qt"): b"pkg-1.1.0"})
        edit_manifest(rerelease, lambda m: m.update(notes="notes-1.1.0-revised"))
        self.publish(rerelease)  # 同哈希文件跳过，stable 换成重发的 1.1.0 公告
        self.assertEqual(self.stable_manifest()["notes"], "notes-1.1.0-revised")

        quiet(publish.do_rollback, self.data, "1.1.0")
        stable = self.stable_manifest()
        self.assertEqual(stable["notes"], "notes-1.1.0")
        self.assertEqual(self.history_entry("1.1.0")["notes"], "notes-1.1.0")
        # 高于回退目标的版本仍保留在 history 中
        self.assertIn("1.2.0", [h["version"] for h in stable["history"]])

    def test_publish_after_rollback_keeps_history(self):
        quiet(publish.do_rollback, self.data)  # → 1.1.0
        r13 = self.new_release("r13", version="1.3.0",
                               contents={("linux", "arm64", "system-qt"): b"pkg-1.3.0"})
        edit_manifest(r13, lambda m: m.update(notes="notes-1.3.0"))
        self.publish(r13)
        stable = self.stable_manifest()
        self.assertEqual(stable["version"], "1.3.0")
        self.assertEqual([h["version"] for h in stable["history"]],
                         ["1.0.0", "1.1.0", "1.2.0", "1.3.0"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
