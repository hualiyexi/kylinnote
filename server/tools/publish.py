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

stable 清单自带可选的 history 数组（每项 {version, publishedAt, notes}，
按版本升序）：publish 会汇总新 release、当前 stable 与全部归档的版本公告
（同版本冲突取最近一次发布），rollback 保留全部已知历史、artifacts 仍取
归档目标版本。

watch 命令轮询 incoming 目录自动发布 CI release：

    <incoming>/kylinlist-<X.Y.Z>/     # 目录名必须严格匹配，隐藏目录忽略
      READY                            # 就绪标记，缺了不处理
      SHA256SUMS                       # 标准 sha256sum 文本，覆盖目录内全部原始文件
      RELEASE-INFO.json                # 必须通过 CI 流程门禁（partial=false 等）
      updates/...                      # 与手工发布相同的 release 结构
      .publish-state.json              # watch 写入的目录状态（publishing/succeeded/...）

publish / rollback / watch 通过 data/.publisher.lock 跨进程互斥；watch 只把
RELEASE-INFO 的 CI 一致性检查当流程门禁，不构成来源签名。
"""

import argparse
import contextlib
import hashlib
import json
import logging
import os
import re
import shutil
import signal
import sys
import tempfile
import time
from pathlib import Path

DEFAULT_DATA_DIR = "/data"
ENV_DATA_DIR = "KYLIN_UPDATE_DATA_DIR"

VERSION_RE = re.compile(r"^(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
# 归档文件名 manifest-<版本>-<UTC时间戳>.json，末尾 -N 为同秒冲突时的序号（按数值递增）
ARCHIVE_RE = re.compile(r"^manifest-(\d+\.\d+\.\d+)-(\d{8}T\d{6}Z)(?:-(\d+))?\.json$")

PLATFORMS = ("linux", "windows")
ARCHES = ("arm64", "amd64")
VARIANTS = ("system-qt", "bundled-qt", "setup", "portable")

# ---------------------------------------------------------------- watch 常量
LOCK_FILENAME = ".publisher.lock"       # data/ 下，publish/rollback/watch 共用
STATE_FILENAME = ".publish-state.json"  # 每个 incoming 候选目录内的状态文件
READY_FILENAME = "READY"
SUMS_FILENAME = "SHA256SUMS"
DEFAULT_WATCH_INTERVAL = 30             # 秒

# 状态文件取值；succeeded/failed/skipped 为终态，后续轮次不再处理
STATE_PUBLISHING = "publishing"
STATE_SUCCEEDED = "succeeded"
STATE_FAILED = "failed"
STATE_SKIPPED = "skipped"
TERMINAL_STATES = frozenset({STATE_SUCCEEDED, STATE_FAILED, STATE_SKIPPED})

# watch 只认本仓库 build.yml 的完整产物：六个 target 精确齐全
CI_REPOSITORY = "hualiyexi/kylinnote"
CI_WORKFLOW_PATH = ".github/workflows/build.yml"
CI_RUN_URL_TEMPLATE = "https://github.com/%s/actions/runs/%s"
GIT_SHA_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
REQUIRED_TARGETS = (
    "linux-arm64-system-qt",
    "linux-arm64-bundled-qt",
    "linux-amd64-system-qt",
    "linux-amd64-bundled-qt",
    "windows-amd64-setup",
    "windows-amd64-portable",
)
# sha256sum 文本行：<64位小写hex><空格><空格|*><相对路径>
SUMS_LINE_RE = re.compile(r"^([0-9a-f]{64}) ([ *])(.+)$")
# SHA256SUMS 必须至少覆盖清单与 RELEASE-INFO
SUMS_REQUIRED_ENTRIES = ("RELEASE-INFO.json", "updates/v1/stable/manifest.json")
# 状态文件与就绪标记不参与 SHA256SUMS 覆盖核对
SUMS_EXCLUDED_FILES = frozenset({READY_FILENAME, SUMS_FILENAME, STATE_FILENAME})


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


def utc_iso_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


# ---------------------------------------------------------------- 跨进程锁

class PublisherLock(object):
    """data/.publisher.lock 跨进程互斥锁。

    Linux 用 flock(LOCK_EX)；Windows 用 msvcrt 对文件首字节加锁，
    语义足够单机互斥（手工 CLI 与 watch 不会同时改写数据目录）。
    """

    def __init__(self, path):
        self.path = Path(path)
        self._fd = None

    @property
    def held(self):
        return self._fd is not None

    def acquire(self):
        """尝试加锁（非阻塞）。成功返回 True；被占用或出错返回 False。"""
        if self._fd is not None:
            return True
        self.path.parent.mkdir(parents=True, exist_ok=True)
        flags = os.O_RDWR | os.O_CREAT
        if hasattr(os, "O_BINARY"):
            flags |= os.O_BINARY
        try:
            fd = os.open(str(self.path), flags, 0o644)
        except OSError:
            return False
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(fd)
            return False
        self._fd = fd
        return True

    def release(self):
        if self._fd is None:
            return
        fd, self._fd = self._fd, None
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass
        finally:
            os.close(fd)


def publisher_lock_guard(data_dir, purpose="发布"):
    """手工 CLI 用的加锁保护：锁被占用时给出明确错误。"""
    lock = PublisherLock(Path(data_dir) / LOCK_FILENAME)

    @contextlib.contextmanager
    def _guard():
        if not lock.acquire():
            raise PublishError(
                "%s失败：数据目录正被另一个发布进程使用（锁文件 %s）。"
                "请等它结束、或停止正在运行的 watch 后重试"
                % (purpose, lock.path))
        try:
            yield lock
        finally:
            lock.release()

    return _guard()


# ---------------------------------------------------------------- 清单校验

def validate_manifest(manifest, source="manifest.json"):
    """严格校验清单结构，返回 (version, 规范化 artifacts, 规范化 history 或 None)。

    history 是可选字段：新发布输入可以不带；一旦带就必须是合法数组。
    任何不符合直接抛错。
    """

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
    published_at = manifest.get("publishedAt")
    if not isinstance(published_at, str) or not published_at:
        err("publishedAt 必须是非空字符串")
    notes = manifest.get("notes")
    if not isinstance(notes, str):
        err("notes 必须是字符串（可为空）")
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

    history = manifest.get("history")
    normalized_history = None
    if history is not None:
        if not isinstance(history, list):
            err("history 必须是数组")
        seen_versions = set()
        normalized_history = []
        for i, item in enumerate(history):
            prefix = "%s: history[%d]" % (source, i)

            def herr(msg, prefix=prefix):
                raise PublishError("%s: %s" % (prefix, msg))

            if not isinstance(item, dict):
                herr("必须是 JSON 对象")
            hversion = item.get("version")
            if not isinstance(hversion, str) or not VERSION_RE.match(hversion):
                herr("version 非法: %r（应形如 1.2.0，各段无前导零）" % (hversion,))
            if hversion in seen_versions:
                herr("同版本重复出现: %s（同版本公告不得重复）" % hversion)
            seen_versions.add(hversion)
            hpublished = item.get("publishedAt")
            if not isinstance(hpublished, str) or not hpublished:
                herr("publishedAt 必须是非空字符串")
            hnotes = item.get("notes")
            if not isinstance(hnotes, str):
                herr("notes 必须是字符串（可为空）")
            normalized_history.append({
                "version": hversion,
                "publishedAt": hpublished,
                "notes": hnotes,
            })
    return version, normalized, normalized_history


def load_release(release_dir):
    """读取并校验 release 目录的清单，返回 (manifest dict, version, artifacts)。"""
    release_dir = Path(release_dir)
    manifest_path = release_dir / "updates" / "v1" / "stable" / "manifest.json"
    if not manifest_path.is_file():
        raise PublishError("未找到发布清单: %s" % manifest_path)
    try:
        manifest_bytes = manifest_path.read_bytes()
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as e:
        raise PublishError("manifest.json 读取或解析失败: %s" % e)
    version, artifacts, _ = validate_manifest(manifest)
    return manifest, version, artifacts


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


# ---------------------------------------------------------------- 历史公告合并

def merge_history(sources):
    """把若干清单的版本公告合并成一份完整的 history。

    sources 是按优先级从高到低排列的清单 dict（均已通过 validate_manifest）。
    每份清单先取顶层公告（version/publishedAt/notes），再取其 history 各项；
    同版本公告冲突时保留优先级更高来源的条目（即"最近一次发布/当前新 release"）。
    返回按版本数值升序排序的 history 列表，结果只取决于 sources 顺序，
    与文件遍历顺序无关。
    """
    merged = {}
    for manifest in sources:
        candidates = [{
            "version": manifest["version"],
            "publishedAt": manifest["publishedAt"],
            "notes": manifest["notes"],
        }]
        for item in manifest.get("history") or ():
            candidates.append({
                "version": item["version"],
                "publishedAt": item["publishedAt"],
                "notes": item["notes"],
            })
        for cand in candidates:
            if cand["version"] not in merged:
                merged[cand["version"]] = cand
    return [merged[v] for v in sorted(merged, key=version_tuple)]


def render_manifest(manifest, history):
    """生成带完整 history 的最终清单字节；落盘前自检，保证写出的清单始终合法。"""
    final = dict(manifest)
    final["history"] = [dict(item) for item in history]
    data = json.dumps(final, ensure_ascii=False, indent=2).encode("utf-8")
    _parse_manifest_bytes(data, "最终 stable 清单")
    return data


def do_publish(release_dir, data_dir):
    """发布 release 目录（自动持有发布锁）。

    stable 清单始终带完整 history：汇总新 release、当前 stable 与全部归档的
    版本公告（同版本冲突取最近一次发布），先生成最终字节再做幂等比较，
    重复发布同一 release 不会改写 stable、也不会归档自己。
    """
    with publisher_lock_guard(data_dir):
        return _do_publish_locked(release_dir, data_dir)


def _do_publish_locked(release_dir, data_dir):
    """do_publish 的实现体；调用方必须已持有发布锁（watch 复用，不重复加锁）。"""
    release_dir = Path(release_dir)
    data_dir = Path(data_dir)
    manifest, version, artifacts = load_release(release_dir)
    verify_release_files(release_dir, version, artifacts)

    updates_root = data_dir / "public" / "updates"
    stable_path = updates_root / "v1" / "stable" / "manifest.json"

    # 1) 汇总版本公告，生成带完整 history 的最终清单字节。
    #    优先级：新 release > 当前 stable > 归档（按时间新->旧）。
    #    任何来源（stable/归档）损坏都在此明确报错，绝不静默丢历史。
    stable_bytes = stable_path.read_bytes() if stable_path.is_file() else None
    stable_manifest = (_parse_manifest_bytes(stable_bytes, "当前 stable 清单")
                       if stable_bytes is not None else None)
    archives = list_archives(updates_root / "v1" / "archive")
    sources = [manifest]
    if stable_manifest is not None:
        sources.append(stable_manifest)
    sources.extend(load_archive_manifests(archives))
    history = merge_history(sources)
    final_bytes = render_manifest(manifest, history)

    # 2) 安装安装包文件（同哈希幂等，不同哈希拒绝原地替换）
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

    # 3) 幂等：与最终清单字节完全一致则确认 RELEASE-INFO 归档后结束
    #    （不改写 stable、不归档 stable；但若该 release 的 INFO 尚未归档
    #    ——例如上次发布在替换清单后、归档 INFO 前中断——这里补齐）
    if stable_bytes == final_bytes:
        ts = utc_timestamp()
        recovered = _archive_release_info(release_dir, version,
                                          updates_root / "v1" / "archive", ts)
        if recovered is not None:
            print("幂等恢复：补归档 RELEASE-INFO.json -> archive/%s" % recovered)
        print("stable 清单已是 %s（内容一致），发布幂等完成" % version)
        return

    # 4) 归档当前 stable 清单（原字节，含其已有 history）
    ts = utc_timestamp()
    if stable_bytes is not None:
        old_version = stable_manifest["version"]
        archive_dir = updates_root / "v1" / "archive"
        dst = unique_archive_path(archive_dir, "manifest-%s" % old_version, ts, ".json")
        atomic_write_bytes(dst, stable_bytes)
        print("已归档原 stable 清单 %s -> archive/%s" % (old_version, dst.name))

    # 5) 原子替换 stable 清单为带完整 history 的最终清单
    #    （任何前置步骤失败都不会走到这里）
    atomic_write_bytes(stable_path, final_bytes)

    # 6) RELEASE-INFO.json 透传归档：不解析、不强依赖，存在即原样归档
    #    （已存在同内容归档则不重复写，回退后重发同版本也不会翻倍）
    info_dst = _archive_release_info(release_dir, version,
                                     updates_root / "v1" / "archive", ts)
    if info_dst is not None:
        print("已透传归档 RELEASE-INFO.json -> archive/%s" % info_dst)

    print("发布完成: stable 版本 -> %s（history %d 条）" % (version, len(history)))


def _archive_release_info(release_dir, version, archive_dir, ts):
    """透传归档 RELEASE-INFO.json；同内容归档已存在则不重复写。

    返回写入的归档文件名；release 目录没有 RELEASE-INFO.json 或已有
    同内容归档时返回 None。
    """
    info_path = Path(release_dir) / "RELEASE-INFO.json"
    if not info_path.is_file():
        return None
    try:
        data = info_path.read_bytes()
    except OSError as e:
        raise PublishError("RELEASE-INFO.json 读取失败: %s" % e)
    prefix = "release-info-%s-" % version
    if archive_dir.is_dir():
        for p in sorted(archive_dir.iterdir()):
            if not (p.is_file() and p.name.startswith(prefix) and p.name.endswith(".json")):
                continue
            try:
                if p.read_bytes() == data:
                    return None
            except OSError:
                continue
    dst = unique_archive_path(archive_dir, "release-info-%s" % version, ts, ".json")
    atomic_write_bytes(dst, data)
    return dst.name


def _parse_manifest_bytes(data, source):
    """解析并完整校验一份清单字节（当前 stable / 归档 / 待落盘的最终清单）。"""
    try:
        manifest = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        raise PublishError("%s 读取或解析失败: %s" % (source, e))
    validate_manifest(manifest, source=source)
    return manifest


def list_archives(archive_dir):
    """返回归档清单列表 [(版本元组, 版本串, 时间戳, 同秒序号, 路径)]。

    按（时间戳, 版本数值, 同秒序号）升序，完全确定、不依赖目录遍历顺序。
    """
    result = []
    archive_dir = Path(archive_dir)
    if archive_dir.is_dir():
        for p in archive_dir.iterdir():
            m = ARCHIVE_RE.match(p.name)
            if p.is_file() and m:
                seq = int(m.group(3)) if m.group(3) else 0
                result.append((version_tuple(m.group(1)), m.group(1), m.group(2), seq, p))
    result.sort(key=lambda item: (item[2], item[0], item[3]))
    return result


def load_archive_manifests(archives):
    """校验并读取归档清单（入参为 list_archives 的升序结果）。

    返回按"新->旧"排列的 manifest dict 列表。任何归档损坏都立即报错终止：
    宁可发布失败，也不能静默丢掉历史公告；同名 release-info-* 与不匹配
    ARCHIVE_RE 的文件不属于清单归档，不参与校验。
    """
    manifests = []
    for _, _, _, _, p in reversed(archives):
        try:
            data = p.read_bytes()
        except OSError as e:
            raise PublishError("归档清单 %s 读取失败: %s（请修复后重试）" % (p.name, e))
        try:
            manifests.append(_parse_manifest_bytes(data, "归档清单 %s" % p.name))
        except PublishError as e:
            raise PublishError(
                "%s；损坏归档会导致发布丢失更新历史，已终止"
                "（请修复或移除该归档后重试）" % e)
    return manifests


def do_rollback(data_dir, to_version=None):
    """把 stable 清单回退到归档中的旧版本（自动持有发布锁）。

    回退后的清单保留当前已知的全部版本公告（包括高于目标版本的），
    artifacts 与顶层公告仍取归档目标版本；客户端自行忽略高于自身的公告，
    网站则可以展示历次公告。
    """
    with publisher_lock_guard(data_dir, purpose="回退"):
        return _do_rollback_locked(data_dir, to_version)


def _do_rollback_locked(data_dir, to_version=None):
    """do_rollback 的实现体；调用方必须已持有发布锁。"""
    data_dir = Path(data_dir)
    updates_root = data_dir / "public" / "updates"
    stable_path = updates_root / "v1" / "stable" / "manifest.json"

    current_version = None
    stable_manifest = None
    if stable_path.is_file():
        stable_manifest = _parse_manifest_bytes(stable_path.read_bytes(), "当前 stable 清单")
        current_version = stable_manifest["version"]

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
    target_manifest = _parse_manifest_bytes(chosen[4].read_bytes(),
                                            "归档清单 %s" % chosen[4].name)
    version = target_manifest["version"]
    for art in target_manifest["artifacts"]:
        if not (updates_root / art["path"]).is_file():
            raise PublishError("回退目标文件缺失: %s（不要手工删除 files/ 下的历史版本）" % art["path"])

    # history 保留当前已知全部公告：目标归档 > 当前 stable > 其他归档（新->旧）。
    # 目标版本自身的公告以归档目标为准，保证顶层公告与 history 条目一致。
    sources = [target_manifest]
    if stable_manifest is not None:
        sources.append(stable_manifest)
    sources.extend(load_archive_manifests(archives))
    history = merge_history(sources)
    final_bytes = render_manifest(target_manifest, history)

    atomic_write_bytes(stable_path, final_bytes)
    before = current_version if current_version is not None else "（无）"
    print("回退完成: %s -> %s（使用归档 %s，history %d 条）"
          % (before, version, chosen[4].name, len(history)))


def do_status(data_dir):
    data_dir = Path(data_dir)
    updates_root = data_dir / "public" / "updates"
    stable_path = updates_root / "v1" / "stable" / "manifest.json"
    print("数据目录: %s" % data_dir)
    if stable_path.is_file():
        try:
            manifest = json.loads(stable_path.read_bytes().decode("utf-8"))
            version, artifacts, history = validate_manifest(manifest)
        except (PublishError, UnicodeDecodeError, ValueError) as e:
            print("当前 stable 清单存在但无法解析: %s" % e)
        else:
            print("当前 stable 版本: %s" % version)
            for art in artifacts:
                print("  - %s-%s-%s  %d 字节  %s"
                      % (art["platform"], art["arch"], art["variant"], art["size"], art["path"]))
            if history:
                ordered = sorted(history, key=lambda h: version_tuple(h["version"]))
                print("历史公告 (%d 条): %s"
                      % (len(ordered), ", ".join(h["version"] for h in ordered)))
    else:
        print("当前 stable 版本: （尚未发布）")
    archives = list_archives(updates_root / "v1" / "archive")
    print("归档清单 (%d 份):" % len(archives))
    for _, version, ts, _, p in reversed(archives):
        print("  %s  %s  %s" % (version, ts, p.name))


# ---------------------------------------------------------------- watch

_watch_logger = logging.getLogger("publish.watch")


def setup_watch_logging(stream=None):
    """watch 日志走 stdout logging；空闲轮次不输出任何记录。"""
    logger = _watch_logger
    if logger.handlers:
        return logger
    handler = logging.StreamHandler(stream if stream is not None else sys.stdout)
    handler.setFormatter(
        logging.Formatter("%(asctime)s %(levelname)s %(message)s", "%Y-%m-%dT%H:%M:%SZ"))
    logger.addHandler(handler)
    logger.setLevel(logging.INFO)
    logger.propagate = False
    return logger


def scan_incoming_candidates(incoming_dir):
    """扫描 incoming 目录，返回按版本数值升序的候选 [(版本元组, 版本串, 路径)]。

    只认非隐藏、目录名严格为 kylinlist-<X.Y.Z> 的目录；READY 与 SHA256SUMS
    由调用方在处理时检查（缺了不处理也不报错，等 CI 写完再来）。
    """
    incoming_dir = Path(incoming_dir)
    if not incoming_dir.is_dir():
        return []
    candidates = []
    for entry in incoming_dir.iterdir():
        name = entry.name
        if name.startswith(".") or not entry.is_dir():
            continue
        m = re.match(r"^kylinlist-(\d+\.\d+\.\d+)$", name)
        if not m or not VERSION_RE.match(m.group(1)):
            continue
        candidates.append((version_tuple(m.group(1)), m.group(1), entry))
    candidates.sort(key=lambda item: item[0])
    return candidates


def _is_safe_sums_path(rel):
    """SHA256SUMS 条目路径必须是目录内安全的相对路径。"""
    if not rel or rel.startswith("/") or "\\" in rel or ":" in rel:
        return False
    parts = rel.split("/")
    if not all(parts):
        return False
    return not any(p in (".", "..") for p in parts)


def parse_sha256sums(text, source=SUMS_FILENAME):
    """解析标准 sha256sum 文本，返回 [(相对路径, sha256)]。

    行格式为 <64位小写hex><空格><空格|*><文件名>（文本/二进制模式标记均可）。
    拒绝格式错误、绝对路径、反斜杠、点片段与重复条目。
    """
    entries = {}
    order = []
    for lineno, line in enumerate(text.splitlines(), 1):
        where = "%s 第 %d 行" % (source, lineno)
        if not line:
            raise PublishError("%s 为空行" % where)
        m = SUMS_LINE_RE.match(line)
        if not m:
            raise PublishError("%s 不是合法的 sha256sum 行" % where)
        rel = m.group(3)
        if not _is_safe_sums_path(rel):
            raise PublishError("%s 路径不安全: %r" % (where, rel))
        if rel in entries:
            raise PublishError("%s 重复条目: %s" % (where, rel))
        entries[rel] = m.group(1)
        order.append(rel)
    return [(rel, entries[rel]) for rel in order]


def verify_sha256sums(release_dir):
    """校验候选目录的 SHA256SUMS：格式、安全路径、重复、缺文件、覆盖与哈希。

    SHA256SUMS 必须恰好覆盖目录内全部原始文件（READY、SHA256SUMS 与
    .publish-state.json 除外），且至少包含 manifest 与 RELEASE-INFO。
    """
    release_dir = Path(release_dir)
    sums_path = release_dir / SUMS_FILENAME
    try:
        text = sums_path.read_bytes().decode("utf-8")
    except (OSError, UnicodeDecodeError) as e:
        raise PublishError("%s 读取失败: %s" % (SUMS_FILENAME, e))
    entries = parse_sha256sums(text)
    if not entries:
        raise PublishError("%s 没有任何条目" % SUMS_FILENAME)
    listed = dict(entries)
    for required in SUMS_REQUIRED_ENTRIES:
        if required not in listed:
            raise PublishError("%s 缺少必需条目: %s" % (SUMS_FILENAME, required))
    actual = set()
    for p in release_dir.rglob("*"):
        if p.is_symlink():
            raise PublishError("候选目录不得包含符号链接: %s"
                               % p.relative_to(release_dir).as_posix())
        if not p.is_file():
            continue
        rel = p.relative_to(release_dir).as_posix()
        if rel in SUMS_EXCLUDED_FILES:
            continue
        actual.add(rel)
    missing = sorted(set(listed) - actual)
    if missing:
        raise PublishError("%s 条目对应的文件不存在: %s"
                           % (SUMS_FILENAME, ", ".join(missing)))
    unlisted = sorted(actual - set(listed))
    if unlisted:
        raise PublishError("目录内存在未列入 %s 的文件: %s"
                           % (SUMS_FILENAME, ", ".join(unlisted)))
    for rel, sha in entries:
        digest = sha256_file(release_dir / rel)
        if digest != sha:
            raise PublishError("sha256 不符: %s（%s 记录 %s，实际 %s）"
                               % (rel, SUMS_FILENAME, sha, digest))


def load_release_info(release_dir):
    """读取 RELEASE-INFO.json 为 dict；watch 在门禁里解析，publish 仍只透传。"""
    path = Path(release_dir) / "RELEASE-INFO.json"
    try:
        return json.loads(path.read_bytes().decode("utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as e:
        raise PublishError("RELEASE-INFO.json 读取或解析失败: %s" % e)


def validate_ci_release(info, dir_version, manifest_version, artifacts):
    """watch 的 CI 流程门禁：只认本仓库 build.yml 完整产出的 release。

    检查 RELEASE-INFO 的 partial/gitSha/ci 元数据、六个 target 精确齐全、
    manifest 与 release 版本等于目录版本。这只是流程一致性检查：
    RELEASE-INFO 随产物一起生成与传输，不构成来源签名。
    """

    def err(msg):
        raise PublishError("RELEASE-INFO 门禁: %s" % msg)

    if not isinstance(info, dict):
        err("必须是 JSON 对象")
    if info.get("partial") is not False:
        err("partial 必须为 false（不发布部分构建，实际 %r）" % (info.get("partial"),))
    ci = info.get("ci")
    if not isinstance(ci, dict):
        err("缺少 ci 来源信息（需为 JSON 对象）")
    if ci.get("repository") != CI_REPOSITORY:
        err("ci.repository 必须为 %s（实际 %r）" % (CI_REPOSITORY, ci.get("repository")))
    if ci.get("workflowPath") != CI_WORKFLOW_PATH:
        err("ci.workflowPath 必须为 %s（实际 %r）"
            % (CI_WORKFLOW_PATH, ci.get("workflowPath")))
    run_id = ci.get("runId")
    if isinstance(run_id, bool) or not (
            (isinstance(run_id, int) and run_id > 0)
            or (isinstance(run_id, str) and run_id.isdigit() and int(run_id) > 0)):
        err("ci.runId 必须是正整数（实际 %r）" % (run_id,))
    run_id_text = run_id if isinstance(run_id, str) else str(run_id)
    expected_url = CI_RUN_URL_TEMPLATE % (CI_REPOSITORY, run_id_text)
    if ci.get("runUrl") != expected_url:
        err("ci.runUrl 必须为 %s（实际 %r）" % (expected_url, ci.get("runUrl")))
    head_sha = ci.get("headSha")
    if not isinstance(head_sha, str) or not GIT_SHA_RE.match(head_sha):
        err("ci.headSha 必须是 40/64 位小写十六进制（实际 %r）" % (head_sha,))
    if info.get("gitSha") != head_sha:
        err("gitSha 必须等于 ci.headSha（gitSha %r，headSha %r）"
            % (info.get("gitSha"), head_sha))
    if info.get("version") != dir_version:
        err("version 必须等于目录版本 %s（实际 %r）" % (dir_version, info.get("version")))
    if manifest_version != dir_version:
        err("manifest 版本必须等于目录版本 %s（实际 %s）" % (dir_version, manifest_version))
    info_artifacts = info.get("artifacts")
    if not isinstance(info_artifacts, list):
        err("artifacts 必须是非空数组")
    info_targets = []
    for art in info_artifacts:
        if not isinstance(art, dict):
            err("artifacts 内必须是 JSON 对象")
        info_targets.append(art.get("target"))
    if sorted(t for t in info_targets if isinstance(t, str)) != sorted(REQUIRED_TARGETS) \
            or len(info_targets) != len(REQUIRED_TARGETS):
        err("artifacts 必须恰好覆盖六个 target: %s（实际 %s）"
            % (", ".join(REQUIRED_TARGETS), ", ".join(str(t) for t in info_targets)))
    manifest_targets = sorted(
        "%s-%s-%s" % (a["platform"], a["arch"], a["variant"]) for a in artifacts)
    if manifest_targets != sorted(REQUIRED_TARGETS):
        err("manifest artifacts 必须恰好覆盖六个 target（实际 %s）"
            % ", ".join(manifest_targets))


def read_publish_state(release_dir):
    """读取 .publish-state.json；不存在或损坏返回 None（视为未处理，可重试）。"""
    try:
        data = (Path(release_dir) / STATE_FILENAME).read_bytes()
        state = json.loads(data.decode("utf-8"))
    except (OSError, UnicodeDecodeError, ValueError):
        return None
    if isinstance(state, dict) and isinstance(state.get("state"), str):
        return state
    return None


def write_publish_state(release_dir, state, version, *, error=None, reason=None,
                        increment=False):
    """原子写入目录状态；increment 在开始一次发布尝试时把 attempts 加一。"""
    path = Path(release_dir) / STATE_FILENAME
    prev = read_publish_state(release_dir) or {}
    attempts = prev.get("attempts")
    if not isinstance(attempts, int) or attempts < 0:
        attempts = 0
    if increment:
        attempts += 1
    record = {
        "state": state,
        "version": version,
        "attempts": attempts,
        "updatedAt": utc_iso_now(),
        "error": error,
        "reason": reason,
    }
    atomic_write_bytes(
        path, json.dumps(record, ensure_ascii=False, indent=2).encode("utf-8"))
    return record


def current_stable_version(data_dir):
    """当前 stable 版本；尚未发布返回 None，清单损坏抛 PublishError。"""
    stable_path = Path(data_dir) / "public" / "updates" / "v1" / "stable" / "manifest.json"
    if not stable_path.is_file():
        return None
    manifest = _parse_manifest_bytes(stable_path.read_bytes(), "当前 stable 清单")
    return manifest["version"]


def _process_watch_candidate(release_dir, version, data_dir, log):
    """处理单个候选目录（调用方必须已持有发布锁）。返回结果或 None（无需处理）。"""
    release_dir = Path(release_dir)
    state = read_publish_state(release_dir)
    if state is not None and state.get("state") in TERMINAL_STATES:
        return None
    try:
        # READY 与 SHA256SUMS 是就绪标记：缺任一个视为 CI 尚未写完，
        # 本轮忽略、不写状态，等下一轮再看
        if not (release_dir / READY_FILENAME).is_file():
            return None
        if not (release_dir / SUMS_FILENAME).is_file():
            return None
        verify_sha256sums(release_dir)
        _, manifest_version, artifacts = load_release(release_dir)
        info = load_release_info(release_dir)
        validate_ci_release(info, version, manifest_version, artifacts)
        current = current_stable_version(data_dir)
        if current is not None and version_tuple(version) < version_tuple(current):
            write_publish_state(release_dir, STATE_SKIPPED, version,
                                reason="低于当前 stable 版本 %s" % current)
            log.info("跳过 kylinlist-%s（低于当前 stable %s）", version, current)
            return STATE_SKIPPED
        write_publish_state(release_dir, STATE_PUBLISHING, version, increment=True)
        try:
            _do_publish_locked(release_dir, data_dir)
        except Exception as e:
            message = str(e) if isinstance(e, PublishError) \
                else "%s: %s" % (type(e).__name__, e)
            write_publish_state(release_dir, STATE_FAILED, version, error=message)
            log.error("发布失败 kylinlist-%s: %s", version, message)
            return STATE_FAILED
        write_publish_state(release_dir, STATE_SUCCEEDED, version)
        log.info("发布成功 kylinlist-%s", version)
        return STATE_SUCCEEDED
    except PublishError as e:
        write_publish_state(release_dir, STATE_FAILED, version, error=str(e))
        log.error("候选 kylinlist-%s 无效: %s", version, e)
        return STATE_FAILED


def do_watch_round(incoming_dir, data_dir, log=None):
    """扫描并处理一轮 incoming 候选，返回 [(版本, 结果)]；失败目录不中断本轮。

    终态目录静默跳过；发布锁被其他进程占用时本轮留待下轮，不写任何状态。
    """
    log = log if log is not None else _watch_logger
    incoming_dir = Path(incoming_dir)
    outcomes = []
    if not incoming_dir.is_dir():
        return outcomes
    lock = PublisherLock(Path(data_dir) / LOCK_FILENAME)
    held = False
    try:
        for _, version, candidate in scan_incoming_candidates(incoming_dir):
            pre = read_publish_state(candidate)
            if pre is not None and pre.get("state") in TERMINAL_STATES:
                continue
            if not held:
                if not lock.acquire():
                    log.info("发布锁被其他进程占用（%s），本轮留待下轮处理", lock.path)
                    break
                held = True
            try:
                outcome = _process_watch_candidate(candidate, version, data_dir, log)
            except Exception as e:  # 单目录意外异常不杀死整轮
                log.error("处理 kylinlist-%s 时发生未预期异常: %s: %s",
                          version, type(e).__name__, e)
                try:
                    write_publish_state(candidate, STATE_FAILED, version,
                                        error="%s: %s" % (type(e).__name__, e))
                except OSError:
                    pass  # 状态写不进去时留待下轮重试
                outcome = STATE_FAILED
            if outcome is not None:
                outcomes.append((version, outcome))
    finally:
        if held:
            lock.release()
    return outcomes


class WatchStop(object):
    """watch 的停止标记；SIGINT/SIGTERM 触发 request()。"""

    def __init__(self):
        self.requested = False

    def request(self, signum=None, frame=None):
        self.requested = True


def install_watch_signal_handlers(stop):
    """注册 SIGINT/SIGTERM 停止 watch；非主线程等不支持的场景静默跳过。"""
    for name in ("SIGINT", "SIGTERM"):
        signum = getattr(signal, name, None)
        if signum is None:
            continue
        try:
            signal.signal(signum, stop.request)
        except (ValueError, OSError):
            pass


def do_watch(incoming_dir, data_dir, interval=DEFAULT_WATCH_INTERVAL, stop=None):
    """常驻轮询 incoming 目录；SIGINT/SIGTERM 优雅停止（退出码 0）。

    空闲轮次不输出日志；单目录失败只记录状态并继续。
    """
    log = _watch_logger
    if stop is None:
        stop = WatchStop()
    install_watch_signal_handlers(stop)
    log.info("watch 启动: incoming=%s 数据目录=%s 轮询间隔=%d 秒",
             incoming_dir, data_dir, interval)
    try:
        while not stop.requested:
            try:
                do_watch_round(incoming_dir, data_dir, log)
            except Exception as e:  # 轮次级异常不终止 watch
                log.error("本轮处理异常（继续运行）: %s: %s", type(e).__name__, e)
            if stop.requested:
                break
            deadline = time.monotonic() + interval
            while not stop.requested:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                time.sleep(min(0.25, remaining))
    finally:
        log.info("watch 已停止")


# ---------------------------------------------------------------- CLI

def positive_int(value):
    try:
        number = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError("必须是整数: %r" % (value,))
    if number <= 0:
        raise argparse.ArgumentTypeError("必须是正整数: %r" % (value,))
    return number


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

    p = sub.add_parser("watch", help="常驻轮询 incoming 目录，自动发布就绪的 CI release")
    p.add_argument("incoming_dir", metavar="incoming目录",
                   help="存放 kylinlist-<X.Y.Z> 候选目录的目录（容器内如 /data/incoming）")
    p.add_argument("--interval", metavar="秒", type=positive_int,
                   default=DEFAULT_WATCH_INTERVAL,
                   help="轮询间隔秒数（默认 %d）；SIGINT/SIGTERM 优雅停止"
                        % DEFAULT_WATCH_INTERVAL)

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
        elif args.command == "watch":
            setup_watch_logging()
            do_watch(Path(args.incoming_dir), data_dir, args.interval)
    except PublishError as e:
        print("错误: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
