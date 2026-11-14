# server/stats — 轻量可信内网统计服务

单容器、个人内网用。Python 3.12 仅标准库（`ThreadingHTTPServer` + `sqlite3`），
无任何第三方依赖。职责：

1. 接收客户端用量快照 `POST /api/v1/stats/usage`（按真实 IP 保存**最新一份**，不累加）。
2. 扫描宿主 nginx 的 JSON 访问日志，统计 `/updates/files/<X.Y.Z>/` 安装包下载数
   （`request_id` 全局去重，支持 `.log` / `.json` / `.log.N` / `*.gz` 轮转，断点续读）。
3. `GET /api/v1/stats/summary` 输出聚合并结果，**绝不输出 IP 明细**；无原始日志 API。

## 运行

```bash
docker build -t kylinlist-stats server/stats
docker run -d --name kylinlist-stats \
  -p 8000:8000 \
  -v /srv/kylinnote/stats-data:/data \
  -v /srv/kylinnote/logs:/logs:ro \
  kylinlist-stats
```

环境变量（均有默认值）：`STATS_DB`（默认 `/data/stats.sqlite3`）、`STATS_LOGS_DIR`
（默认 `/logs`）、`STATS_HOST`（默认 `0.0.0.0`）、`STATS_PORT`（默认 `8000`）、
`STATS_LOG_LEVEL`（默认 `INFO`）。

## API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| POST | `/api/v1/stats/usage` | `application/json`，体 ≤ 8 KiB；严格对象，仅允许 `schemaVersion`(=1)、`tasksCreated`、`notesCreated`、`tasksCompleted`（整数 0..1e9）、`firstTaskDays`（null 或整数 0..100000）。成功 `{"ok":true}`；非法 400，超限 413，写库失败 500。 |
| GET | `/api/v1/stats/summary` | `{schemaVersion, generatedAt, users, usage{tasksCreated,notesCreated,tasksCompleted,longestUsageDays}, downloads{total,byVersion[]}}`；`users` 为用量 IP 与有效下载 IP 的并集数。 |
| GET | `/api/v1/stats/healthz` | `{"ok":true}` |

其他路径 404，方法不符 405。真实 IP 取值顺序：`X-Real-IP` > `X-Forwarded-For` 首段 >
`remote_addr`，`ipaddress` 校验规范化，全部无效则 400 且不落库。**只在宿主 nginx
反代并覆盖写入这两个头的前提下信任它们。**

## 宿主 nginx 配套建议（未改动现有 server/nginx.conf，接入时参考）

```nginx
# JSON 行日志（escape=json 保证单行合法 JSON；buffer/flush 可按需调整）
log_format stats_json escape=json
  '{"request_id":"$request_id","time":"$time_iso8601","ip":"$remote_addr",'
  '"method":"$request_method","uri":"$request_uri","status":$status,'
  '"bytes_sent":$bytes_sent}';
access_log /srv/kylinnote/logs/stats.access.log stats_json;

# 反代统计服务：覆盖写入真实 IP 头
location /api/v1/stats/ {
    proxy_pass http://127.0.0.1:8000;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    client_max_body_size 16k;
}
```

## 设计要点

- SQLite：WAL + `busy_timeout=30s`，每请求独立连接与事务；下载记录与日志文件
  offset checkpoint 同事务提交，崩溃后至多重读、不重计（`request_id` 主键去重）。
- 日志扫描只在 summary 时增量触发；明文文件按“文件身份(inode/size)+offset”断点
  续读且只处理完整行，`.gz` 视为轮转终态整读一次；坏行/半截行安全跳过。
- 已导入历史保存在 SQLite 中：日志文件被移走/清空也不丢统计。
- 应用日志（请求行/错误栈）输出 stdout，不记录任何请求体内容。

## 测试

```bash
python -I test_stats.py        # 或：python -I -m unittest -v test_stats
```

只使用临时目录与合成数据，不访问任何真实服务器。
