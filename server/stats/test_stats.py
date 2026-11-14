#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""KylinList 统计服务全量 unittest。

只使用临时目录与合成数据；HTTP 测试绑定 127.0.0.1 随机端口。
运行：python -I test_stats.py  或  python -I -m unittest -v test_stats
"""

import gzip
import http.client
import ipaddress
import json
import os
import sqlite3
import sys
import tempfile
import threading
import unittest
from datetime import datetime
from http.server import ThreadingHTTPServer
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import app as stats_app  # noqa: E402
from app import (  # noqa: E402
    DOWNLOAD_PATH_RE,
    InvalidUsagePayload,
    LOG_FILE_NAME_RE,
    StatsApp,
    normalize_ip,
    parse_usage_body,
    pick_client_ip,
)


# --------------------------------------------------------------------------
# 构造数据
# --------------------------------------------------------------------------

def usage_payload(**overrides):
    payload = {
        "schemaVersion": 1,
        "tasksCreated": 1,
        "notesCreated": 2,
        "tasksCompleted": 3,
        "firstTaskDays": None,
    }
    payload.update(overrides)
    return payload


def dl(request_id, version="1.0.0", ip="10.20.30.40", method="GET", status=200,
       uri=None, **extra):
    """合成一行 nginx JSON 下载日志。"""
    record = {
        "request_id": request_id,
        "time": "2026-11-14T00:00:00+00:00",
        "ip": ip,
        "method": method,
        "uri": uri if uri is not None
        else "/updates/files/%s/windows-amd64-setup/KylinList_%s_x64.exe" % (version, version),
        "status": status,
        "bytes_sent": 4096,
    }
    record.update(extra)
    return json.dumps(record)


def write_text(path, text):
    with open(path, "wb") as handle:
        handle.write(text.encode("utf-8"))


def append_text(path, text):
    with open(path, "ab") as handle:
        handle.write(text.encode("utf-8"))


def rows(db_path, sql, params=()):
    conn = sqlite3.connect(db_path)
    try:
        return conn.execute(sql, params).fetchall()
    finally:
        conn.close()


# --------------------------------------------------------------------------
# 纯函数：IP
# --------------------------------------------------------------------------

class TestNormalizeIp(unittest.TestCase):
    def test_valid_ipv4(self):
        self.assertEqual(normalize_ip("192.168.1.5"), "192.168.1.5")
        self.assertEqual(normalize_ip(" 10.0.0.1\t"), "10.0.0.1")

    def test_valid_ipv6_normalized(self):
        raw = "2001:DB8::0001"
        self.assertEqual(normalize_ip(raw), str(ipaddress.ip_address("2001:db8::1")))
        # IPv4-mapped 地址规范化为统一形式，同一客户端不会重复计数
        self.assertEqual(normalize_ip("::ffff:192.168.0.1"),
                         normalize_ip("::ffff:192.168.0.1"))

    def test_invalid(self):
        for bad in ("", "   ", "abc", "999.1.1.1", "10.0.0", "1.2.3.4.5",
                    "10.01.0.1", "1.2.3.4:80", "example.com", None, 7, b"1.2.3.4"):
            self.assertIsNone(normalize_ip(bad), repr(bad))


class TestPickClientIp(unittest.TestCase):
    def test_prefers_real_ip(self):
        headers = {"X-Real-IP": "10.0.0.1", "X-Forwarded-For": "10.0.0.2, 10.0.0.3"}
        self.assertEqual(pick_client_ip(headers, "192.168.0.9"), "10.0.0.1")

    def test_falls_back_to_xff_first_segment(self):
        headers = {"X-Forwarded-For": "10.0.0.2, 10.0.0.3"}
        self.assertEqual(pick_client_ip(headers, "192.168.0.9"), "10.0.0.2")

    def test_invalid_real_ip_falls_to_xff(self):
        headers = {"X-Real-IP": "not-an-ip", "X-Forwarded-For": "10.0.0.4"}
        self.assertEqual(pick_client_ip(headers, "192.168.0.9"), "10.0.0.4")

    def test_invalid_headers_fall_to_remote(self):
        headers = {"X-Real-IP": "junk", "X-Forwarded-For": "also junk"}
        self.assertEqual(pick_client_ip(headers, "192.168.0.9"), "192.168.0.9")

    def test_xff_only_first_segment_is_considered(self):
        headers = {"X-Forwarded-For": "bad-entry, 10.0.0.7"}
        self.assertEqual(pick_client_ip(headers, "10.9.9.9"), "10.9.9.9")

    def test_no_headers_uses_remote(self):
        self.assertEqual(pick_client_ip({}, "127.0.0.1"), "127.0.0.1")

    def test_nothing_valid_returns_none(self):
        self.assertIsNone(pick_client_ip({}, "garbage"))
        self.assertIsNone(
            pick_client_ip({"X-Real-IP": "junk", "X-Forwarded-For": "junk"}, "nope"))


# --------------------------------------------------------------------------
# 纯函数：usage 请求体
# --------------------------------------------------------------------------

class TestParseUsageBody(unittest.TestCase):
    def test_valid(self):
        payload = usage_payload(tasksCreated=0, notesCreated=10 ** 9,
                                tasksCompleted=5, firstTaskDays=100000)
        self.assertEqual(parse_usage_body(json.dumps(payload).encode()), payload)
        self.assertEqual(parse_usage_body(json.dumps(usage_payload()).encode()),
                         usage_payload())

    def test_missing_key(self):
        body = usage_payload()
        del body["firstTaskDays"]
        with self.assertRaises(InvalidUsagePayload):
            parse_usage_body(json.dumps(body).encode())

    def test_extra_key(self):
        body = usage_payload(extra="x")
        with self.assertRaises(InvalidUsagePayload):
            parse_usage_body(json.dumps(body).encode())

    def test_bad_schema_version(self):
        for bad in (0, 2, -1, "1", True, 1.0, None):
            with self.assertRaises(InvalidUsagePayload, msg=repr(bad)):
                parse_usage_body(json.dumps(usage_payload(schemaVersion=bad)).encode())

    def test_bad_counts(self):
        for key in ("tasksCreated", "notesCreated", "tasksCompleted"):
            for bad in (-1, 10 ** 9 + 1, "5", 1.5, True, False, None):
                with self.assertRaises(InvalidUsagePayload, msg="%s=%r" % (key, bad)):
                    parse_usage_body(json.dumps(usage_payload(**{key: bad})).encode())

    def test_bad_first_task_days(self):
        for bad in (-1, 100001, "7", 1.5, True, []):
            with self.assertRaises(InvalidUsagePayload, msg=repr(bad)):
                parse_usage_body(json.dumps(usage_payload(firstTaskDays=bad)).encode())

    def test_not_an_object(self):
        for raw in (b"[]", b'"x"', b"5", b"null", b"true", b""):
            with self.assertRaises(InvalidUsagePayload, msg=repr(raw)):
                parse_usage_body(raw)

    def test_invalid_utf8(self):
        with self.assertRaises(InvalidUsagePayload):
            parse_usage_body(b"\xff\xfe{")

    def test_trailing_garbage(self):
        with self.assertRaises(InvalidUsagePayload):
            parse_usage_body(json.dumps(usage_payload()).encode() + b" x")

    def test_duplicate_keys_rejected(self):
        raw = ('{"schemaVersion":1,"schemaVersion":1,"tasksCreated":0,'
               '"notesCreated":0,"tasksCompleted":0,"firstTaskDays":null}')
        with self.assertRaises(InvalidUsagePayload):
            parse_usage_body(raw.encode())


# --------------------------------------------------------------------------
# 纯函数：日志文件名 / 下载路径匹配
# --------------------------------------------------------------------------

class TestPatterns(unittest.TestCase):
    def test_log_file_names(self):
        for name in ("access.log", "stats.json", "access.log.1", "access.log.12",
                     "stats.access.log.3.gz", "stats.JSON", "access.log.20261110.gz",
                     "downloads.json.log.20261114T040000Z",
                     "downloads.json.log.20261114T040000Z.gz"):
            self.assertIsNotNone(LOG_FILE_NAME_RE.match(name), name)

    def test_ignored_file_names(self):
        for name in ("readme.txt", "access.log.bak", "log", ".log",
                     "access.log.gz.tmp", "archive.tar"):
            self.assertIsNone(LOG_FILE_NAME_RE.match(name), name)

    def test_download_paths(self):
        cases = {
            "/updates/files/1.2.3/windows-amd64-setup/x.exe": "1.2.3",
            "/updates/files/10.20.30/linux-amd64-system-qt/pkg.zip": "10.20.30",
            "/updates/files/0.0.0/linux-arm64-system-qt/a.deb": "0.0.0",
            "/updates/files/2.0.0/linux-arm64-bundled-qt/c.deb": "2.0.0",
        }
        for path, version in cases.items():
            match = DOWNLOAD_PATH_RE.match(path)
            self.assertIsNotNone(match, path)
            self.assertEqual(".".join(match.group(1, 2, 3)), version)

    def test_non_download_paths(self):
        for path in ("/updates/files/1.2/x", "/updates/files/v1.2.3/x",
                     "/updates/files/1.2.3x/a", "/updates/files/1.2.3.4/x",
                     "/updates/files/1.2.3", "/updates/files/1.2.3/",
                     "/updates/files/1.2.3/x.exe",  # 缺安装形式目录
                     "/updates/files/1.2.3/a/b/c.exe",  # 额外路径段
                     "/updates/files/01.2.3/a/x.exe",  # 版本前导零
                     "/updates/files/1.2.3/a/",  # 目录不是安装包
                     "//updates/files/1.2.3/a", "/api/updates/files/1.2.3/a",
                     "/updates%2Ffiles%2F1.2.3%2Fa", "/downloads/files/1.2.3/a",
                     "/updates/file/1.2.3/a"):
            self.assertIsNone(DOWNLOAD_PATH_RE.match(path), path)


# --------------------------------------------------------------------------
# 存储 / summary（无日志目录）
# --------------------------------------------------------------------------

class StoreTestFixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = self._tmp.name
        # logs 目录刻意不存在：验证导入被安全跳过
        self.app = StatsApp(os.path.join(self.tmp, "data", "stats.sqlite3"),
                            os.path.join(self.tmp, "logs"))
        self.db_path = self.app.store.db_path

    def tearDown(self):
        self._tmp.cleanup()

    def summary(self):
        return self.app.build_summary()


class TestStoreSummary(StoreTestFixture):
    def test_empty_summary_shape(self):
        summary = self.summary()
        self.assertEqual(set(summary),
                         {"schemaVersion", "generatedAt", "users", "usage", "downloads"})
        self.assertEqual(summary["schemaVersion"], 1)
        self.assertEqual(summary["users"], 0)
        self.assertEqual(summary["usage"], {
            "tasksCreated": 0, "notesCreated": 0,
            "tasksCompleted": 0, "longestUsageDays": 0,
        })
        self.assertEqual(summary["downloads"], {"total": 0, "byVersion": []})

    def test_generated_at_is_utc_iso(self):
        stamp = self.summary()["generatedAt"]
        parsed = datetime.fromisoformat(stamp)
        self.assertEqual(parsed.utcoffset().total_seconds(), 0)

    def test_same_ip_repeated_reports_not_amplified(self):
        store = self.app.store
        store.record_usage("10.1.1.1", usage_payload(tasksCreated=50, notesCreated=60,
                                                     tasksCompleted=70, firstTaskDays=10))
        store.record_usage("10.1.1.1", usage_payload(tasksCreated=7, notesCreated=8,
                                                     tasksCompleted=9, firstTaskDays=None))
        summary = self.summary()
        self.assertEqual(summary["users"], 1)
        self.assertEqual(summary["usage"], {
            "tasksCreated": 7, "notesCreated": 8,
            "tasksCompleted": 9, "longestUsageDays": 0,
        })

    def test_different_ips_are_summed(self):
        store = self.app.store
        store.record_usage("10.1.1.1", usage_payload(tasksCreated=1, notesCreated=2,
                                                     tasksCompleted=3, firstTaskDays=None))
        store.record_usage("10.2.2.2", usage_payload(tasksCreated=10, notesCreated=20,
                                                     tasksCompleted=30, firstTaskDays=42))
        store.record_usage("10.3.3.3", usage_payload(tasksCreated=100, notesCreated=0,
                                                     tasksCompleted=0, firstTaskDays=5))
        summary = self.summary()
        self.assertEqual(summary["users"], 3)
        self.assertEqual(summary["usage"], {
            "tasksCreated": 111, "notesCreated": 22,
            "tasksCompleted": 33, "longestUsageDays": 42,
        })

    def test_first_seen_kept_last_seen_updated(self):
        store = self.app.store
        store.record_usage("10.1.1.1", usage_payload(), now="2026-01-01T00:00:00+00:00")
        store.record_usage("10.1.1.1", usage_payload(), now="2026-02-02T00:00:00+00:00")
        first_seen, last_seen = rows(
            self.db_path, "SELECT first_seen, last_seen FROM usage WHERE ip = ?",
            ("10.1.1.1",))[0]
        self.assertEqual(first_seen, "2026-01-01T00:00:00+00:00")
        self.assertEqual(last_seen, "2026-02-02T00:00:00+00:00")

    def test_ipv6_text_forms_normalize_identically(self):
        # 不同书写形式归一到同一规范化串（v4-mapped 的具体渲染随 Python 版本而异，
        # 关键是同一运行时内形式不同的同一地址得到同一结果），同一客户端不会重复计数
        self.assertEqual(normalize_ip("::FFFF:192.168.1.1"),
                         normalize_ip("::ffff:192.168.1.1"))
        self.assertEqual(normalize_ip("2001:DB8::1"),
                         normalize_ip("2001:db8:0:0:0:0:0:1"))
        self.assertEqual(normalize_ip(" 2001:db8::1 "), "2001:db8::1")

    def test_db_parent_dirs_created(self):
        self.assertTrue(os.path.isfile(self.db_path))


# --------------------------------------------------------------------------
# 下载日志导入
# --------------------------------------------------------------------------

class ImportTestFixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = self._tmp.name
        self.logs_dir = os.path.join(self.tmp, "logs")
        os.makedirs(self.logs_dir)
        self.app = StatsApp(os.path.join(self.tmp, "data", "stats.sqlite3"),
                            self.logs_dir)
        self.db_path = self.app.store.db_path

    def tearDown(self):
        self._tmp.cleanup()

    def summary(self):
        return self.app.build_summary()

    @property
    def access_log(self):
        return os.path.join(self.logs_dir, "stats.access.log")


class TestDownloadImport(ImportTestFixture):
    def test_line_filtering_and_counting(self):
        lines = [
            dl("r1", version="1.2.3"),                                # 计入
            dl("r1", version="1.2.3"),                                # 同 request_id 去重
            dl("r2", method="POST"),                                  # 非 GET
            dl("r3", status=404),                                     # 非 200
            dl("r4", status=304),                                     # 非 200
            dl("r5", uri="/updates/files/1.0/x.exe"),                 # 版本非 X.Y.Z
            dl("r6", uri="/updates/files/1.2.3.4/x.exe"),             # 版本段不符
            dl("r7", uri="/downloads/files/1.2.3/x.exe"),             # 前缀不符
            dl("r8", uri="/updates/files/2.0.0/windows-amd64-setup/pkg.exe?token=abc&x=1"),  # 查询串计入
            dl("r9", uri="/updates/files/2.0.0/"),                    # 目录请求不计入
            dl(""),                                                   # request_id 为空
            json.dumps({k: v for k, v in json.loads(dl("r10")).items()
                        if k != "request_id"}),                       # 缺 request_id
            "{broken json",                                           # 坏行
            "[1, 2, 3]",                                              # 非 JSON 对象
            "null",                                                   # 非 JSON 对象
            dl("r11", status="200"),                                  # status 非整数
            dl("r12", method="get"),                                  # 方法大小写敏感
        ]
        write_text(self.access_log, "\n".join(lines) + "\n")
        summary = self.summary()
        self.assertEqual(summary["downloads"]["total"], 2)
        self.assertEqual(summary["downloads"]["byVersion"],
                         [{"version": "1.2.3", "count": 1},
                          {"version": "2.0.0", "count": 1}])
        # 无 usage 上报，users 只含有效下载 IP
        self.assertEqual(summary["users"], 1)

    def test_repeated_summary_does_not_recount(self):
        write_text(self.access_log, dl("r1") + "\n" + dl("r2") + "\n")
        first = self.summary()
        second = self.summary()
        third = self.summary()
        self.assertEqual(first["downloads"]["total"], 2)
        self.assertEqual(second["downloads"]["total"], 2)
        self.assertEqual(third["downloads"]["total"], 2)

    def test_incremental_append(self):
        write_text(self.access_log, dl("r1") + "\n" + dl("r2") + "\n")
        self.assertEqual(self.summary()["downloads"]["total"], 2)
        append_text(self.access_log, dl("r3") + "\n" + dl("r4") + "\n")
        self.assertEqual(self.summary()["downloads"]["total"], 4)
        self.assertEqual(self.summary()["downloads"]["total"], 4)

    def test_half_line_is_deferred_then_completed(self):
        write_text(self.access_log, dl("r1") + "\n" + dl("r2"))  # r2 无换行：半截行
        self.assertEqual(self.summary()["downloads"]["total"], 1)
        append_text(self.access_log, "\n" + dl("r3") + "\n")
        self.assertEqual(self.summary()["downloads"]["total"], 3)

    def test_duplicate_request_id_across_files(self):
        write_text(self.access_log, dl("r1") + "\n")
        write_text(os.path.join(self.logs_dir, "other.log"), dl("r1") + "\n" + dl("r2") + "\n")
        write_text(os.path.join(self.logs_dir, "stats.json"), dl("r1") + "\n")
        self.assertEqual(self.summary()["downloads"]["total"], 2)

    def test_rotation_by_rename(self):
        write_text(self.access_log, dl("r1") + "\n" + dl("r2") + "\n")
        self.assertEqual(self.summary()["downloads"]["total"], 2)
        os.replace(self.access_log, self.access_log + ".1")
        write_text(self.access_log, dl("r2") + "\n" + dl("r3") + "\n")
        self.assertEqual(self.summary()["downloads"]["total"], 3)
        self.assertEqual(self.summary()["downloads"]["total"], 3)

    def test_truncated_file_is_rescanned_without_double_count(self):
        long_pad = "x" * 3000
        write_text(self.access_log,
                   dl("r1", padding=long_pad) + "\n" + dl("r2", padding=long_pad) + "\n")
        self.assertEqual(self.summary()["downloads"]["total"], 2)
        # 模拟 copytruncate：文件被截短后写入新内容（且总长小于旧 offset）
        write_text(self.access_log, dl("r2") + "\n" + dl("r3") + "\n")
        self.assertLess(os.path.getsize(self.access_log), 3000)
        self.assertEqual(self.summary()["downloads"]["total"], 3)
        self.assertEqual(self.summary()["downloads"]["total"], 3)

    def test_gzip_rotated_file(self):
        gz_path = os.path.join(self.logs_dir, "stats.access.log.3.gz")
        with gzip.open(gz_path, "wb") as handle:
            handle.write((dl("g1") + "\n" + dl("g2") + "\n" + dl("g1") + "\n").encode())
        self.assertEqual(self.summary()["downloads"]["total"], 2)
        self.assertEqual(self.summary()["downloads"]["total"], 2)  # gz 只导入一次

    def test_truncated_gzip_is_skipped_and_does_not_crash(self):
        gz_path = os.path.join(self.logs_dir, "broken.log.gz")
        payload = (dl("g1") + "\n" + dl("g2") + "\n").encode()
        # 只保留 gzip 头 + 极少量压缩数据，任何一行都不可能完整解出
        with open(gz_path, "wb") as handle:
            handle.write(gzip.compress(payload)[:25])
        self.assertEqual(self.summary()["downloads"]["total"], 0)
        self.assertEqual(self.summary()["downloads"]["total"], 0)

    def test_history_survives_log_removal(self):
        write_text(self.access_log, dl("r1") + "\n" + dl("r2") + "\n")
        before = self.summary()
        os.remove(self.access_log)
        after = self.summary()
        self.assertEqual(before["downloads"], after["downloads"])
        self.assertEqual(after["downloads"]["total"], 2)

    def test_ignored_file_names(self):
        write_text(os.path.join(self.logs_dir, "readme.txt"), dl("r1") + "\n")
        write_text(os.path.join(self.logs_dir, "access.log.bak"), dl("r2") + "\n")
        write_text(os.path.join(self.logs_dir, "access.log.7"), dl("r3") + "\n")
        self.assertEqual(self.summary()["downloads"]["total"], 1)

    def test_invalid_download_ip_counts_total_but_not_users(self):
        write_text(self.access_log, dl("r1", ip="not-an-ip") + "\n")
        summary = self.summary()
        self.assertEqual(summary["downloads"]["total"], 1)
        self.assertEqual(summary["users"], 0)

    def test_users_union_of_usage_and_download_ips(self):
        self.app.store.record_usage("10.5.5.5", usage_payload())
        write_text(self.access_log,
                   dl("r1", ip="10.6.6.6") + "\n"
                   + dl("r2", ip="10.5.5.5") + "\n"     # 与 usage 重叠
                   + dl("r3", ip="bad") + "\n")         # 无效 IP 不计入
        summary = self.summary()
        self.assertEqual(summary["users"], 2)  # 10.5.5.5 + 10.6.6.6

    def test_by_version_ordering(self):
        lines = ([dl("v%d" % i, version="10.0.0") for i in range(2)]
                 + [dl("w%d" % i, version="1.0.0") for i in range(1)]
                 + [dl("x%d" % i, version="2.0.0") for i in range(1)])
        write_text(self.access_log, "\n".join(lines) + "\n")
        self.assertEqual(self.summary()["downloads"]["byVersion"],
                         [{"version": "10.0.0", "count": 2},
                          {"version": "1.0.0", "count": 1},
                          {"version": "2.0.0", "count": 1}])

    def test_overlong_line_does_not_wreck_import(self):
        huge = dl("big", padding="y" * (2 * 1024 * 1024))
        write_text(self.access_log, dl("r1") + "\n" + huge[: len(huge) - 1])  # 无换行
        summary = self.summary()
        self.assertEqual(summary["downloads"]["total"], 1)


# --------------------------------------------------------------------------
# HTTP 端到端
# --------------------------------------------------------------------------

class HttpTestFixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = self._tmp.name
        self.logs_dir = os.path.join(self.tmp, "logs")
        os.makedirs(self.logs_dir)
        self.app = StatsApp(os.path.join(self.tmp, "data", "stats.sqlite3"),
                            self.logs_dir)
        handler = type("TestStatsHandler", (stats_app.StatsRequestHandler,),
                       {"app": self.app})
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        self._tmp.cleanup()

    # -- helpers ----------------------------------------------------------

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            conn.request(method, path, body=body, headers=headers or {})
            response = conn.getresponse()
            data = response.read()
            return response.status, response.getheaders(), data
        finally:
            conn.close()

    def post_usage(self, payload=None, raw_body=None, headers=None):
        request_headers = {"Content-Type": "application/json"}
        request_headers.update(headers or {})
        body = raw_body if raw_body is not None else json.dumps(
            payload if payload is not None else usage_payload())
        return self.request("POST", "/api/v1/stats/usage",
                            body=body, headers=request_headers)

    def get_summary(self):
        status, _, data = self.request("GET", "/api/v1/stats/summary")
        self.assertEqual(status, 200)
        return json.loads(data)


class TestHttpHealthz(HttpTestFixture):
    def test_healthz(self):
        status, headers, data = self.request("GET", "/api/v1/stats/healthz")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(data), {"ok": True})
        self.assertIn("application/json",
                      dict(headers).get("Content-Type", ""))


class TestHttpUsage(HttpTestFixture):
    def test_valid_report(self):
        status, _, data = self.post_usage(
            usage_payload(tasksCreated=4, notesCreated=5, tasksCompleted=6,
                          firstTaskDays=7))
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(data), {"ok": True})
        summary = self.get_summary()
        self.assertEqual(summary["users"], 1)  # remote_addr = 127.0.0.1
        self.assertEqual(summary["usage"]["tasksCreated"], 4)

    def test_content_type_required(self):
        status, _, _ = self.request(
            "POST", "/api/v1/stats/usage",
            body=json.dumps(usage_payload()),
            headers={"Content-Type": "text/plain"})
        self.assertEqual(status, 400)

    def test_charset_parameter_accepted(self):
        status, _, _ = self.post_usage(
            headers={"Content-Type": "application/json; charset=utf-8"})
        self.assertEqual(status, 200)

    DATA_DRIVEN_BAD_PAYLOADS = [
        usage_payload(schemaVersion=2),
        usage_payload(schemaVersion="1"),
        usage_payload(schemaVersion=None),
        usage_payload(tasksCreated=-1),
        usage_payload(tasksCreated=10 ** 9 + 1),
        usage_payload(tasksCreated=True),
        usage_payload(tasksCreated=1.5),
        usage_payload(notesCreated="9"),
        usage_payload(tasksCompleted=None),
        usage_payload(firstTaskDays=-1),
        usage_payload(firstTaskDays=100001),
        usage_payload(firstTaskDays="7"),
        usage_payload(firstTaskDays=True),
        {k: v for k, v in usage_payload().items() if k != "firstTaskDays"},
        dict(usage_payload(), surprise=1),
        {},
    ]

    def test_invalid_payloads_rejected(self):
        for bad in self.DATA_DRIVEN_BAD_PAYLOADS:
            status, _, data = self.post_usage(bad)
            self.assertEqual(status, 400, "%r -> %s %r" % (bad, status, data))
        # 全部被拒：库里什么都没存
        self.assertEqual(self.get_summary()["users"], 0)

    def test_non_object_body_rejected(self):
        for raw in ("[]", '"x"', "5", "null", "", "not json"):
            status, _, _ = self.post_usage(raw_body=raw)
            self.assertEqual(status, 400, raw)

    def test_duplicate_keys_rejected(self):
        raw = ('{"schemaVersion":1,"schemaVersion":1,"tasksCreated":0,'
               '"notesCreated":0,"tasksCompleted":0,"firstTaskDays":null}')
        status, _, _ = self.post_usage(raw_body=raw)
        self.assertEqual(status, 400)

    def test_invalid_utf8_rejected(self):
        status, _, _ = self.post_usage(raw_body=b"\xff\xfe{}")
        self.assertEqual(status, 400)

    def test_missing_content_length(self):
        # 手工发不带 Content-Length 的 POST
        sock = socket_connection(self.port)
        try:
            sock.sendall(b"POST /api/v1/stats/usage HTTP/1.1\r\n"
                         b"Host: 127.0.0.1\r\nContent-Type: application/json\r\n"
                         b"\r\n")
            status_line = read_http_status(sock)
        finally:
            sock.close()
        self.assertEqual(status_line, 400)

    def test_body_limit_8kib(self):
        base = json.dumps(usage_payload()).encode("utf-8")
        over = base + b" " * (8 * 1024 + 1 - len(base))
        self.assertEqual(len(over), 8 * 1024 + 1)
        status, _, _ = self.post_usage(raw_body=over)
        self.assertEqual(status, 413)
        self.assertEqual(self.get_summary()["users"], 0)

        exact = base + b" " * (8 * 1024 - len(base))
        self.assertEqual(len(exact), 8 * 1024)
        status, _, _ = self.post_usage(raw_body=exact)
        self.assertEqual(status, 200)
        self.assertEqual(self.get_summary()["users"], 1)

    def test_ipv6_normalized_forms_dedupe_to_one_user(self):
        for header_value in ("::FFFF:192.168.7.7", "::ffff:192.168.7.7"):
            status, _, _ = self.post_usage(headers={"X-Real-IP": header_value})
            self.assertEqual(status, 200)
        self.assertEqual(self.get_summary()["users"], 1)

    def test_same_ip_reports_not_amplified_over_http(self):
        for count in (50, 41, 7):
            status, _, _ = self.post_usage(
                usage_payload(tasksCreated=count),
                headers={"X-Real-IP": "10.1.1.1"})
            self.assertEqual(status, 200)
        summary = self.get_summary()
        self.assertEqual(summary["users"], 1)
        self.assertEqual(summary["usage"]["tasksCreated"], 7)

    def test_real_ip_header_precedence(self):
        cases = [
            ({"X-Real-IP": "10.0.0.1"}, "10.0.0.1"),
            ({"X-Forwarded-For": "10.0.0.2, 10.0.0.99"}, "10.0.0.2"),
            ({"X-Real-IP": "10.0.0.3", "X-Forwarded-For": "10.0.0.2"}, "10.0.0.3"),
            ({"X-Real-IP": "bogus", "X-Forwarded-For": "10.0.0.4"}, "10.0.0.4"),
            ({"X-Real-IP": "bogus", "X-Forwarded-For": "also bogus"}, "127.0.0.1"),
        ]
        for headers, _ in cases:
            status, _, _ = self.post_usage(headers=headers)
            self.assertEqual(status, 200)
        self.assertEqual(self.get_summary()["users"], 5)

    def test_store_failure_returns_500(self):
        with mock.patch.object(
                self.app.store, "record_usage",
                side_effect=sqlite3.OperationalError("disk I/O error")):
            status, _, data = self.post_usage()
            self.assertEqual(status, 500)
            self.assertFalse(json.loads(data)["ok"])
        # 故障恢复后服务仍可用
        status, _, _ = self.post_usage()
        self.assertEqual(status, 200)


class TestHttpRouting(HttpTestFixture):
    def test_method_not_allowed_with_allow_header(self):
        status, headers, _ = self.request("GET", "/api/v1/stats/usage")
        self.assertEqual(status, 405)
        self.assertEqual(dict(headers).get("Allow"), "POST")

        for method in ("POST", "PUT", "DELETE", "OPTIONS", "PATCH"):
            status, _, _ = self.request(method, "/api/v1/stats/summary")
            self.assertEqual(status, 405, method)
        status, headers, _ = self.request("DELETE", "/api/v1/stats/healthz")
        self.assertEqual(status, 405)
        self.assertEqual(dict(headers).get("Allow"), "GET")

    def test_method_not_allowed_drains_body(self):
        # 405 响应后连接仍可正常收尾（客户端能读到完整响应）
        status, _, data = self.request("GET", "/api/v1/stats/usage",
                                       body=json.dumps(usage_payload()),
                                       headers={"Content-Type": "application/json"})
        self.assertEqual(status, 405)
        self.assertFalse(json.loads(data)["ok"])

    def test_unknown_paths_404(self):
        for method, path in (("GET", "/"), ("GET", "/api/v1/stats/"),
                             ("GET", "/api/v1/stats/usage/"),
                             ("POST", "/api/v1/stats/other"),
                             ("GET", "/api/v1/stats/summaryx"),
                             ("GET", "/logs/stats.access.log"),
                             ("GET", "/logs/stats.access.log.1")):
            status, _, _ = self.request(method, path)
            self.assertEqual(status, 404, "%s %s" % (method, path))

    def test_known_path_with_query_routes_by_path(self):
        # 路由只看路径：带查询串的 summary 是 GET → 200；带查询串的 usage 是 GET → 405
        status, _, _ = self.request("GET", "/api/v1/stats/summary?verbose=1")
        self.assertEqual(status, 200)
        status, _, _ = self.request("GET", "/api/v1/stats/usage?x=1")
        self.assertEqual(status, 405)


class TestHttpSummary(HttpTestFixture):
    def test_summary_shape_and_privacy(self):
        for ip, count in (("10.1.1.1", 3), ("10.2.2.2", 4)):
            self.post_usage(usage_payload(tasksCreated=count, firstTaskDays=9),
                            headers={"X-Real-IP": ip})
        write_text(os.path.join(self.logs_dir, "stats.access.log"),
                   dl("d1", version="1.1.0", ip="10.20.30.40") + "\n"
                   + dl("d2", version="1.1.0", ip="10.20.30.41") + "\n"
                   + dl("d3", version="1.2.0", ip="10.1.1.1") + "\n")

        status, _, data = self.request("GET", "/api/v1/stats/summary")
        self.assertEqual(status, 200)
        raw = data.decode("utf-8")
        summary = json.loads(data)

        self.assertEqual(set(summary),
                         {"schemaVersion", "generatedAt", "users", "usage", "downloads"})
        self.assertEqual(summary["schemaVersion"], 1)
        # users：10.1.1.1、10.2.2.2（usage）∪ 10.20.30.40、10.20.30.41（下载）= 4
        self.assertEqual(summary["users"], 4)
        self.assertEqual(summary["usage"], {
            "tasksCreated": 7, "notesCreated": 4, "tasksCompleted": 6,
            "longestUsageDays": 9,
        })
        self.assertEqual(summary["downloads"]["total"], 3)
        self.assertEqual(summary["downloads"]["byVersion"],
                         [{"version": "1.1.0", "count": 2},
                          {"version": "1.2.0", "count": 1}])
        # 隐私：任何 IP 明细都不得出现（键或值）
        for leaked in ("10.1.1.1", "10.2.2.2", "10.20.30.40", "10.20.30.41",
                       "127.0.0.1"):
            self.assertNotIn(leaked, raw)
        self.assertEqual(collect_keys(summary), {"schemaVersion", "generatedAt",
                                                 "users", "usage", "downloads",
                                                 "tasksCreated", "notesCreated",
                                                 "tasksCompleted", "longestUsageDays",
                                                 "total", "byVersion", "version",
                                                 "count"})

    def test_summary_triggered_by_log_import(self):
        write_text(os.path.join(self.logs_dir, "stats.access.log"),
                   dl("d1") + "\n" + dl("d1") + "\n" + dl("d2") + "\n")
        summary = self.get_summary()
        self.assertEqual(summary["downloads"]["total"], 2)
        # 再次请求不重复计数
        self.assertEqual(self.get_summary()["downloads"]["total"], 2)

    def test_summary_accepts_query_string(self):
        status, _, _ = self.request("GET", "/api/v1/stats/summary?verbose=1")
        self.assertEqual(status, 200)

    def test_concurrent_posts_and_summaries(self):
        errors = []

        def worker(index):
            try:
                for count in (2, 5, 9):
                    status, _, _ = self.post_usage(
                        usage_payload(tasksCreated=count),
                        headers={"X-Real-IP": "10.60.0.%d" % index})
                    if status != 200:
                        errors.append(("post", index, status))
            except Exception as exc:  # pragma: no cover - 记录线程内异常
                errors.append(("exception", index, repr(exc)))

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(1, 9)]
        for thread in threads:
            thread.start()
        self.get_summary()  # 导入与写入并发
        for thread in threads:
            thread.join(timeout=10)

        self.assertEqual(errors, [])
        summary = self.get_summary()
        self.assertEqual(summary["users"], 8)
        # 每 IP 只保留最新快照（最后一次 tasksCreated=9），不随重复上报放大
        self.assertEqual(summary["usage"]["tasksCreated"], 8 * 9)


class TestMainEnv(unittest.TestCase):
    def test_invalid_port_returns_error(self):
        env = {"STATS_PORT": "not-a-port", "STATS_DB": ":memory:"}
        with mock.patch.dict(os.environ, env):
            self.assertEqual(stats_app.main(), 2)


# --------------------------------------------------------------------------
# 辅助
# --------------------------------------------------------------------------

def collect_keys(value, keys=None):
    keys = set() if keys is None else keys
    if isinstance(value, dict):
        for key, item in value.items():
            keys.add(key)
            collect_keys(item, keys)
    elif isinstance(value, list):
        for item in value:
            collect_keys(item, keys)
    return keys


def socket_connection(port):
    import socket
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    return sock


def read_http_status(sock):
    data = b""
    while b"\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    status_line = data.split(b"\r\n", 1)[0]
    return int(status_line.split(b" ")[1])


if __name__ == "__main__":
    unittest.main(verbosity=2)
