# server/data/ 目录说明

本目录存放发布服务的运行数据，**不纳入版本库**（根 `.gitignore` 中的 `server/data/` 已忽略）。
目录结构由 `tools/publish.py` 自动创建和维护，不要手工改动已发布内容：

```
data/
  public/                          # nginx 站点根（http 服务只读挂载）
    updates/
      v1/
        stable/manifest.json       # 当前 stable 版本清单（发布时原子替换）
        archive/                   # 历史清单归档 manifest-<版本>-<UTC时间戳>.json
                                    # 以及透传归档的 release-info-<版本>-<时间戳>.json
      files/<版本>/<platform>-<arch>-<variant>/<文件名>   # 安装包，按版本不可变
  incoming/                        # （可选约定）存放待发布 release 目录，自行创建
```

首次部署 http 服务前，先创建站点根目录：

```
mkdir -p data/public
```

注意：

- 不要手工向 `data/public` 放置或删除文件；一切变更通过 `docker compose run --rm publisher ...` 完成。
- `files/` 下的历史版本目录是回退的前提，删除后对应版本将无法回退。
