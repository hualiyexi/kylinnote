#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""KylinList 合成演示数据驱动脚本（仅标准库）。

用法（详见 docs/testing-demo-data.md）：

    python tools/generate_test_data.py --app <kylinlist 可执行文件> --seed   --data-dir <绝对路径>
    python tools/generate_test_data.py --app <kylinlist 可执行文件> --verify --data-dir <绝对路径>
    python tools/generate_test_data.py --app <kylinlist 可执行文件> --reset  --data-dir <绝对路径>
    python tools/generate_test_data.py --app <kylinlist 可执行文件> --launch --data-dir <绝对路径>

安全规则：
- 只接受绝对 --data-dir；拒绝文件系统根、符号链接/junction 组件与 realpath 解析不一致的路径。
- --seed 只写入全新或完全为空的目录；任何失败都不会触碰真实用户数据。
- --reset 必须先通过 demo-manifest.json 的所有权校验和原生 --verify-demo（同时证明目录未被锁），
  然后把旧目录“重命名”为带时间戳的同级隔离目录；绝不递归删除任何用户路径。
- --launch 要求有效 manifest；净化子进程的 KYLINLIST_*/QT_* 环境变量，仅显式传 --data-dir，
  并在启动前写入隔离设置（关闭自动更新检查与更新源）。

退出码：0 成功；1 操作失败；2 用法错误；3 数据目录被其他实例锁定。
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

MANIFEST_NAME = "demo-manifest.json"
MANIFEST_FORMAT = "kylinlist.demo-manifest"
MANIFEST_VERSION = 1
OWNERSHIP_MARKER = "kylinlist-synthetic-demo"
EXIT_OK = 0
EXIT_FAILURE = 1
EXIT_USAGE = 2
EXIT_LOCKED = 3

FILE_ATTRIBUTE_REPARSE_POINT = 0x400


def fail(message, code=EXIT_FAILURE):
    print("错误：%s" % message, file=sys.stderr)
    return code


def is_windows():
    return os.name == "nt"


