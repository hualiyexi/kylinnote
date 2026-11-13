#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""KylinList 内网更新发布工具（仅 Python 标准库）。

以隔离模式运行：python -I /tools/publish.py <命令> ...

数据目录默认 /data（compose 挂载 server/data），可用环境变量
KYLIN_UPDATE_DATA_DIR 覆盖（主要供测试使用）。

发布输入是 CI 产出的 release 目录，结构（见 server/README.md）：

    <release-dir>/
      updates/
        v1/stable/manifest.json
        files/<版本>/<platform>-<arch>-<variant>/<原文件名>
      RELEASE-INFO.json        # 可选；只透传归档，不解析、不强依赖

manifest 内 artifact.path 相对于「服务器基础地址 + /updates/」。
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

DEFAULT_DATA_DIR = "/data"
ENV_DATA_DIR = "KYLIN_UPDATE_DATA_DIR"

VERSION_RE = re.compile(r"^(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
# 归档文件名 manifest-<版本>-<UTC时间戳>.json，末尾 -N 为同秒冲突时的序号
ARCHIVE_RE = re.compile(r"^manifest-(\d+\.\d+\.\d+)-(\d{8}T\d{6}Z)(?:-\d+)?\.json$")

PLATFORMS = ("linux", "windows")
ARCHES = ("arm64", "amd64")
VARIANTS = ("system-qt", "bundled-qt", "setup", "portable")


class PublishError(Exception):
    """可预期的发布错误，消息直接展示给用户。"""


# ---------------------------------------------------------------- 基础工具

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def atomic_write_bytes(path, data):
    """临时文件 + os.replace 的原子写；先写同目录临时文件再替换。"""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix="." + path.name + ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o644)  # mkstemp 默认 0600，nginx worker 非root需要可读
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def utc_timestamp():
    return time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())


def unique_archive_path(archive_dir, prefix, ts, suffix):
    base = "%s-%s" % (prefix, ts)
    cand = archive_dir / (base + suffix)
    n = 1
    while cand.exists():
        cand = archive_dir / ("%s-%d%s" % (base, n, suffix))
        n += 1
    return cand


def version_tuple(version):
    return tuple(int(x) for x in version.split("."))


# ---------------------------------------------------------------- 清单校验

def validate_manifest(manifest, source="manifest.json"):
    """严格校验清单结构，返回 (version, 规范化 artifacts 列表)。任何不符合直接抛错。"""

    def err(msg):
        raise PublishError("%s: %s" % (source, msg))

    if not isinstance(manifest, dict):
        err("清单必须是 JSON 对象")
    schema = manifest.get("schemaVersion")
    if type(schema) is not int or schema != 1:
        err("schemaVersion 必须为整数 1")
    if manifest.get("channel") != "stable":
        err('channel 必须为 "stable"')
    version = manifest.get("version")
    if not isinstance(version, str) or not VERSION_RE.match(version):
        err("version 非法: %r（应形如 1.2.0，各段无前导零）" % (version,))
    arts = manifest.get("artifacts")
    if not isinstance(arts, list) or not arts:
        err("artifacts 必须为非空数组")

    seen = set()
    normalized = []
    for i, art in enumerate(arts):
        prefix = "%s: artifacts[%d]" % (source, i)

        def aerr(msg, prefix=prefix):
            raise PublishError("%s: %s" % (prefix, msg))

        if not isinstance(art, dict):
            aerr("必须是 JSON 对象")
        platform = art.get("platform")
        arch = art.get("arch")
        variant = art.get("variant")
        if platform not in PLATFORMS:
            aerr("platform 非法: %r（允许 %s）" % (platform, "/".join(PLATFORMS)))
        if arch not in ARCHES:
            aerr("arch 非法: %r（允许 %s）" % (arch, "/".join(ARCHES)))
        if variant not in VARIANTS:
            aerr("variant 非法: %r（允许 %s）" % (variant, "/".join(VARIANTS)))
        key = (platform, arch, variant)
        if key in seen:
            aerr("(platform, arch, variant) 重复: %s" % "-".join(key))
        seen.add(key)
        if art.get("packageVersion") != version:
            aerr("packageVersion 必须等于 version（%s）" % version)

        path = art.get("path")
        if not isinstance(path, str) or not path:
            aerr("path 必须是非空字符串")
        if path.startswith("/") or "\\" in path:
            aerr("path 不得是绝对路径或包含反斜杠")
        target = "%s-%s-%s" % (platform, arch, variant)
        parts = path.split("/")
        if (len(parts) != 4 or parts[0] != "files" or parts[1] != version
                or parts[2] != target or not parts[3]):
            aerr("path 必须形如 files/%s/%s/<文件名>" % (version, target))
        if any(p in (".", "..") for p in parts):
            aerr("path 不得包含 . 或 .. 片段")

        size = art.get("size")
        if type(size) is not int or size < 0:
            aerr("size 必须是非负整数")
        sha = art.get("sha256")
        if not isinstance(sha, str) or not SHA256_RE.match(sha):
            aerr("sha256 必须是 64 位小写十六进制字符串")

        normalized.append({
            "platform": platform,
            "arch": arch,
            "variant": variant,
            "packageVersion": art.get("packageVersion"),
            "path": path,
            "size": size,
            "sha256": sha,
        })
    return version, normalized


