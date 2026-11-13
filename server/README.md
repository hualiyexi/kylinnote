# KylinList 内网更新发布服务

给局域网内的麒麟清单客户端（银河麒麟 ARM64 / amd64 为主）提供版本清单与安装包下载的纯 HTTP 服务。由两个 Compose 服务组成：常驻的 `http`（nginx，只读分发 `data/public`），以及按需运行的 `publisher`（Python 发布工具，宿主机无需安装 Python）。

> 安全边界：**纯内网 HTTP，无 TLS、无鉴权**。本服务只做文件完整性（SHA-256）校验，不含任何证书或签名机制。**只允许部署在可信局域网内，严禁暴露到公网。**

## 目录职责

| 文件 / 目录 | 职责 |
|---|---|
| `compose.yaml` | 两个服务的定义；`http` 常驻，`publisher` 挂 `tools` profile 按需运行 |
| `nginx.conf` | nginx conf.d 片段：清单 no-cache、安装包 immutable、无目录列表 |
| `.env.example` | 环境变量样例（复制为 `.env`，当前只有 `HTTP_PORT`） |
| `tools/publish.py` | 发布工具（仅标准库，容器内以 `python -I` 运行） |
| `tools/test_publish.py` | 发布工具的单元测试（unittest） |
| `data/` | 运行数据，不入库；结构见 `data/README.md` |

## 首次部署

前置：宿主机已安装 Docker 与 Compose 插件，且能拉到 `nginx:1.27-alpine`、`python:3.12-alpine`（离线导入见下文）。

```bash
cd server
cp .env.example .env        # 按需修改 HTTP_PORT
mkdir -p data/public
docker compose up -d http
curl http://127.0.0.1:8080/healthz   # 应输出 ok（端口以 .env 为准）
```

客户端更新基础地址即 `http://<宿主机IP>:<HTTP_PORT>`。不要把真实内网 IP 写进任何文件。

## 发布目录从哪来

发布工具的输入是 CI 的 `kylinlist-update-bundle` 产物解包出的 **release 目录**（也可以手工组织成同构结构）：

```
<release-dir>/
  updates/
    v1/stable/manifest.json                          # 版本清单
    files/<版本>/<platform>-<arch>-<variant>/<原文件名>   # 安装包
  RELEASE-INFO.json                                  # 版本与来源说明（可选；发布工具只透传归档，不解析）
```

清单内 `artifact.path` 相对于「服务器基础地址 + `/updates/`」，即客户端最终下载地址为 `<base>/updates/files/1.2.0/linux-arm64-system-qt/xxx.deb`。

## 发布 / 校验 / 回退 / 查看命令

约定把解包出的 release 目录放在 `data/incoming/` 下（`data` 已挂载进 publisher 容器），然后：

```bash
mkdir -p data/incoming
unzip kylinlist-update-bundle.zip -d data/incoming/   # 解包出 updates/ 与 RELEASE-INFO.json

docker compose run --rm publisher validate /data/incoming/<release目录>   # 只校验，不落盘
docker compose run --rm publisher publish   /data/incoming/<release目录>   # 校验 + 安装 + 原子更新清单
docker compose run --rm publisher status                                 # 当前版本 / 文件 / 归档
docker compose run --rm publisher rollback                              # 回退到最近一份早于当前的归档
docker compose run --rm publisher rollback --to 1.1.0                    # 回退到指定版本
```

说明：

- 发布成功后原 stable 清单自动归档到 `data/public/updates/v1/archive/manifest-<版本>-<UTC时间戳>.json`；`RELEASE-INFO.json` 存在时也原样透传归档一份（`release-info-<版本>-<时间戳>.json`），发布工具从不解析它。
- 同一版本重复发布：文件哈希完全一致时幂等成功；内容不同则拒绝（同版本禁止原地替换），请升版本号。
- 回退要求目标清单指向的安装包仍存在于 `data/public/updates/files/`，不要手工删除历史版本目录。
- 不想复制到 `data/incoming/` 时，也可以临时多挂一个目录：`docker compose run --rm -v /宿主机/路径/release:/release:ro publisher publish /release`。

## 运行发布工具的测试

```bash
docker compose run --rm --entrypoint python publisher -I /tools/test_publish.py
```

（`--entrypoint python` 覆盖 compose 入口，在容器内直接跑 `python -I /tools/test_publish.py`；宿主机不用装 Python。）

## 离线导入镜像（宿主机可能无外网）

在有外网的机器上：

```bash
docker pull nginx:1.27-alpine
docker pull python:3.12-alpine
docker save nginx:1.27-alpine python:3.12-alpine -o kylin-update-images.tar
```

把 tar 传到内网宿主机后：

```bash
docker load -i kylin-update-images.tar
```

然后照常 `docker compose up -d http`。

## 常见问题

- **端口被占用**：修改 `.env` 中的 `HTTP_PORT`，再 `docker compose up -d http` 重建容器。
- **经过反向代理 / 带路径前缀**：若经由反代加路径前缀对外（例如 `http://内网网关/kylinlist/` 转发到本服务），客户端只需把基础地址写成 `http://<host>:<port>/<前缀>`；manifest 内 `path` 是相对 `/updates/` 的，**服务端无需任何改动**。反代需原样转发路径与 `Cache-Control` 响应头。
- **清单看起来是旧的**：`/updates/v1/` 已设 `no-cache`；先用 `status` 确认发布是否成功，再检查反代是否额外加了缓存。
- **回退报文件缺失**：目标历史版本的安装包已不在 `files/` 下；发布新版本或从 CI 产物重新发布该版本。
- **healthz 不通**：`docker compose ps` 确认 http 在运行；检查 `.env` 端口与防火墙。
- **在 Windows Git Bash 里执行 publisher 命令**：Git Bash 会把 `/data/...` 这类参数改写成宿主机路径，需在命令前加 `MSYS_NO_PATHCONV=1`（Linux 宿主机上无此问题）。
