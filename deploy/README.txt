========================================
KylinList 可信内网生产部署包（1.3.0）
========================================

本目录自包含：版本化镜像 tar、Compose、宿主 nginx 配置、安装包与更新清单。
生产客户端默认入口为 http://143.147.10.31:11924，无需手填服务器地址。
截至 2026-11-14，已在开发机完成 tar 校验、离线导入与本机生产 Compose 演练；
尚未在真实生产宿主或麒麟/飞腾真机执行，实际部署仍需用户验收。

安全边界：纯 HTTP、无鉴权，只允许可信内网访问，严禁暴露公网。
统计只上报聚合数量；服务器按 IP 识别用户，同一 IP 的多次上报只保留最新快照。
聚合结果只在 /s 管理页（http://143.147.10.31:11924/s/），首页无入口，
无鉴权，仅限可信内网管理员访问，API 不返回 IP 明细。

----------------------------------------
一、开发机生成部署目录
----------------------------------------

先用现有 Docker/CI 流程生成 release 目录（updates/ + RELEASE-INFO.json），然后：

  bash deploy/package.sh <release目录> 1.3.0

脚本会：
  1. 核对版本与 CMakeLists.txt 一致；
  2. 构建 kylinlist-http / publisher / stats 三个镜像；
  3. 用 publisher validate 校验安装包大小与 sha256；
  4. 把 release 拷贝到 data/incoming/kylinlist-1.3.0/；
  5. 导出 images/*.tar，并生成 SHA256SUMS。

tar、.env.prod、安装包、运行数据均不入 Git。同版本 incoming 目录拒绝覆盖；
如确需重导同名镜像 tar，可设 FORCE=1，但这不能绕过同版本安装包不可替换规则。

----------------------------------------
二、首次部署到生产机
----------------------------------------

1) 把整个 deploy 目录拷贝到生产机，例如 /opt/kylinlist/。

2) 导入镜像（版本号以本次为准）：

   cd /opt/kylinlist
   docker load -i images/kylinlist-http-1.3.0.tar
   docker load -i images/kylinlist-publisher-1.3.0.tar
   docker load -i images/kylinlist-stats-1.3.0.tar

3) 配置并检查：

   cp -n .env.example .env.prod   # package.sh 已生成时跳过
   编辑 .env.prod，确认 KYLINLIST_IMAGE_VERSION=1.3.0、HTTP_PORT=18080。
   如果修改 HTTP_PORT，也要同步修改 host_nginx.conf 的 proxy_pass 回源端口。
   sha256sum -c SHA256SUMS

4) 校验并发布安装包：

   docker compose --env-file .env.prod -f docker-compose-prod.yml run --rm publisher \
     validate /data/incoming/kylinlist-1.3.0
   docker compose --env-file .env.prod -f docker-compose-prod.yml run --rm publisher \
     publish /data/incoming/kylinlist-1.3.0

5) 启动服务：

   docker compose --env-file .env.prod -f docker-compose-prod.yml up -d
   curl http://127.0.0.1:18080/healthz
   curl http://127.0.0.1:18080/api/v1/stats/healthz

6) 接入宿主 nginx（必须经此入口，才能获取真实 IP）：

   sudo cp host_nginx.conf /etc/nginx/conf.d/kylinlist.conf
   sudo nginx -t && sudo systemctl reload nginx
   curl http://127.0.0.1:11924/healthz

生产防火墙只允许可信内网访问 TCP 11924；不要对外开放 18080，也不要把容器端口改为 0.0.0.0。

----------------------------------------
三、升级与回滚
----------------------------------------

升级：开发机用新版本运行 package.sh，整体拷贝新 deploy 内容到生产机，保留生产 data/public、
data/stats 与 data/branding；导入新 tar，把 .env.prod 的镜像版本改为新版本，publish 新 incoming，
最后 up -d。本次若 host_nginx.conf 有变化，还需重新复制并 nginx -t / reload。

回退更新通道（不会回滚程序或统计数据）：

  docker compose --env-file .env.prod -f docker-compose-prod.yml run --rm publisher rollback
  # 或指定版本：... rollback --to 1.2.0

回滚镜像：保留旧 tar，.env.prod 指回旧镜像版本后 up -d。安装包历史目录不要手工删除。

----------------------------------------
四、统计、品牌与日志
----------------------------------------

  GET  /api/v1/stats/summary       聚合统计，不返回 IP 明细
  POST /api/v1/stats/usage         客户端每小时聚合上报，body 最大 8KiB
  /s/                              聚合统计管理页（首页无入口，无鉴权，
                                   仅限可信内网管理员访问；/s 会以相对
                                   Location 跳转到 /s/）

客户端“使用统计”窗口只显示本机累计数据，不含全局统计、下载量与上报状态。

网站品牌覆盖在运行数据 data/branding/site/（挂载为容器内
/usr/share/nginx/html/local）：编辑 config.json 的
displayName/title/description/logo，值为空即不覆盖；修改后刷新生效，
不需要重建镜像或重启容器，/s 管理页同样生效。该目录不入 Git，
生产值在生产机自行填写。客户端名称/Logo 覆盖见 docs/branding.md。

下载量来自 nginx 实际 GET/200 安装包日志，不是网页按钮点击数；按 request_id 去重。
日志轮转：

  bash rotate-logs.sh

脚本先触发下载日志导入，再轮转并让 nginx 重开文件，90 天后清理已导入旧日志。
可由生产机 cron 每日调用。容器 stdout 由 Compose 限制为 10MiB × 3。

常用维护：

  docker compose --env-file .env.prod -f docker-compose-prod.yml ps
  docker compose --env-file .env.prod -f docker-compose-prod.yml logs -f http stats
  docker compose --env-file .env.prod -f docker-compose-prod.yml down