def is_reparse_or_link(path):
    """符号链接；Windows 上还包括 junction 等 reparse point。"""
    if os.path.islink(str(path)):
        return True
    if is_windows():
        try:
            st = os.lstat(str(path))
            return bool(st.st_file_attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        except (OSError, AttributeError):
            return False
    return False


def check_data_dir(raw, *, allow_missing):
    """返回规范化的绝对 Path；拒绝根/链接/解析不一致。allow_missing 允许尚不存在（seed 用）。"""
    if not raw:
        return None, "缺少 --data-dir"
    # 必须显式绝对路径：绝不把相对路径按当前目录悄悄放大。
    if not os.path.isabs(os.fspath(raw)):
        return None, "--data-dir 必须是绝对路径：%s" % raw
    candidate = Path(os.path.abspath(os.fspath(raw)))
    if candidate.parent == candidate:
        return None, "拒绝把文件系统根目录当作演示数据目录：%s" % candidate
    if candidate.exists() and not candidate.is_dir():
        return None, "--data-dir 指向的是文件而不是目录：%s" % candidate
    if not allow_missing and not candidate.is_dir():
        return None, "演示数据目录不存在：%s" % candidate
    # realpath 必须与规范化路径一致：任何符号链接/junction 组件都会让两者不同。
    resolved = Path(os.path.realpath(str(candidate)))
    if os.path.normcase(str(resolved)) != os.path.normcase(str(candidate)):
        return None, "路径包含符号链接/联接点或解析后不一致：%s -> %s" % (candidate, resolved)
    # 已存在的组件逐级复查（realpath 已覆盖大多数情况，这里防御 lstat 可见的 reparse point）。
    probe = candidate
    while probe.parent != probe:
        if probe.exists() and is_reparse_or_link(probe):
            return None, "路径组件是符号链接/联接点：%s" % probe
        probe = probe.parent
    return candidate, None


def check_app(raw):
    if not raw:
        return None, "缺少 --app <kylinlist 可执行文件>"
    app = Path(os.path.abspath(os.fspath(raw)))
    if not app.is_file():
        return None, "--app 不是文件：%s" % app
    return app, None


def read_manifest(data_dir):
    manifest_path = data_dir / MANIFEST_NAME
    try:
        with open(str(manifest_path), "r", encoding="utf-8") as handle:
            manifest = json.load(handle)
    except (OSError, ValueError):
        return None, "无法读取 %s（目录不是演示数据）" % manifest_path
    if not isinstance(manifest, dict):
        return None, "%s 不是 JSON 对象，拒绝作为演示数据" % manifest_path
    ownership = manifest.get("ownership") or {}
    if (manifest.get("format") != MANIFEST_FORMAT
            or manifest.get("version") != MANIFEST_VERSION
            or ownership.get("marker") != OWNERSHIP_MARKER
            or ownership.get("owned") is not True
            or ownership.get("canReset") is not True):
        return None, "%s 所有权字段无效，不能确认为本工具生成的演示数据" % manifest_path
    return manifest, None


def run_native(app, data_dir, mode):
    """调用原生 --seed-demo/--verify-demo；返回 (returncode, stdout, stderr)。
    子进程环境一律净化（不带 KYLINLIST_*/QT_*），与 --launch 相同口径。"""
    command = [str(app), mode, "--data-dir", str(data_dir)]
    try:
        completed = subprocess.run(command, capture_output=True, text=True,
                                   encoding="mbcs" if is_windows() else "utf-8",
                                   errors="replace", timeout=600,
                                   env=sanitized_environment())
        return completed.returncode, completed.stdout or "", completed.stderr or ""
    except subprocess.TimeoutExpired:
        return EXIT_FAILURE, "", "原生 %s 超时" % mode
    except OSError as error:
        return EXIT_FAILURE, "", "无法启动 %s：%s" % (app, error)


def print_native_output(stdout, stderr):
    for stream, label in ((stdout, "stdout"), (stderr, "stderr")):
        text = (stream or "").strip()
        if text:
            print("[native %s]\n%s" % (label, text))


def do_seed(app, data_dir):
    # 只允许全新或完全为空的目录；realpath 一致性在 check_data_dir 里完成。
    # 崩溃残留/外部持有的锁文件不算数据（活锁由原生端拒绝，死锁会被清理后重建）。
    existing = []
    if data_dir.exists():
        existing = [entry for entry in data_dir.iterdir() if entry.name != ".kylinlist.lock"]
    if existing:
        return fail("seed 目标目录必须为空：%s（旧目录请用 --reset 隔离后重建）" % data_dir, EXIT_FAILURE)
    code, stdout, stderr = run_native(app, data_dir, "--seed-demo")
    print_native_output(stdout, stderr)
    if code == EXIT_LOCKED:
        return fail("数据目录被其他实例锁定：%s" % data_dir, EXIT_LOCKED)
    if code != EXIT_OK:
        return fail("原生 --seed-demo 失败（退出码 %d）；目录未被确认为演示数据" % code, EXIT_FAILURE)
    manifest, error = read_manifest(data_dir)
    if manifest is None:
        return fail("seed 后缺少有效 manifest：%s" % error, EXIT_FAILURE)
    counts = manifest.get("counts", {})
    print("seed 完成：%s" % data_dir)
    print("  实体 %s（任务 %s / 笔记 %s），清单 %s，附件 %s，事件 %s"
          % (counts.get("entities"), counts.get("tasks"), counts.get("notes"),
             counts.get("lists"), counts.get("attachments"), counts.get("events")))
    return EXIT_OK


def do_verify(app, data_dir):
    code, stdout, stderr = run_native(app, data_dir, "--verify-demo")
    # 只使用本次进程返回的报告；旧文件不参与本次结果展示。
    print_native_output(stdout, stderr)
    if code == EXIT_LOCKED:
        return fail("数据目录被其他实例锁定：%s" % data_dir, EXIT_LOCKED)
    if code != EXIT_OK:
        return fail("原生 --verify-demo 失败（退出码 %d）" % code, EXIT_FAILURE)
    print("verify 通过：%s" % data_dir)
    return EXIT_OK


def do_reset(app, data_dir):
    manifest, error = read_manifest(data_dir)
    if manifest is None:
        return fail("--reset 前提不满足：%s" % error, EXIT_FAILURE)
    code, stdout, stderr = run_native(app, data_dir, "--verify-demo")
    print_native_output(stdout, stderr)
    if code == EXIT_LOCKED:
        return fail("数据目录被其他实例锁定，拒绝 reset：%s" % data_dir, EXIT_LOCKED)
    if code != EXIT_OK:
        return fail("原生验证未通过（退出码 %d），拒绝 reset" % code, EXIT_FAILURE)
    # 验证成功即证明锁已释放；把旧目录重命名为同级时间戳隔离目录（绝不删除），
    # 然后原位重新生成一份新的演示数据。时间戳加 pid 防止同秒冲突。
    stamp = "%s-%d" % (time.strftime("%Y%m%d-%H%M%S"), os.getpid())
    quarantine = data_dir.parent / ("%s.demo-quarantine-%s" % (data_dir.name, stamp))
    if quarantine.exists():
        return fail("隔离目录已存在：%s" % quarantine, EXIT_FAILURE)
    try:
        os.rename(str(data_dir), str(quarantine))
    except OSError as error:
        return fail("重命名失败（目录可能仍被占用）：%s" % error, EXIT_FAILURE)
    print("旧数据已隔离：%s（未删除任何数据，可手动清理）" % quarantine)
    seed_code = do_seed(app, data_dir)
    if seed_code != EXIT_OK:
        return fail("reset 隔离成功但重新生成失败；旧数据仍保留在 %s" % quarantine, EXIT_FAILURE)
    print("reset 完成：%s（已原位重建）" % data_dir)
    return EXIT_OK


def sanitized_environment():
    environment = dict(os.environ)
    for key in list(environment):
        if key.upper().startswith("KYLINLIST_") or key.upper().startswith("QT_"):
            del environment[key]
    return environment


def write_isolated_settings(data_dir):
    """main.cpp 会让 QSettings 落在 <data-dir>/settings/kylinlist/kylinlist.ini；
    预先写入关闭自动更新的键（updates/autoCheck=false 且显式空 baseUrl）。"""
    settings_dir = data_dir / "settings" / "kylinlist"
    settings_dir.mkdir(parents=True, exist_ok=True)
    settings_path = settings_dir / "kylinlist.ini"
    with open(str(settings_path), "w", encoding="utf-8", newline="\n") as handle:
        handle.write("[updates]\n")
        handle.write("autoCheck=false\n")
        handle.write("baseUrl=\n")
    return settings_path


def do_launch(app, data_dir):
    manifest, error = read_manifest(data_dir)
    if manifest is None:
        return fail("--launch 前提不满足：%s" % error, EXIT_FAILURE)
    settings_path = write_isolated_settings(data_dir)
    environment = sanitized_environment()
    command = [str(app), "--data-dir", str(data_dir)]
    try:
        process = subprocess.Popen(command, env=environment, close_fds=True)
    except OSError as error:
        return fail("无法启动：%s" % error, EXIT_FAILURE)
    print("launch：pid=%s" % process.pid)
    print("  命令：%s" % " ".join(command))
    print("  隔离设置：%s（自动更新已关闭）" % settings_path)
    return EXIT_OK


def main(argv):
    # Windows 控制台可能是 GBK 等编码：统一按 UTF-8 输出并容错，避免打印原生输出时崩溃。
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError):
            pass
    parser = argparse.ArgumentParser(
        prog="generate_test_data.py",
        description="KylinList 合成演示数据驱动：生成/校验/隔离重置/隔离启动（仅标准库，安全优先）",
        epilog="示例：\n"
               "  python tools/generate_test_data.py --app build/bin/kylinlist.exe "
               "--seed --data-dir D:/demo-data\n"
               "  python tools/generate_test_data.py --app build/bin/kylinlist "
               "--verify --data-dir /tmp/demo-data\n",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", help="kylinlist 可执行文件的路径（必需）")
    parser.add_argument("--data-dir", help="演示数据目录（必须是绝对路径，必需）")
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--seed", action="store_true", help="在全新空目录生成合成演示数据")
    actions.add_argument("--verify", action="store_true", help="只执行原生校验")
    actions.add_argument("--reset", action="store_true",
                         help="验证所有权并原生校验通过后，隔离旧目录（改名，绝不删除）并原位重新生成")
    actions.add_argument("--launch", action="store_true", help="净化环境后用该目录启动应用")
    args = parser.parse_args(argv)

    if not argv:
        parser.print_help()
        return EXIT_USAGE
    if not any((args.seed, args.verify, args.reset, args.launch)):
        parser.print_help()
        return fail("必须且只能选择 --seed/--verify/--reset/--launch 之一", EXIT_USAGE)

    app, error = check_app(args.app)
    if app is None:
        return fail(error, EXIT_USAGE)

    data_dir, error = check_data_dir(args.data_dir, allow_missing=args.seed)
    if data_dir is None:
        return fail(error, EXIT_USAGE)

    if args.seed:
        return do_seed(app, data_dir)
    if args.verify:
        return do_verify(app, data_dir)
    if args.reset:
        return do_reset(app, data_dir)
    return do_launch(app, data_dir)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
