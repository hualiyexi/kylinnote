#!/usr/bin/env bash
# 在生产机按需或由 cron 每日执行。先导入统计，再轮转并通知 nginx 重开日志。
set -euo pipefail
cd "$(dirname "$0")"
COMPOSE=(docker compose --env-file .env.prod -f docker-compose-prod.yml)
HTTP_PORT=$(sed -n 's/^HTTP_PORT=//p' .env.prod | head -n 1)
HTTP_PORT=${HTTP_PORT:-18080}
curl -fsS "http://127.0.0.1:$HTTP_PORT/api/v1/stats/summary" >/dev/null
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
MOVED=0
for name in downloads.json.log access.log error.log; do
    if [[ -f "data/logs/$name" ]]; then
        mv "data/logs/$name" "data/logs/$name.$STAMP"
        MOVED=1
    fi
done
if [[ "$MOVED" == "1" ]]; then "${COMPOSE[@]}" kill -s USR1 http; fi
# 已导入 SQLite 的历史不会因清理旧日志而丢失。
find data/logs -type f \( -name 'downloads.json.log.2*' -o -name 'access.log.2*' -o -name 'error.log.2*' \) \
    -mtime +90 -delete
