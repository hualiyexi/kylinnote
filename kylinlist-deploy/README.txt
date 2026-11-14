========================================
KylinList 可信内网生产部署
========================================

安全边界：纯 HTTP、无鉴权，只允许可信内网访问，严禁暴露公网。
统计只上报聚合数量；服务器按 IP 识别用户，同一 IP 只保留最新快照。
聚合结果由管理员自行访问 /s/，首页无入口，API 不返回 IP 明细。

核心原则：
1. 客户端安装包只使用 GitHub Actions 的 kylinlist-update-bundle，不在开发机编译。
2. 日常更新只投递 release 目录；publisher 常驻监视并自动发布，不重启服务。
3. 服务端镜像只在服务自身变化时单独打包，版本号不与客户端版本绑定。
4. 清单、安装包、统计 SQLite、品牌和 nginx 日志全部使用宿主 bind mount，
   不使用 Docker named/anonymous volume。容器 stdout 的 json-file 日志不是数据卷。

----------------------------------------
一、日常发布：CI 下载 → 本地打包 → 拷贝
----------------------------------------

1) 在 GitHub Actions 确认目标 build.yml 运行的全部 job 成功，复制 RUN_ID。
   只选本仓库的 push / workflow_dispatch 运行，不使用 Pull Request 或 fork 运行。
   在开发机仓库根目录执行（需要本机已有 gh 和 Python）：

   python -I kylinlist-deploy/fetch_ci_release.py RUN_ID dist/ci-release
   bash kylinlist-deploy/package.sh dist/ci-release

   fetch_ci_release.py 会核验 GitHub run、workflow、headSha，并安全解压到全新目录。
   package.sh 只校验并复制 CI release，不编译客户端，也不构建/导出服务端镜像。
   RELEASE-INFO 必须来自完整 CI 六目标，partial=true 或本地手工包会被拒绝。

   CI 更新公告来自源码中的 docs/release-notes/<版本>.txt；服务器自动合并历史公告。

2) 打包结果：

   kylinlist-deploy/data/incoming/kylinlist-<版本>/
     updates/...             安装包与原始清单
     RELEASE-INFO.json       CI 来源与文件信息
     SHA256SUMS              逐文件完整性校验
     READY                   最后生成的就绪标记

3) 复制到生产机时，先使用同文件系统内的隐藏暂存目录：

   /opt/kylinlist/data/incoming/.kylinlist-<版本>.uploading/

   整个目录复制完成并确认无写入后，在生产机执行一次原子改名：

   cd /opt/kylinlist/data/incoming
   test ! -e kylinlist-<版本>   # 已存在时先移走旧失败目录，不要嵌套覆盖
   mv -T .kylinlist-<版本>.uploading kylinlist-<版本>

   必须先隐藏暂存、后改名。publisher 只消费带 READY/SHA256SUMS 的正式目录，
   不读取 .uploading。不要直接向正式目录边复制边发布。

4) publisher 自动校验 CI 来源、SHA256SUMS、manifest 和安装包哈希，然后安装文件、
   合并 history 并原子替换 stable。无需执行 publish 命令，无需 restart/up。

   查看状态：

   cd /opt/kylinlist
   docker compose --env-file .env.prod -f docker-compose-prod.yml logs -f publisher
   curl http://127.0.0.1:18080/updates/v1/stable/manifest.json

   每个输入目录写 .publish-state.json。succeeded/failed/skipped 不会每轮重复处理；
   修复失败输入时，先移走该 failed incoming，再重新投递一个完整的新目录。
   同版本安装包仍禁止原地替换。旧版本自动投递只会 skipped，不会自动降级。

----------------------------------------
二、首次部署或服务端镜像升级
----------------------------------------

仅首次部署，或 nginx/publisher/stats 代码、配置发生变化时执行：

  bash kylinlist-deploy/package-images.sh <服务端镜像版本>

该脚本只构建/导出 kylinlist-http、publisher、stats 三个服务端镜像，
不读取 CMake 客户端版本，不生成 incoming。输出：

  images/kylinlist-http-<镜像版本>.tar
  images/kylinlist-publisher-<镜像版本>.tar
  images/kylinlist-stats-<镜像版本>.tar
  images/SHA256SUMS

首次部署：
1) 准备 CI release（见第一节），并生成服务端镜像包。
2) 把 kylinlist-deploy 复制到生产机，例如 /opt/kylinlist/。
   从开发工作区复制时排除本机运行数据与配置，避免把演练数据带上生产：

   rsync -a --exclude 'data/' --exclude '.env.prod' --exclude '.publisher.lock' \
     kylinlist-deploy/ <用户>@<生产机>:/opt/kylinlist/

   生产机先建好 data 子目录并交给部署用户：

   sudo mkdir -p /opt/kylinlist/data/{incoming,public,branding/site,stats,logs}
   sudo chown -R "$USER":"$USER" /opt/kylinlist

   再把已打包的 release 目录单独投递到 data/incoming/（见第一节第 3 步）。