def load_release(release_dir):
    """读取并校验 release 目录的清单，返回 (manifest_bytes, version, artifacts)。"""
    release_dir = Path(release_dir)
    manifest_path = release_dir / "updates" / "v1" / "stable" / "manifest.json"
    if not manifest_path.is_file():
        raise PublishError("未找到发布清单: %s" % manifest_path)
    try:
        manifest_bytes = manifest_path.read_bytes()
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as e:
        raise PublishError("manifest.json 读取或解析失败: %s" % e)
    version, artifacts = validate_manifest(manifest)
    return manifest_bytes, version, artifacts


def verify_release_files(release_dir, version, artifacts):
    """逐个核对 release 目录内的 artifact 文件存在且 size/sha256 与清单一致。"""
    updates = Path(release_dir) / "updates"
    for art in artifacts:
        src = updates / art["path"]
        if not src.is_file():
            raise PublishError("清单引用的文件缺失: %s" % art["path"])
        actual_size = src.stat().st_size
        if actual_size != art["size"]:
            raise PublishError("文件大小不符: %s（清单 %d，实际 %d）"
                               % (art["path"], art["size"], actual_size))
        actual_sha = sha256_file(src)
        if actual_sha != art["sha256"]:
            raise PublishError("sha256 不符: %s（清单 %s，实际 %s）"
                               % (art["path"], art["sha256"], actual_sha))


# ---------------------------------------------------------------- 安装

def install_file(src, dst, sha, rel):
    """把单个安装包安装到 data/public：临时文件写入并校验后原子落位，再回读确认。"""
    dst.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(dst.parent), prefix="." + dst.name + ".", suffix=".part")
    tmp_path = Path(tmp)
    try:
        with os.fdopen(fd, "wb") as out, open(src, "rb") as fin:
            shutil.copyfileobj(fin, out, 1024 * 1024)
            out.flush()
            os.fsync(out.fileno())
        os.chmod(tmp, 0o644)  # mkstemp 默认 0600，nginx worker 非root需要可读
        if sha256_file(tmp_path) != sha:
            raise PublishError("复制后哈希不符（写入异常）: %s" % rel)
        os.replace(tmp, dst)
    except BaseException:
        try:
            tmp_path.unlink()
        except OSError:
            pass
        raise
    if sha256_file(dst) != sha:
        raise PublishError("安装后回读哈希不符: %s" % rel)
    print("  已安装: %s" % rel)


# ---------------------------------------------------------------- 各命令实现

def do_validate(release_dir):
    """只做清单结构与文件一致性检查，不改动数据目录。"""
    _, version, artifacts = load_release(release_dir)
    verify_release_files(release_dir, version, artifacts)
    print("校验通过: 版本 %s，%d 个文件大小与 sha256 均与清单一致" % (version, len(artifacts)))


