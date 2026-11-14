# 部署运行数据

此目录在开发机由 `deploy/package.sh` 填充，在生产机由 Compose 和发布工具使用：

- `incoming/kylinlist-<版本>/`：待发布的安装包、manifest 与 RELEASE-INFO（`package.sh` 生成；同版本目录拒绝覆盖）。
- `public/`：publisher 原子安装的当前及历史版本文件，升级时保留。
- `stats/`：统计 SQLite；同一 IP 只保留最新使用快照，下载按 request_id 去重，升级时保留。
- `logs/`：nginx 访问/下载日志；轮转脚本见 `deploy/rotate-logs.sh`。
- `branding/site/`：网站品牌覆盖（`config.json` + Logo 文件），只读挂载为容器内
  `/usr/share/nginx/html/local`；修改后刷新生效，无需重建镜像，首页与 `/s/` 都生效。
  默认不存在时使用麒麟清单默认品牌；字段与规则见 `docs/branding.md`。

这些内容均不纳入 Git（根 `.gitignore` 已忽略 `deploy/data/*`，仅保留本说明）。
发布时不要手工改 `public/`，必须使用 publisher；`branding/site/` 由管理员按需填写。