3) 导入镜像并核对：

   cd /opt/kylinlist
   (cd images && sha256sum -c SHA256SUMS)
   docker load -i images/kylinlist-http-<镜像版本>.tar
   docker load -i images/kylinlist-publisher-<镜像版本>.tar
   docker load -i images/kylinlist-stats-<镜像版本>.tar
   (cd data/incoming/kylinlist-<客户端版本> && sha256sum -c SHA256SUMS)

4) 配置并启动：

   cp -n .env.example .env.prod
   编辑 .env.prod：KYLINLIST_IMAGE_VERSION=<镜像版本>、HTTP_PORT=18080。
   若修改回源端口，也同步修改 host_nginx.conf 的 proxy_pass。
   docker compose --env-file .env.prod -f docker-compose-prod.yml up -d
   curl http://127.0.0.1:18080/healthz
   curl http://127.0.0.1:18080/api/v1/stats/healthz

   publisher 随 Compose 常驻；首次启动时自动消费已就绪的 incoming。
   生产容器只绑定 127.0.0.1:18080，不对外直接开放。

5) 接入宿主 nginx（真实客户端 IP 由此写入）：

   sudo cp host_nginx.conf /etc/nginx/conf.d/kylinlist.conf
   sudo nginx -t && sudo systemctl reload nginx
   curl http://127.0.0.1:11924/healthz

   生产防火墙只允许可信内网访问 TCP 11924。

服务端镜像升级：只复制新 images 和本次变更的 Compose/nginx 配置，
保留生产 data/public、data/stats、data/branding、data/logs 与 .env.prod；
导入新 tar，修改 .env.prod 的镜像版本后 up -d。不需要重新投递客户端安装包。

----------------------------------------
三、显式回滚与恢复
----------------------------------------

自动 watch 是“一次性消费”，不是永远追随 incoming 最高版本。
已成功消费的版本在回滚后不会被自动升回。

  cd /opt/kylinlist
  docker compose --env-file .env.prod -f docker-compose-prod.yml run --rm publisher status
  docker compose --env-file .env.prod -f docker-compose-prod.yml run --rm publisher rollback --to 1.3.0

手工 publish 仍作为故障恢复命令保留，但不是日常流程。publish/rollback/watch
共用 data/.publisher.lock，避免并发破坏 stable 或 history。

回滚更新通道不会回滚客户端程序或统计数据。镜像回滚则保留旧 tar，
将 .env.prod 指回旧镜像版本后 up -d。历史安装包目录不要手工删除。

----------------------------------------
四、统计、品牌与日志
----------------------------------------

  GET  /api/v1/stats/summary       聚合统计，不返回 IP 明细
  POST /api/v1/stats/usage         客户端每小时聚合上报，body 最大 8KiB
  /s/                              管理页，无鉴权，仅限可信内网管理员

客户端“使用统计”窗口只显示本机累计数据，不含全局统计、下载量与上报状态。
网站品牌覆盖在 data/branding/site/，编辑 config.json 和 Logo 后刷新生效，
无需重建镜像；客户端、开始菜单和桌面入口的品牌覆盖见源码 docs/branding.md。

下载量来自 nginx 实际 GET/200 安装包日志，按 request_id 去重。日志轮转：

  bash rotate-logs.sh

脚本先触发统计导入，再轮转并通知 nginx，90 天后清理旧日志。
所有业务持久化都在 data/ 的宿主目录中；升级时不要盲目覆盖生产 data。

常用维护：

  docker compose --env-file .env.prod -f docker-compose-prod.yml ps
  docker compose --env-file .env.prod -f docker-compose-prod.yml logs -f http stats publisher
  docker compose --env-file .env.prod -f docker-compose-prod.yml down

tar、.env.prod、incoming、public、stats、日志、品牌素材与校验文件均不入 Git。
真实生产宿主和麒麟/飞腾真机验收由用户完成，不能以开发机演练替代。

----------------------------------------
五、目录结构与备份
----------------------------------------

生产目录（示例 /opt/kylinlist/，内容即复制后的 kylinlist-deploy）：

  docker-compose-prod.yml  编排；所有持久化都是 ./data 下的宿主 bind mount
  .env.prod                端口与镜像版本（不入 Git）
  host_nginx.conf          宿主 nginx 反代配置
  images/                  服务端镜像 tar 与 SHA256SUMS（可由源码重新生成）
  data/                    全部运行数据，备份重点：
    incoming/              待发布与已消费的 CI release（回滚需要）
    public/                对外清单与历史安装包
    stats/                 统计 SQLite
    branding/site/         网站品牌覆盖
    logs/                  nginx 访问/下载日志
  tests/                   发布工具回归测试，仅开发机使用

备份：在低写入窗口内整体打包 data/ 与 .env.prod，即可完整恢复业务状态。
镜像 tar 与 incoming 都可由 CI release 重新生成，但历史 incoming 建议保留，
因为显式回滚依赖其中的安装包。kylinlist-deploy 之外没有需要单独备份的运行数据。