def do_publish(release_dir, data_dir):
    """发布 release 目录：先全部校验，再安装文件，最后原子替换 stable 清单。"""
    release_dir = Path(release_dir)
    data_dir = Path(data_dir)
    manifest_bytes, version, artifacts = load_release(release_dir)
    verify_release_files(release_dir, version, artifacts)

    updates_root = data_dir / "public" / "updates"
    stable_path = updates_root / "v1" / "stable" / "manifest.json"

    # 1) 安装安装包文件（同哈希幂等，不同哈希拒绝原地替换）
    print("校验通过，开始安装 %d 个文件（版本 %s）" % (len(artifacts), version))
    for art in artifacts:
        src = release_dir / "updates" / art["path"]
        dst = updates_root / art["path"]
        if dst.exists():
            if not dst.is_file():
                raise PublishError("目标已存在且不是普通文件: %s" % dst)
            current = sha256_file(dst)
            if current == art["sha256"]:
                print("  已存在且一致，跳过: %s" % art["path"])
                continue
            raise PublishError(
                "目标已存在但内容不同，拒绝替换: %s（同版本禁止原地替换，"
                "请升版本号或先人工清理）" % art["path"])
        install_file(src, dst, art["sha256"], art["path"])

    # 2) 幂等：stable 清单与本次完全一致则直接结束
    if stable_path.is_file() and stable_path.read_bytes() == manifest_bytes:
        print("stable 清单已是 %s（内容一致），发布幂等完成" % version)
        return

    # 3) 归档当前 stable 清单（如有）
    ts = utc_timestamp()
    if stable_path.is_file():
        old_bytes = stable_path.read_bytes()
        old_version = _manifest_version_of(old_bytes, "当前 stable 清单")
        archive_dir = updates_root / "v1" / "archive"
        dst = unique_archive_path(archive_dir, "manifest-%s" % old_version, ts, ".json")
        atomic_write_bytes(dst, old_bytes)
        print("已归档原 stable 清单 %s -> archive/%s" % (old_version, dst.name))

    # 4) 原子替换 stable 清单（任何前置步骤失败都不会走到这里）
    atomic_write_bytes(stable_path, manifest_bytes)

    # 5) RELEASE-INFO.json 透传归档：不解析、不强依赖，存在即原样归档
    info_path = release_dir / "RELEASE-INFO.json"
    if info_path.is_file():
        archive_dir = updates_root / "v1" / "archive"
        info_dst = unique_archive_path(archive_dir, "release-info-%s" % version, ts, ".json")
        atomic_write_bytes(info_dst, info_path.read_bytes())
        print("已透传归档 RELEASE-INFO.json -> archive/%s" % info_dst.name)

    print("发布完成: stable 版本 -> %s" % version)


def _manifest_version_of(data, source):
    try:
        manifest = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        raise PublishError("%s 解析失败: %s" % (source, e))
    version = manifest.get("version") if isinstance(manifest, dict) else None
    if not isinstance(version, str):
        raise PublishError("%s 缺少 version 字段" % source)
    return version


def list_archives(archive_dir):
    """返回归档清单列表 [(版本元组, 版本串, 时间戳, 路径)]，按时间戳升序。"""
    result = []
    archive_dir = Path(archive_dir)
    if archive_dir.is_dir():
        for p in archive_dir.iterdir():
            m = ARCHIVE_RE.match(p.name)
            if p.is_file() and m:
                result.append((version_tuple(m.group(1)), m.group(1), m.group(2), p))
    result.sort(key=lambda item: (item[2], item[1]))
    return result


