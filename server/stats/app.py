#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""KylinList 轻量可信内网统计服务（仅 Python 3.12 标准库）。

职责：
- 接收客户端 POST /api/v1/stats/usage 的用量快照（按真实 IP 保存最新一份，不累加）；
- 扫描宿主 nginx 的 JSON 访问日志，统计安装包下载次数（request_id 去重，轮转安全）；
- GET /api/v1/stats/summary 输出聚合结果，绝不输出任何 IP 明细。

环境变量（均可覆盖）：
    STATS_DB        SQLite 路径，默认 /data/stats.sqlite3
    STATS_LOGS_DIR  nginx JSON 日志目录，默认 /logs
    STATS_HOST      监听地址，默认 0.0.0.0
    STATS_PORT      监听端口，默认 8000
    STATS_LOG_LEVEL 日志级别，默认 INFO

部署约定：服务只应部署在可信链路中，即入口由宿主 nginx 反代并
“覆盖写入” X-Real-IP / X-Forwarded-For，本服务才信任这两个头。
"""

from __future__ import annotations

import gzip
import ipaddress
import json
import logging
import os
import re
import sqlite3
import sys
import threading
import zlib
from contextlib import closing
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

LOG = logging.getLogger("kylinlist-stats")

SCHEMA_VERSION = 1
MAX_BODY_BYTES = 8 * 1024
BODY_DRAIN_LIMIT = 256 * 1024
COUNT_MAX = 10 ** 9
FIRST_TASK_DAYS_MAX = 100_000
READ_CHUNK_BYTES = 64 * 1024
MAX_LINE_BYTES = 2 * 1024 * 1024

DEFAULT_DB_PATH = "/data/stats.sqlite3"
DEFAULT_LOGS_DIR = "/logs"
DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 8000

USAGE_KEYS = frozenset(
    ("schemaVersion", "tasksCreated", "notesCreated", "tasksCompleted", "firstTaskDays")
)
USAGE_COUNT_KEYS = ("tasksCreated", "notesCreated", "tasksCompleted")

# 下载计入条件：method=GET、status=200，且路径严格为
# /updates/files/<X.Y.Z>/<安装形式>/<文件名>，目录与额外路径段不计。
# 只匹配原始路径（不解码 %XX），带查询串不影响路径判断。
DOWNLOAD_PATH_RE = re.compile(
    r"^/updates/files/(0|[1-9]\d{0,8})\.(0|[1-9]\d{0,8})\.(0|[1-9]\d{0,8})"
    r"/[^/]+/[^/]+$")

# 参与扫描：*.log / *.json，以及数字序号或 deploy 脚本 UTC 时间戳轮转文件。
LOG_FILE_NAME_RE = re.compile(
    r"^.+\.(?:log|json)(?:\.\d+|\.\d{8}T\d{6}Z)?(?:\.gz)?$", re.IGNORECASE)

# .gz 文件属轮转终态、不会再增长：整体导入一次后用 offset=-1 作“已完成”标记。
GZ_DONE_OFFSET = -1

SCHEMA_SQL = """
CREATE TABLE IF NOT EXISTS usage (
    ip               TEXT PRIMARY KEY,
    tasks_created    INTEGER NOT NULL,
    notes_created    INTEGER NOT NULL,
    tasks_completed  INTEGER NOT NULL,
    first_task_days  INTEGER,
    first_seen       TEXT NOT NULL,
    last_seen        TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS downloads (
    request_id TEXT PRIMARY KEY,
    version    TEXT NOT NULL,
    ip         TEXT,
    time       TEXT
);
CREATE INDEX IF NOT EXISTS downloads_version_idx ON downloads(version);
CREATE TABLE IF NOT EXISTS log_files (
    path     TEXT PRIMARY KEY,
    file_key TEXT NOT NULL,
    size     INTEGER NOT NULL,
    offset   INTEGER NOT NULL
);
"""

UPSERT_USAGE_SQL = """
INSERT INTO usage (ip, tasks_created, notes_created, tasks_completed,
                   first_task_days, first_seen, last_seen)
VALUES (?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(ip) DO UPDATE SET
    tasks_created    = excluded.tasks_created,
    notes_created    = excluded.notes_created,
    tasks_completed  = excluded.tasks_completed,
    first_task_days  = excluded.first_task_days,
    last_seen        = excluded.last_seen
"""

UPSERT_LOG_FILE_SQL = """
INSERT INTO log_files (path, file_key, size, offset)
VALUES (?, ?, ?, ?)
ON CONFLICT(path) DO UPDATE SET
    file_key = excluded.file_key,
    size     = excluded.size,
    offset   = excluded.offset
"""


# --------------------------------------------------------------------------
# 纯函数：IP 解析 / 请求体验证
# --------------------------------------------------------------------------

def utc_now_iso():
    """当前 UTC 时间的 ISO 8601 字符串（秒级）。"""
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def normalize_ip(value):
    """用 ipaddress 校验并规范化 IP；非法输入返回 None。"""
    if not isinstance(value, str):
        return None
    text = value.strip()
    if not text:
        return None
    try:
        return str(ipaddress.ip_address(text))
    except ValueError:
        return None


def pick_client_ip(headers, remote_addr):
    """真实 IP 选择：X-Real-IP > X-Forwarded-For 首段 > remote_addr。

    仅当部署链的宿主 nginx 覆盖写入这两个头时它们才可信（见模块 docstring）。
    任一头非法时向后回退；全部无效返回 None。
    """
    real_ip = headers.get("X-Real-IP")
    if real_ip:
        ip = normalize_ip(real_ip)
        if ip:
            return ip
    forwarded = headers.get("X-Forwarded-For")
    if forwarded:
        first = forwarded.split(",")[0]
        ip = normalize_ip(first)
        if ip:
            return ip
    return normalize_ip(remote_addr)


class InvalidUsagePayload(ValueError):
    """usage 请求体不符合契约。"""


def _reject_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate key")
        result[key] = value
    return result


def parse_usage_body(raw):
    """严格校验 usage 请求体，通过则原样返回 dict（键与取值已合规）。"""
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise InvalidUsagePayload("invalid usage payload") from None
    try:
        payload = json.loads(text, object_pairs_hook=_reject_duplicate_keys)
    except ValueError:
        raise InvalidUsagePayload("invalid usage payload") from None
    if not isinstance(payload, dict) or set(payload) != USAGE_KEYS:
        raise InvalidUsagePayload("invalid usage payload")
    schema_version = payload["schemaVersion"]
    if type(schema_version) is not int or schema_version != SCHEMA_VERSION:
        raise InvalidUsagePayload("invalid usage payload")
    for key in USAGE_COUNT_KEYS:
        value = payload[key]
        if type(value) is not int or not 0 <= value <= COUNT_MAX:
            raise InvalidUsagePayload("invalid usage payload")
    first_task_days = payload["firstTaskDays"]
    if first_task_days is not None:
        if (type(first_task_days) is not int
                or not 0 <= first_task_days <= FIRST_TASK_DAYS_MAX):
            raise InvalidUsagePayload("invalid usage payload")
    return payload


# --------------------------------------------------------------------------
# 存储层：SQLite（每请求独立连接 + 事务，WAL）
# --------------------------------------------------------------------------

class StatsStore:
    def __init__(self, db_path):
        self.db_path = os.path.abspath(db_path)
        parent = os.path.dirname(self.db_path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        with closing(self.connect()) as conn:
            conn.executescript(SCHEMA_SQL)
            conn.commit()

    def connect(self):
        conn = sqlite3.connect(self.db_path, timeout=30.0)
        conn.execute("PRAGMA busy_timeout = 30000")
        conn.execute("PRAGMA journal_mode = WAL")
        conn.execute("PRAGMA synchronous = NORMAL")
        return conn

    def record_usage(self, ip, payload, now=None):
        """保存该 IP 的最新用量快照（覆盖旧值，不累加），并维护 first/last seen。"""
        timestamp = now or utc_now_iso()
        with closing(self.connect()) as conn, conn:
            conn.execute(
                UPSERT_USAGE_SQL,
                (
                    ip,
                    payload["tasksCreated"],
                    payload["notesCreated"],
                    payload["tasksCompleted"],
                    payload["firstTaskDays"],
                    timestamp,
                    timestamp,
                ),
            )


def _file_key(stat):
    """文件身份：优先 inode 元组；平台无 inode 时退化为 size+mtime。"""
    if getattr(stat, "st_ino", 0):
        return "ino:%s:%s" % (stat.st_dev, stat.st_ino)
    return "mtime:%s:%s" % (stat.st_size, stat.st_mtime_ns)


class StatsApp:
    """业务对象：聚合 summary 与下载日志增量导入。"""

    def __init__(self, db_path, logs_dir):
        self.store = StatsStore(db_path)
        self.logs_dir = logs_dir
        self._import_lock = threading.Lock()

    # ---- summary -------------------------------------------------------

    def build_summary(self):
        """先增量导入下载日志，再输出不含任何 IP 明细的聚合结果。"""
        self.import_download_logs()
        with closing(self.store.connect()) as conn, conn:
            usage = conn.execute(
                """
                SELECT COALESCE(SUM(tasks_created), 0),
                       COALESCE(SUM(notes_created), 0),
                       COALESCE(SUM(tasks_completed), 0),
                       COALESCE(MAX(first_task_days), 0),
                       COUNT(*)
                FROM usage
                """
            ).fetchone()
            downloads_total = conn.execute("SELECT COUNT(*) FROM downloads").fetchone()[0]
            by_version = [
                {"version": row[0], "count": row[1]}
                for row in conn.execute(
                    "SELECT version, COUNT(*) AS c FROM downloads "
                    "GROUP BY version ORDER BY c DESC, version ASC"
                )
            ]
            users = conn.execute(
                """
                SELECT COUNT(*) FROM (
                    SELECT ip FROM usage
                    UNION
                    SELECT ip FROM downloads WHERE ip IS NOT NULL
                )
                """
            ).fetchone()[0]
        return {
            "schemaVersion": SCHEMA_VERSION,
            "generatedAt": utc_now_iso(),
            "users": users,
            "usage": {
                "tasksCreated": usage[0],
                "notesCreated": usage[1],
                "tasksCompleted": usage[2],
                "longestUsageDays": usage[3],
            },
            "downloads": {"total": downloads_total, "byVersion": by_version},
        }

    # ---- nginx JSON 日志增量导入 ----------------------------------------

    def import_download_logs(self):
        """扫描日志目录；同文件按 offset 断点续读，跨文件按 request_id 去重。

        与 log_files checkpoint 在同一事务内提交，崩溃后至多重读、不会重计。
        """
        logs_dir = self.logs_dir
        if not logs_dir or not os.path.isdir(logs_dir):
            return
        with self._import_lock:
            with closing(self.store.connect()) as conn:
                imported = 0
                for name in sorted(os.listdir(logs_dir)):
                    path = os.path.join(logs_dir, name)
                    if not os.path.isfile(path) or not LOG_FILE_NAME_RE.match(name):
                        continue
                    try:
                        if name.lower().endswith(".gz"):
                            imported += self._scan_gzip_file(conn, path)
                        else:
                            imported += self._scan_plain_file(conn, path)
                        conn.commit()
                    except (OSError, EOFError, zlib.error) as exc:
                        # 坏文件/半截压缩流：回滚该文件本批写入，保留旧 checkpoint 下次重试。
                        conn.rollback()
                        LOG.warning("skipping unreadable log file %s: %s", path, exc)
                if imported:
                    LOG.info("imported %d new download record(s)", imported)

    def _scan_plain_file(self, conn, path):
        """断点续读明文日志；只处理到最近一个完整换行，半截行留待下次。"""
        stat = os.stat(path)
        file_key = _file_key(stat)
        row = conn.execute(
            "SELECT file_key, offset FROM log_files WHERE path = ?", (path,)
        ).fetchone()
        if row is None or row[0] != file_key or stat.st_size < row[1]:
            offset = 0  # 新文件 / 已轮转换身份 / 被截短：从头重扫，靠 request_id 去重
        else:
            offset = row[1]
        inserted = 0
        new_offset = offset
        with open(path, "rb") as handle:
            handle.seek(offset)
            carry = b""
            while True:
                chunk = handle.read(READ_CHUNK_BYTES)
                if not chunk:
                    break
                carry += chunk
                cut = carry.rfind(b"\n")
                if cut < 0:
                    if len(carry) > MAX_LINE_BYTES:
                        # 超长行视为损坏：不推进 checkpoint，本批已入库的行仍提交（有去重兜底）。
                        LOG.warning("log file %s contains an overlong line; deferring", path)
                        return inserted
                    continue
                block = carry[: cut + 1]
                carry = carry[cut + 1:]
                for raw_line in block.split(b"\n"):
                    if self._ingest_download_line(conn, raw_line):
                        inserted += 1
                new_offset += len(block)
        conn.execute(UPSERT_LOG_FILE_SQL, (path, file_key, stat.st_size, new_offset))
        return inserted

    def _scan_gzip_file(self, conn, path):
        """.gz 为轮转终态：整体导入一次后记录“已完成”标记（内容变化则重扫）。"""
        stat = os.stat(path)
        file_key = _file_key(stat)
        row = conn.execute(
            "SELECT file_key, offset FROM log_files WHERE path = ?", (path,)
        ).fetchone()
        if row is not None and row[0] == file_key and row[1] == GZ_DONE_OFFSET:
            return 0
        inserted = 0
        with gzip.open(path, "rb") as handle:
            for raw_line in handle:
                if self._ingest_download_line(conn, raw_line):
                    inserted += 1
        conn.execute(UPSERT_LOG_FILE_SQL, (path, file_key, stat.st_size, GZ_DONE_OFFSET))
        return inserted

    def _ingest_download_line(self, conn, raw_line):
        """单行 JSON 判定与入库；返回是否新插入了下载记录。"""
        line = raw_line.strip()
        if not line:
            return False
        try:
            record = json.loads(line.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return False  # 坏行 / 非 JSON：跳过
        if not isinstance(record, dict):
            return False
        if record.get("method") != "GET" or record.get("status") != 200:
            return False
        uri = record.get("uri")
        if not isinstance(uri, str):
            return False
        match = DOWNLOAD_PATH_RE.match(urlsplit(uri).path)
        if match is None:
            return False
        request_id = record.get("request_id")
        if not isinstance(request_id, str) or not request_id:
            return False  # 无 request_id 无法去重，宁可不计
        version = ".".join(match.group(1, 2, 3))
        ip = normalize_ip(record.get("ip"))  # 无效 IP 不参与 users 统计
        time_value = record.get("time")
        if not isinstance(time_value, str):
            time_value = None
        cursor = conn.execute(
            "INSERT OR IGNORE INTO downloads (request_id, version, ip, time) "
            "VALUES (?, ?, ?, ?)",
            (request_id, version, ip, time_value),
        )
        return cursor.rowcount > 0


# --------------------------------------------------------------------------
# HTTP 层
# --------------------------------------------------------------------------

ROUTES = {
    "/api/v1/stats/usage": ("POST", "_handle_usage"),
    "/api/v1/stats/summary": ("GET", "_handle_summary"),
    "/api/v1/stats/healthz": ("GET", "_handle_healthz"),
}


class StatsRequestHandler(BaseHTTPRequestHandler):
    app = None  # 由 serve() / 测试注入 StatsApp 实例
    server_version = "KylinListStats/1"
    protocol_version = "HTTP/1.1"
    timeout = 30

    def do_POST(self):
        self._dispatch("POST")

    def do_GET(self):
        self._dispatch("GET")

    def do_HEAD(self):
        self._dispatch("HEAD")

    def do_PUT(self):
        self._dispatch("PUT")

    def do_DELETE(self):
        self._dispatch("DELETE")

    def do_PATCH(self):
        self._dispatch("PATCH")

    def do_OPTIONS(self):
        self._dispatch("OPTIONS")

    def _dispatch(self, method):
        path = urlsplit(self.path).path
        try:
            route = ROUTES.get(path)
            if route is None:
                self._discard_request_body()
                self._respond_error(404, "not found")
                return
            allowed, handler_name = route
            if method != allowed:
                self._discard_request_body()
                self._respond_error(405, "method not allowed",
                                    extra_headers={"Allow": allowed})
                return
            getattr(self, handler_name)()
        except Exception:
            # 含 sqlite 写失败：统一 500；日志只含异常栈，不含请求体。
            LOG.exception("error while serving %s %s", method, path)
            self.close_connection = True
            if not getattr(self, "_response_sent", False):
                self._respond_error_quietly(500, "internal error")

    # ---- endpoints ------------------------------------------------------

    def _handle_usage(self):
        client_ip = pick_client_ip(self.headers, self.client_address[0])
        if client_ip is None:
            self._respond_error(400, "client ip missing or invalid")
            return
        content_type = (self.headers.get("Content-Type") or "").split(";", 1)[0].strip().lower()
        if content_type != "application/json":
            self._respond_error(400, "content-type must be application/json")
            return
        if "chunked" in (self.headers.get("Transfer-Encoding") or "").lower():
            self._respond_error(400, "chunked request bodies are not supported", close=True)
            return
        length_header = self.headers.get("Content-Length")
        if length_header is None or not length_header.strip().isdigit():
            self._respond_error(400, "content-length required", close=True)
            return
        length = int(length_header)
        if length > MAX_BODY_BYTES:
            self._discard_request_body()
            self._respond_error(413, "request body too large")
            return
        body = self.rfile.read(length) if length else b""
        if len(body) != length:
            self._respond_error(400, "request body shorter than content-length", close=True)
            return
        try:
            payload = parse_usage_body(body)
        except InvalidUsagePayload:
            self._respond_error(400, "invalid usage payload")
            return
        self.app.store.record_usage(client_ip, payload)
        LOG.info("usage snapshot stored (client=%s)", client_ip)
        self._respond_json(200, {"ok": True})

    def _handle_summary(self):
        self._respond_json(200, self.app.build_summary())

    def _handle_healthz(self):
        self._respond_json(200, {"ok": True})

    # ---- 响应与辅助 ------------------------------------------------------

    def _discard_request_body(self):
        """尽量读完并丢弃请求体（小包），保证连接状态干净；过大则直接关闭。"""
        length_header = self.headers.get("Content-Length")
        if not length_header or not length_header.strip().isdigit():
            return
        remaining = int(length_header)
        if remaining > BODY_DRAIN_LIMIT:
            self.close_connection = True
            return
        while remaining > 0:
            chunk = self.rfile.read(min(remaining, READ_CHUNK_BYTES))
            if not chunk:
                self.close_connection = True
                return
            remaining -= len(chunk)

    def _respond_json(self, status, payload, extra_headers=None):
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for key, value in (extra_headers or {}).items():
            self.send_header(key, value)
        if self.close_connection:
            self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)
        self._response_sent = True

    def _respond_error(self, status, message, close=False, extra_headers=None):
        if close:
            self.close_connection = True
        headers = dict(extra_headers or {})
        self._respond_json(status, {"ok": False, "error": message}, extra_headers=headers)

    def _respond_error_quietly(self, status, message):
        try:
            self._respond_error(status, message)
        except Exception:
            LOG.debug("failed to send error response", exc_info=True)

    def version_string(self):
        return self.server_version

    def log_message(self, fmt, *args):
        # 请求行级日志（方法/路径/状态），绝不打印请求体内容。
        LOG.info("%s %s", self.client_address[0], fmt % args)


def serve(db_path, logs_dir, host, port):
    app = StatsApp(db_path, logs_dir)
    handler = type("BoundStatsRequestHandler", (StatsRequestHandler,), {"app": app})
    server = ThreadingHTTPServer((host, port), handler)
    LOG.info("kylinlist stats listening on %s:%d (db=%s, logs=%s)",
             host, port, db_path, logs_dir)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


def main(argv=None):
    logging.basicConfig(
        stream=sys.stdout,
        level=os.environ.get("STATS_LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    db_path = os.environ.get("STATS_DB", DEFAULT_DB_PATH)
    logs_dir = os.environ.get("STATS_LOGS_DIR", DEFAULT_LOGS_DIR)
    host = os.environ.get("STATS_HOST", DEFAULT_HOST)
    try:
        port = int(os.environ.get("STATS_PORT", str(DEFAULT_PORT)))
    except ValueError:
        LOG.error("invalid STATS_PORT value")
        return 2
    serve(db_path, logs_dir, host, port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
