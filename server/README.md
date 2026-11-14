# KylinList 内网更新发布服务

给局域网内的麒麟清单客户端（银河麒麟 ARM64 / amd64 为主）提供版本清单与安装包下载的纯 HTTP 服务，同时承载同源产品首页与聚合统计。三个 Compose 服务均为常驻：`http`（nginx、静态站点与统计反代）、`stats`（聚合统计后端）、`publisher`（监视 incoming 并自动发布）。同一个 publisher 镜像也用于显式 status/rollback。

> 安全边界：**纯内网 HTTP，无 TLS、无鉴权**。SHA-256 用于完整性校验，不是来源签名。统计接口与 `/s/` 同样无鉴权，只返回聚合值、不返回 IP 明细。**只允许部署在可信局域网内，严禁暴露到公网。**统计口径见 [docs/stats.md](../docs/stats.md)。

## 目录职责

| 文件 / 目录 | 职责 |
|---|---|
| `compose.yaml` | http/stats/publisher 三个常驻服务；publisher 执行 `watch /data/incoming` |
| `nginx.conf` | 清单 no-cache、安装包 immutable、下载 JSON 日志、统计转发、相对目录跳转 |
| `.env.example` | 开发端口与服务端镜像版本 |
| `site/` | 同源产品首页与 `/s/` 统计管理页，无外部资源 |
| `stats/` | 聚合统计后端，仅 Python 标准库 |
| `tools/publish.py` | 校验、自动 watch、history、原子 stable、显式恢复/回滚 |
| `data/` | 全部宿主运行数据，bind mount，不入库 |

所有业务持久化使用 `./data/...` 宿主目录 bind mount，不使用 named/anonymous volume。镜像也不声明 `VOLUME`；容器 stdout 的 json-file 日志不属于数据卷。

## 首次启动

前置：宿主机已有 Docker 与 Compose 插件。开发机可直接构建服务端镜像；生产离线镜像由 `kylinlist-deploy/package-images.sh` 单独生成，不需要构建客户端。

```bash
cd server
cp .env.example .env        # 按需修改 HTTP_PORT 与服务端镜像版本
docker compose up -d
curl http://127.0.0.1:8080/healthz
curl http://127.0.0.1:8080/api/v1/stats/healthz
```

客户端更新基础地址为 `http://<宿主机IP>:<HTTP_PORT>`。除已确认的客户端默认生产入口外，不要在文档或示例中新增其他真实内网 IP。生产固定拓扑、宿主 nginx 与真实 IP 配置见根目录 `kylinlist-deploy/README.txt`。

## 产品首页、/s 统计页与品牌覆盖

- `/` 直接读取 stable 清单提供下载与完整更新日志，首页没有 `/s` 入口。
- `/s/` 展示不同 IP 用户数、任务/笔记/完成合计、最久使用天数与各版本下载次数；无鉴权，仅限可信内网管理员自行访问。`/s` 使用相对 `Location: /s/` 跳转，不丢失端口。
- 客户端“使用统计”窗口只显示本机数据，全局聚合只在此页。
- 网站品牌覆盖放在 `data/branding/site/`（只读挂载到 `/usr/share/nginx/html/local`）：`config.json` 的 `displayName/title/description/logo`，空值不覆盖；修改后刷新生效，无需重建镜像。客户端和麒麟系统入口见 [docs/branding.md](../docs/branding.md)。

## 日常发布：只投递 GitHub CI release

客户端安装包不在开发机本地编译。CI 的三个构建目标全部成功后，`update-bundle` 汇总四个 Linux deb、Windows setup/portable、manifest、公告和 RELEASE-INFO。公告来自 `docs/release-notes/<版本>.txt`，缺失时 CI 不生成发布包。

在开发机仓库根目录：

```bash
python -I kylinlist-deploy/fetch_ci_release.py <RUN_ID> dist/ci-release
bash kylinlist-deploy/package.sh dist/ci-release
```

`fetch_ci_release.py` 使用已有 `gh` 下载并交叉核验指定成功运行；只接受本仓库的 push / workflow_dispatch，不接受 PR 或 fork 运行。`package.sh` 要求完整 CI 六目标、非 partial、匹配的 headSha/run 信息，调用现有校验器核对所有文件，然后生成：

```
kylinlist-deploy/data/incoming/kylinlist-<版本>/
  updates/...
  RELEASE-INFO.json
  SHA256SUMS
  READY
```