def do_rollback(data_dir, to_version=None):
    """把 stable 清单回退到归档中的旧版本；默认取最近一份早于当前版本的归档。"""
    data_dir = Path(data_dir)
    updates_root = data_dir / "public" / "updates"
    stable_path = updates_root / "v1" / "stable" / "manifest.json"

    current_version = None
    if stable_path.is_file():
        current_version = _manifest_version_of(stable_path.read_bytes(), "当前 stable 清单")

    archives = list_archives(updates_root / "v1" / "archive")
    if not archives:
        raise PublishError("没有可用的归档清单，无法回退")

    if to_version is not None:
        if not VERSION_RE.match(to_version):
            raise PublishError("--to 版本号非法: %r" % to_version)
        candidates = [a for a in archives if a[1] == to_version]
        if not candidates:
            known = ", ".join(sorted({a[1] for a in archives}))
            raise PublishError("归档中没有版本 %s（可用: %s）" % (to_version, known or "无"))
        chosen = candidates[-1]
    else:
        if current_version is None:
            raise PublishError("当前没有 stable 清单，请用 --to <版本> 明确回退目标")
        current = version_tuple(current_version)
        candidates = [a for a in archives if a[0] < current]
        if not candidates:
            raise PublishError("没有早于 %s 的归档版本" % current_version)
        chosen = candidates[-1]

    # 回退目标清单本身仍需合法，且其指向的文件必须还在
    archive_bytes = chosen[3].read_bytes()
    manifest = json.loads(archive_bytes.decode("utf-8"))
    version, artifacts = validate_manifest(manifest, source=chosen[3].name)
    for art in artifacts:
        if not (updates_root / art["path"]).is_file():
            raise PublishError("回退目标文件缺失: %s（不要手工删除 files/ 下的历史版本）" % art["path"])

    atomic_write_bytes(stable_path, archive_bytes)
    before = current_version if current_version is not None else "（无）"
    print("回退完成: %s -> %s（使用归档 %s）" % (before, version, chosen[3].name))


def do_status(data_dir):
    data_dir = Path(data_dir)
    updates_root = data_dir / "public" / "updates"
    stable_path = updates_root / "v1" / "stable" / "manifest.json"
    print("数据目录: %s" % data_dir)
    if stable_path.is_file():
        try:
            manifest = json.loads(stable_path.read_bytes().decode("utf-8"))
            version, artifacts = validate_manifest(manifest)
        except (PublishError, UnicodeDecodeError, ValueError) as e:
            print("当前 stable 清单存在但无法解析: %s" % e)
        else:
            print("当前 stable 版本: %s" % version)
            for art in artifacts:
                print("  - %s-%s-%s  %d 字节  %s"
                      % (art["platform"], art["arch"], art["variant"], art["size"], art["path"]))
    else:
        print("当前 stable 版本: （尚未发布）")
    archives = list_archives(updates_root / "v1" / "archive")
    print("归档清单 (%d 份):" % len(archives))
    for _, version, ts, p in reversed(archives):
        print("  %s  %s  %s" % (version, ts, p.name))


# ---------------------------------------------------------------- CLI

def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="publish.py",
        description="KylinList 内网更新发布工具（仅标准库；数据目录默认 /data）")
    sub = parser.add_subparsers(dest="command", metavar="命令", required=True)

    p = sub.add_parser("publish", help="校验并发布一个 release 目录")
    p.add_argument("release_dir", metavar="release目录",
                   help="CI 产物解包出的 release 目录（容器内路径，如 /data/incoming/xxx）")

    p = sub.add_parser("rollback", help="把 stable 清单回退到归档中的旧版本")
    p.add_argument("--to", dest="to_version", metavar="版本", default=None,
                   help="精确回退到指定版本；默认取最近一份早于当前版本的归档")

    p = sub.add_parser("validate", help="只校验 release 目录，不落盘")
    p.add_argument("release_dir", metavar="release目录")

    sub.add_parser("status", help="查看当前 stable 版本与归档列表")

    args = parser.parse_args(argv)
    data_dir = Path(os.environ.get(ENV_DATA_DIR, DEFAULT_DATA_DIR))
    try:
        if args.command == "publish":
            do_publish(Path(args.release_dir), data_dir)
        elif args.command == "rollback":
            do_rollback(data_dir, args.to_version)
        elif args.command == "validate":
            do_validate(Path(args.release_dir))
        elif args.command == "status":
            do_status(data_dir)
    except PublishError as e:
        print("错误: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
