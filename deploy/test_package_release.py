#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""deploy/package_release.py 的本地回归测试（仅标准库）。

运行：python -I deploy/test_package_release.py

全部在临时目录进行：把真实的 package_release.py、server/tools/publish.py
和一个 ci_release.py 测试替身组装成沙箱仓库，再以 `python -I` 子进程跑
完整流水线。不打 Docker、不联网、不触碰真实 deploy/data。
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DEPLOY_DIR = Path(__file__).resolve().parent
ROOT = DEPLOY_DIR.parent

# ci_release.py 测试替身：模拟另一模块提供的 validate_ci_release 接口。
# partial=true 时拒绝，其余返回 (version, info)，与真实语义的关键面一致。
CI_STUB = '''\
# -*- coding: utf-8 -*-
"""测试替身：validate_ci_release(release_dir) -> (version, info)。"""
import json
import os


def validate_ci_release(release_dir):
    info_path = os.path.join(str(release_dir), "RELEASE-INFO.json")
    if not os.path.isfile(info_path):
        raise ValueError("missing RELEASE-INFO.json")
    with open(info_path, "r", encoding="utf-8") as f:
        info = json.load(f)
    if info.get("partial") is True:
        raise ValueError("partial release must never be published")
    version = info.get("version")
    if not isinstance(version, str):
        raise ValueError("RELEASE-INFO.json has no version")
    return version, info
'''

# (platform, arch, variant, 文件名模板)——与 CI 六目标一一对应。
TARGET_FILES = (
    ("linux", "arm64", "system-qt", "kylinlist_{v}_arm64.deb"),
    ("linux", "arm64", "bundled-qt", "kylinlist_{v}_arm64_bundle.deb"),
    ("linux", "amd64", "system-qt", "kylinlist_{v}_amd64.deb"),
    ("linux", "amd64", "bundled-qt", "kylinlist_{v}_amd64_bundle.deb"),
    ("windows", "amd64", "setup", "KylinList-{v}-Windows-x64-Setup.exe"),
    ("windows", "amd64", "portable", "KylinList-{v}-Windows-x64.zip"),
)