将目录复制到服务器的 `data/incoming/.kylinlist-<版本>.uploading/`，全部完成后在同一文件系统原子改名为 `data/incoming/kylinlist-<版本>/`。publisher 自动处理，不需要执行 publish、重启容器或更新服务端镜像。

### watch 的处理规则

- 只处理带 `READY` 和 `SHA256SUMS` 的正式版本目录；隐藏暂存、旧式无 READY 目录不读取。
- 逐项校验 SHA256SUMS、CI 元数据、manifest 和安装包大小/SHA-256，再复用原有发布与 history 合并逻辑。
- 每个候选写 `.publish-state.json`；succeeded/failed/skipped 不重复消费。失败输入修复后作为新目录重新投递。
- 多候选按数值版本升序；低于当前 stable 的候选 skipped，不自动降级。等于当前版本只允许幂等恢复。
- 已成功目录在管理员 rollback 后不会自动升回。watch、手工 publish 和 rollback 共用 `data/.publisher.lock`。
- CI 字段是流程与溯源门禁，不是密码学来源证明；实际来源由从可信 GitHub 仓库/运行下载来保证。

完整命令与生产目录见 [kylinlist-deploy/README.txt](../kylinlist-deploy/README.txt)。

## 显式恢复、回滚与查看

日常不需要这些命令；它们用于故障恢复和管理员回滚：

```bash
# 开发 server/ 目录；生产改用 kylinlist-deploy/README.txt 中的 compose 文件与 .env.prod
docker compose run --rm publisher status
docker compose run --rm publisher validate /data/incoming/<release目录>
docker compose run --rm publisher publish /data/incoming/<release目录>
docker compose run --rm publisher rollback
docker compose run --rm publisher rollback --to 1.2.0
```

- 发布保留事务边界：安装包逐个原子落位，stable 最后原子替换；不把整批目录宣称为单一事务。
- 同版本同哈希幂等；同版本同路径不同内容拒绝。公告历史由 stable、归档与新 release 合并，回滚也保留已知历史。
- RELEASE-INFO 在幂等恢复时也会补齐归档；同内容归档不重复。
- publisher 工具内置在版本化镜像中；修改 `server/tools/` 后先 `docker compose build publisher`，避免运行旧代码。
- 在 Windows Git Bash 中，容器路径参数前使用 `MSYS_NO_PATHCONV=1`。

## 运行测试

```bash
docker compose run --rm --entrypoint python publisher -I /tools/test_publish.py
docker run --rm -v "$(pwd)/stats":/stats:ro --entrypoint python \
  kylinlist-stats:${KYLINLIST_IMAGE_VERSION:-1.3.0} -I /stats/test_stats.py
```

发布器测试包含目录 READY、哈希、状态去重、版本顺序、锁、回滚和崩溃恢复；统计测试使用临时目录与合成数据。均不访问真实服务器。

## 离线镜像

服务端镜像与客户端 release 已解耦：

```bash
bash kylinlist-deploy/package-images.sh <服务端镜像版本>   # 仅服务自身变化时
bash kylinlist-deploy/package.sh <完整CI release目录>      # 日常客户端发布
```

前者生成 `kylinlist-deploy/images/*.tar` 与独立 SHA256SUMS，后者只生成 incoming。生产导入、升级和回滚步骤见 kylinlist-deploy 文档。

## 常见问题

- **清单没有变化**：先查看 `docker compose logs publisher` 和候选目录 `.publish-state.json`；确认复制后已从 `.uploading` 原子改名，且 READY/SHA256SUMS 齐全。
- **为什么必须是暂存目录再改名**：避免 watcher 在大型安装包尚未复制完成时读到半成品。
- **failed 后为什么不反复重试**：防止永久冲突持续刷日志；修正后重新投递新目录。
- **清单看起来是旧的**：/updates/v1/ 已 no-cache；核对 stable、状态文件以及反向代理缓存。
- **回退报文件缺失**：历史安装包目录已被删除；从完整 CI release 恢复后再回滚。
- **healthz 不通**：检查 Compose 状态、端口、防火墙和宿主 nginx。
- **经过路径前缀反代**：客户端基础地址包含前缀，manifest 的 path 仍相对 `/updates/`；反代需原样转发路径与 Cache-Control。
