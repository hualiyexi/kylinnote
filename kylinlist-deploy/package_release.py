#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""KylinList 日常发布打包（仅 Python 标准库）。

用法（一般经 kylinlist-deploy/package.sh 薄入口以隔离模式调用）：

    python -I kylinlist-deploy/package_release.py <release目录>

release 目录是 GitHub CI 产出的不可信输入；本脚本与输入分离（不从输入目录
加载任何代码、绝不修改输入），流程：

  1. kylinlist-deploy/ci_release.py 的 validate_ci_release 校验 CI 来源：
     完整 CI 六目标、非 partial、run/headSha 元数据，返回 (version, info)；
  2. 直接读取 manifest，复核 artifacts 恰好覆盖完整 CI 六目标；
  3. 以隔离子进程运行 server/tools/publish.py validate，复核清单结构与
     安装包大小/sha256 一致；
  4. 复制到隐藏暂存 kylinlist-deploy/data/incoming/.kylinlist-<版本>.packaging-<pid>/，
     在暂存内生成标准 SHA256SUMS（覆盖全部原始文件、相对路径，不含
     自身/READY/state 文件），最后写 READY，再原子改名为
     kylinlist-deploy/data/incoming/kylinlist-<版本>/。

目标目录已存在或任何失败都拒绝覆盖并清理本次暂存；本脚本不构建任何
客户端或服务端镜像（服务端变化用 kylinlist-deploy/package-images.sh）。
"""

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

STRICT_VERSION_RE = re.compile(r"^(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)$")

# 完整 CI 六目标：4 个 Linux deb + Windows setup/portable（见
# packaging/generate-update-manifest.py 的 LINUX_LAYOUT / WINDOWS_KIND_VARIANTS）。
CI_TARGETS = (
    ("linux", "arm64", "system-qt"),
    ("linux", "arm64", "bundled-qt"),
    ("linux", "amd64", "system-qt"),
    ("linux", "amd64", "bundled-qt"),
    ("windows", "amd64", "setup"),
    ("windows", "amd64", "portable"),
)

READY_NAME = "READY"
SUMS_NAME = "SHA256SUMS"
# 输入里出现这些名字说明它是已投递/已消费过的目录（publisher 的状态文件、
# 本脚本的产物），重新打包会制造“看似已处理”的脏输入，直接拒绝。
STALE_INPUT_NAMES = (READY_NAME, SUMS_NAME, ".publish-state.json")


class PackagingError(Exception):
    """可预期的打包错误，消息直接展示给用户。"""


# ---------------------------------------------------------------- 基础工具

def repo_root():
    return Path(__file__).resolve().parent.parent


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def fsync_write(path, data):
    """写临时文件 + fsync + 原子替换，保证可见内容要么旧要么新、永不残缺。"""
    path = Path(path)
    tmp = path.parent / ("." + path.name + ".tmp")
    try:
        with open(tmp, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def is_within(child, ancestor):
    """child 是否等于 ancestor 或位于其内部（均按真实路径比较）。"""
    child = Path(os.path.realpath(str(child)))
    ancestor = Path(os.path.realpath(str(ancestor)))
    return child == ancestor or ancestor in child.parents


# ---------------------------------------------------------------- 校验步骤

def load_ci_release_validator():
    """按文件路径加载 kylinlist-deploy/ci_release.py（-I 模式下 sys.path 不含脚本目录）。

    ci_release.py 是仓库内的可信脚本，与不可信的 release 输入分离。
    """
    path = Path(__file__).resolve().parent / "ci_release.py"
    if not path.is_file():
        raise PackagingError(
            "缺少 CI 校验模块: %s（应与 fetch_ci_release.py 一并提供）" % path)
    spec = importlib.util.spec_from_file_location("kylinlist_ci_release", str(path))
    if spec is None or spec.loader is None:
        raise PackagingError("无法加载 CI 校验模块: %s" % path)
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except Exception as e:
        raise PackagingError("加载 CI 校验模块失败: %s: %s" % (path, e))
    func = getattr(module, "validate_ci_release", None)
    if not callable(func):
        raise PackagingError(
            "ci_release.py 未提供 validate_ci_release(release_dir) -> (version, info)")
    return func


def validate_ci(release_dir):
    """调用另一模块的 CI 校验器，返回 release 版本号。"""
    validator = load_ci_release_validator()
    try:
        version, _info = validator(str(release_dir))
    except ValueError as e:
        raise PackagingError(
            "CI 校验未通过（必须是完整六目标、非 partial 的 CI release）: %s" % e)
    if not isinstance(version, str) or not STRICT_VERSION_RE.match(version):
        raise PackagingError("CI 校验返回的版本号非法: %r" % (version,))
    return version


def validate_six_targets(release_dir):
    """读取 manifest，复核 artifacts 恰好等于完整 CI 六目标。"""
    manifest_path = Path(release_dir) / "updates" / "v1" / "stable" / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_bytes().decode("utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as e:
        raise PackagingError("manifest.json 读取或解析失败: %s" % e)
    found = set()
    for i, art in enumerate(manifest.get("artifacts") or ()):
        if isinstance(art, dict):
            found.add((art.get("platform"), art.get("arch"), art.get("variant")))
    expected = set(CI_TARGETS)
    missing = sorted(expected - found)
    extra = sorted(found - expected)
    if missing:
        raise PackagingError(
            "release 缺少 CI 目标: %s（必须完整六目标）"
            % ", ".join("-".join(t) for t in missing))
    if extra:
        raise PackagingError(
            "release 含 CI 六目标之外的产物: %s"
            % ", ".join("-".join(t) for t in extra))


def run_publish_validate(root, release_dir):
    """隔离子进程运行现有校验器，核对清单 hash 与安装包一致性。"""
    publish = root / "server" / "tools" / "publish.py"
    if not publish.is_file():
        raise PackagingError("未找到现有校验器: %s" % publish)
    cmd = [sys.executable, "-I", str(publish), "validate", str(release_dir)]
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "").strip()
        raise PackagingError("publish.py validate 失败（退出码 %d）:\n%s"
                             % (proc.returncode, detail))
    return (proc.stdout or "").strip()


# ---------------------------------------------------------------- 复制与落位

def copy_release(release_dir, staging):
    """把输入完整复制进暂存，拒绝符号链接与非普通文件；返回相对路径列表。

    只读输入、只写暂存；相对路径统一用 "/"，供 SHA256SUMS 使用。
    """
    release_dir = Path(release_dir)
    copied = []
    for dirpath, dirnames, filenames in os.walk(str(release_dir)):
        dirnames.sort()
        filenames.sort()
        for name in dirnames:
            if os.path.islink(os.path.join(dirpath, name)):
                rel = os.path.relpath(os.path.join(dirpath, name), str(release_dir))
                raise PackagingError("输入包含符号链接目录，拒绝: %s" % rel)
        for name in filenames:
            src = os.path.join(dirpath, name)
            rel = os.path.relpath(src, str(release_dir)).replace(os.sep, "/")
            if os.path.islink(src):
                raise PackagingError("输入包含符号链接文件，拒绝: %s" % rel)
            if not os.path.isfile(src):
                raise PackagingError("输入包含非普通文件，拒绝: %s" % rel)
            dst = staging.joinpath(*rel.split("/"))
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            copied.append(rel)
    return sorted(copied)


def write_sha256sums(staging, copied):
    """在暂存内生成标准 sha256sum 清单：只覆盖本次复制的原始文件。"""
    lines = ["%s  %s" % (sha256_file(staging.joinpath(*rel.split("/"))), rel)
             for rel in copied]
    data = ("\n".join(lines) + "\n").encode("utf-8")
    fsync_write(staging / SUMS_NAME, data)


def write_ready(staging):
    """最后写 READY 就绪标记（空文件，fsync 落盘后才算提交）。"""
    with open(staging / READY_NAME, "wb") as f:
        f.flush()
        os.fsync(f.fileno())


def package(release_dir):
    """完整流水线；返回 (version, 文件数, 最终目录)。"""
    root = repo_root()
    incoming = root / "kylinlist-deploy" / "data" / "incoming"
    release = Path(os.path.realpath(str(release_dir)))

    if not release.is_dir():
        raise PackagingError("release 目录不存在或不是目录: %s" % release_dir)
    if is_within(incoming, release):
        raise PackagingError(
            "输入目录不得是 kylinlist-deploy/data 的上级或包含它（%s）" % release)
    for name in STALE_INPUT_NAMES:
        if (release / name).exists():
            raise PackagingError(
                "输入已含 %s（像是已投递/已消费的目录），请用 "
                "fetch_ci_release.py 解包全新的 CI release" % name)

    print("CI 校验: %s" % release)
    version = validate_ci(release)
    validate_six_targets(release)
    print("现有校验器复核 manifest 与安装包哈希…")
    run_publish_validate(root, release)

    final_dir = incoming / ("kylinlist-%s" % version)
    if os.path.lexists(str(final_dir)):
        raise PackagingError("拒绝覆盖已存在的发布目录: %s" % final_dir)
    staging = incoming / (".kylinlist-%s.packaging-%d" % (version, os.getpid()))
    if os.path.lexists(str(staging)):
        raise PackagingError("暂存目录已存在，拒绝复用: %s" % staging)

    incoming.mkdir(parents=True, exist_ok=True)
    try:
        staging.mkdir()
        copied = copy_release(release, staging)
        if not copied:
            raise PackagingError("输入目录为空: %s" % release)
        write_sha256sums(staging, copied)
        write_ready(staging)
        if os.path.lexists(str(final_dir)):
            raise PackagingError("拒绝覆盖已存在的发布目录: %s" % final_dir)
        try:
            os.rename(str(staging), str(final_dir))
        except OSError as e:
            raise PackagingError(
                "原子改名失败（目标已存在或同盘受限）: %s -> %s: %s"
                % (staging, final_dir, e))
    except BaseException:
        shutil.rmtree(str(staging), ignore_errors=True)
        raise
    return version, len(copied), final_dir


# ---------------------------------------------------------------- CLI

def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="package_release.py",
        description="校验 CI release 并投递到 kylinlist-deploy/data/incoming（不构建镜像）")
    parser.add_argument("release_dir", metavar="release目录",
                        help="CI 产出的 release 目录（不可信输入，只读不修改）")
    args = parser.parse_args(argv)
    try:
        version, count, final_dir = package(args.release_dir)
    except (PackagingError, OSError) as e:
        print("错误: %s" % e, file=sys.stderr)
        return 1
    print("打包完成: 版本 %s，%d 个文件" % (version, count))
    print("  输入目录: %s" % os.path.realpath(str(args.release_dir)))
    print("  发布目录: %s" % final_dir)
    print("投递到生产机: 复制到 data/incoming/.kylinlist-%s.uploading/ 后同盘原子改名"
          % version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