def build_release(dest, version, partial=False, skip_target=None, real_ci=False):
    """构造一个能通过 publish.py validate 的六目标 release 目录。

    real_ci=True 时附带能通过真实 deploy/ci_release.py 校验的 ci 元数据。
    """
    dest = Path(dest)
    artifacts = []
    for platform, arch, variant, name_fmt in TARGET_FILES:
        target = "%s-%s-%s" % (platform, arch, variant)
        if target == skip_target:
            continue
        name = name_fmt.format(v=version)
        data = ("kylinlist %s %s\n" % (version, target)).encode("utf-8") * 5
        rel = "files/%s/%s/%s" % (version, target, name)
        path = dest / "updates" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        artifacts.append({
            "platform": platform, "arch": arch, "variant": variant,
            "packageVersion": version, "path": rel,
            "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
        })
    manifest = {
        "schemaVersion": 1, "channel": "stable", "version": version,
        "publishedAt": "20260101T000000Z", "notes": "test release",
        "artifacts": artifacts,
    }
    mdir = dest / "updates" / "v1" / "stable"
    mdir.mkdir(parents=True, exist_ok=True)
    (mdir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    info = {
        "version": version, "gitSha": "0" * 40,
        "generatedAt": "20260101T000000Z", "partial": partial,
        "artifacts": [{"target": "%s-%s-%s" % (a["platform"], a["arch"], a["variant"]),
                       "file": a["path"].rsplit("/", 1)[1],
                       "sha256": a["sha256"], "size": a["size"]}
                      for a in artifacts],
    }
    if real_ci:
        info["ci"] = {
            "repository": "hualiyexi/kylinnote",
            "workflow": "Build and package",
            "workflowPath": ".github/workflows/build.yml",
            "runId": "1234567890",
            "runUrl": "https://github.com/hualiyexi/kylinnote/actions/runs/1234567890",
            "ref": "refs/tags/v7.7.7",
            "eventName": "push",
            "headSha": "0" * 40,
        }
    (dest / "RELEASE-INFO.json").write_text(
        json.dumps(info, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest


def make_sandbox(tmp, with_stub=True):
    """组装沙箱仓库：deploy/package_release.py(+替身) 与 server/tools/publish.py。"""
    sandbox = Path(tmp)
    deploy = sandbox / "deploy"
    deploy.mkdir(parents=True, exist_ok=True)
    shutil.copy2(str(DEPLOY_DIR / "package_release.py"), str(deploy / "package_release.py"))
    if with_stub:
        (deploy / "ci_release.py").write_text(CI_STUB, encoding="utf-8")
    tools = sandbox / "server" / "tools"
    tools.mkdir(parents=True, exist_ok=True)
    shutil.copy2(str(ROOT / "server" / "tools" / "publish.py"), str(tools / "publish.py"))
    return sandbox


def run_packager(sandbox, release_dir):
    cmd = [sys.executable, "-I",
           str(sandbox / "deploy" / "package_release.py"), str(release_dir)]
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace")


def snapshot(directory):
    """目录内全部普通文件的 {相对路径: sha256} 快照。"""
    result = {}
    for dirpath, _dirnames, filenames in os.walk(str(directory)):
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, str(directory)).replace(os.sep, "/")
            result[rel] = sha256_of(path)
    return result


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def incoming_entries(sandbox):
    incoming = sandbox / "deploy" / "data" / "incoming"
    if not incoming.is_dir():
        return []
    return sorted(p.name for p in incoming.iterdir())


def expect_fail(proc, needle, label):
    assert proc.returncode != 0, "%s: 应当失败，却成功了:\n%s" % (label, proc.stdout)
    combined = (proc.stderr or "") + (proc.stdout or "")
    assert needle in combined, "%s: 输出应包含 %r:\n%s" % (label, needle, combined)


# ---------------------------------------------------------------- 测试用例

def test_happy_path(tmp):
    sandbox = make_sandbox(tmp)
    release = Path(tmp) / "release-in"
    release.mkdir()
    build_release(release, "9.9.9")
    before = snapshot(release)

    proc = run_packager(sandbox, release)
    assert proc.returncode == 0, "happy path 失败:\n%s\n%s" % (proc.stdout, proc.stderr)

    final = sandbox / "deploy" / "data" / "incoming" / "kylinlist-9.9.9"
    assert final.is_dir(), "缺少最终目录: %s" % final
    assert (final / "READY").is_file(), "缺少 READY"
    assert incoming_entries(sandbox) == ["kylinlist-9.9.9"], \
        "暂存目录未清理: %s" % incoming_entries(sandbox)
    assert snapshot(release) == before, "输入目录被修改"

    # SHA256SUMS：恰好覆盖 8 个原始文件（6 安装包 + manifest + RELEASE-INFO），
    # 相对路径、排序、双空格标准格式，不含自身/READY/state。
    lines = (final / "SHA256SUMS").read_text(encoding="utf-8").splitlines()
    originals = [rel for rel in sorted(before)]
    assert len(lines) == len(originals) == 8, "SHA256SUMS 行数异常: %r" % lines
    assert [line.split("  ", 1)[1] for line in lines] == originals
    for line in lines:
        digest, rel = line.split("  ", 1)
        assert digest == sha256_of(final.joinpath(*rel.split("/"))), rel
    assert not any("/READY" in l or "SHA256SUMS" in l or ".publish-state" in l
                   for l in lines)
    assert "kylinlist-9.9.9" in proc.stdout, "输出应包含清晰路径:\n%s" % proc.stdout

    # 与 sha256sum -c 兼容（若有该命令）。
    if shutil.which("sha256sum"):
        check = subprocess.run(["sha256sum", "-c", "SHA256SUMS"], cwd=str(final),
                               capture_output=True, text=True, errors="replace")
        assert check.returncode == 0, "sha256sum -c 失败:\n%s%s" % (check.stdout, check.stderr)


def test_refuse_existing_final(tmp):
    sandbox = make_sandbox(tmp)
    release = Path(tmp) / "release-in"
    release.mkdir()
    build_release(release, "1.2.3")
    final = sandbox / "deploy" / "data" / "incoming" / "kylinlist-1.2.3"
    final.mkdir(parents=True)
    (final / "sentinel").write_text("keep", encoding="utf-8")

    proc = run_packager(sandbox, release)
    expect_fail(proc, "拒绝覆盖已存在的发布目录", "existing-final")
    assert (final / "sentinel").read_text(encoding="utf-8") == "keep"
    assert incoming_entries(sandbox) == ["kylinlist-1.2.3"], \
        "失败后应无暂存残留: %s" % incoming_entries(sandbox)


def test_reject_partial(tmp):
    sandbox = make_sandbox(tmp)
    release = Path(tmp) / "release-in"
    release.mkdir()
    build_release(release, "2.0.0", partial=True)
    proc = run_packager(sandbox, release)
    expect_fail(proc, "partial", "partial")
    assert incoming_entries(sandbox) == [], "不应产生任何 incoming 内容"


def test_missing_ci_release_module(tmp):
    sandbox = make_sandbox(tmp, with_stub=False)
    release = Path(tmp) / "release-in"
    release.mkdir()
    build_release(release, "3.0.0")
    proc = run_packager(sandbox, release)
    expect_fail(proc, "ci_release.py", "missing-module")
    assert incoming_entries(sandbox) == []


def test_missing_target(tmp):
    sandbox = make_sandbox(tmp)
    release = Path(tmp) / "release-in"
    release.mkdir()
    build_release(release, "4.0.0", skip_target="windows-amd64-portable")
    proc = run_packager(sandbox, release)
    expect_fail(proc, "windows-amd64-portable", "missing-target")
    assert incoming_entries(sandbox) == []


def test_corrupt_artifact(tmp):
    sandbox = make_sandbox(tmp)
    release = Path(tmp) / "release-in"
    release.mkdir()
    build_release(release, "5.0.0")
    # 破坏一个安装包：publish.py validate 应发现 sha256 不符。
    victim = next((release / "updates" / "files" / "5.0.0").rglob("*.deb"))
    with open(victim, "ab") as f:
        f.write(b"corruption")
    proc = run_packager(sandbox, release)
    expect_fail(proc, "validate 失败", "corrupt-artifact")
    assert incoming_entries(sandbox) == []


def test_stale_state_file(tmp):
    sandbox = make_sandbox(tmp)
    release = Path(tmp) / "release-in"
    release.mkdir()
    build_release(release, "6.0.0")
    (release / ".publish-state.json").write_text('{"state": "succeeded"}',
                                                 encoding="utf-8")
    proc = run_packager(sandbox, release)
    expect_fail(proc, ".publish-state.json", "stale-state")
    assert incoming_entries(sandbox) == []


def test_integration_real_ci_module(tmp):
    """用仓库内真实的 deploy/ci_release.py 走通完整流水线（仍在临时目录）。"""
    real = DEPLOY_DIR / "ci_release.py"
    if not real.is_file():
        print("SKIP test_integration_real_ci_module（deploy/ci_release.py 尚未提供）")
        return
    sandbox = make_sandbox(tmp, with_stub=False)
    shutil.copy2(str(real), str(sandbox / "deploy" / "ci_release.py"))
    release = Path(tmp) / "release-in"
    release.mkdir()
    build_release(release, "7.7.7", real_ci=True)
    before = snapshot(release)

    proc = run_packager(sandbox, release)
    assert proc.returncode == 0, "真实 ci_release 集成失败:\n%s\n%s" % (
        proc.stdout, proc.stderr)
    final = sandbox / "deploy" / "data" / "incoming" / "kylinlist-7.7.7"
    assert final.is_dir() and (final / "READY").is_file()
    assert incoming_entries(sandbox) == ["kylinlist-7.7.7"]
    assert snapshot(release) == before, "输入目录被修改"


TESTS = (
    test_happy_path,
    test_refuse_existing_final,
    test_reject_partial,
    test_missing_ci_release_module,
    test_missing_target,
    test_corrupt_artifact,
    test_stale_state_file,
    test_integration_real_ci_module,
)


def main():
    failures = 0
    for test in TESTS:
        with tempfile.TemporaryDirectory(prefix="kylinlist-pkg-test-") as tmp:
            try:
                test(tmp)
                print("PASS %s" % test.__name__)
            except AssertionError as e:
                failures += 1
                print("FAIL %s\n  %s" % (test.__name__, e))
    if failures:
        print("%d/%d 项失败" % (failures, len(TESTS)))
        return 1
    print("全部 %d 项通过" % len(TESTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
