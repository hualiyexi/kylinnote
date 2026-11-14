# 部署运行数据

所有业务持久化都使用本目录的宿主 bind mount，不使用 Docker named/anonymous volume。

- `incoming/kylinlist-<版本>/`：由 `kylinlist-deploy/package.sh` 从完整 GitHub CI release 生成，
  包含安装包、manifest、RELEASE-INFO、逐文件 `SHA256SUMS` 和最后生成的 `READY`。
  同版本目录拒绝覆盖。日常只更新这里，不需要更新服务端镜像。
- `incoming/.kylinlist-<版本>.uploading/`：复制到生产机时使用的隐藏暂存目录。
  完整复制后在同一文件系统原子改名为正式目录；publisher 不读取暂存目录。
- `incoming/*/.publish-state.json`：watch 的处理状态。succeeded/failed/skipped
  不会重复消费；修复失败输入时移走旧失败目录并重新投递完整目录。
- `public/`：publisher 自动安装的当前及历史版本文件与公告归档，升级时保留；
  不要手工修改或把安装包直接放进 public。
- `stats/`：统计 SQLite；同一 IP 只保留最新使用快照，下载按 request_id 去重。
- `logs/`：nginx 访问/下载日志；轮转脚本见 `kylinlist-deploy/rotate-logs.sh`。
- `branding/site/`：网站品牌覆盖（`config.json` + Logo 文件），只读挂载为容器内
  `/usr/share/nginx/html/local`；修改后刷新生效，首页与 `/s/` 都生效。
- `.publisher.lock`：自动 watch 与手工 publish/rollback 共用的跨进程锁文件。

`kylinlist-deploy/package-images.sh` 只负责 `../images/` 中的服务端镜像，与这里的客户端版本无关。
这些内容均不纳入 Git；升级时保留生产 `public/`、`stats/`、`logs/` 和 `branding/`。
