# KylinList 内网统计服务：仅标准库、无包安装、无 pip、完全非交互。
FROM python:3.12-alpine

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    STATS_DB=/data/stats.sqlite3 \
    STATS_LOGS_DIR=/logs \
    STATS_HOST=0.0.0.0 \
    STATS_PORT=8000

COPY app.py /app/app.py

RUN mkdir -p /data /logs

# 个人内网单容器：保持镜像默认用户（root），宿主 bind mount 的 /data、/logs
# 无需额外 chown 即可用；如需收紧请自行以匹配 uid 运行。
# 数据全部走宿主 bind mount（见 compose），不声明 VOLUME，避免匿名卷残留。
EXPOSE 8000

HEALTHCHECK --interval=60s --timeout=5s --start-period=10s --retries=3 \
    CMD ["python", "-c", "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/api/v1/stats/healthz', timeout=3)"]

# -I：隔离模式，忽略 PYTHON* 环境变量与用户 site-packages
CMD ["python", "-I", "/app/app.py"]
