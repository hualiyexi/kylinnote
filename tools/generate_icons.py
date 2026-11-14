"""把本机的图标集生成 C++ 数据表。

输入：图标源目录 滴答清单前端图片/icons/single/*.svg（本地，不入库）
输出：src/ui/icons/IconData.gen.h / .cpp（入库，供干净 clone 的 CI 构建）

只读取本地 SVG 文本做字段拆分，不做任何图形转换。
"""

import glob
import os
import re
import sys

# 品牌标识不接入。
EXCLUDED = {"logo", "connect-logo"}

ROOT_RE = re.compile(r"^<svg\b([^>]*)>", re.S)
ATTR_RE = re.compile(r'([a-zA-Z:-]+)\s*=\s*"([^"]*)"')
HEX_COLOR_RE = re.compile(r'#[0-9A-Fa-f]{3,8}\b')


def parse_svg(path):
    text = open(path, "r", encoding="utf-8").read()
    m = ROOT_RE.match(text)
    if not m:
        raise ValueError("no <svg> root: %s" % path)
    attrs = dict(ATTR_RE.findall(m.group(1)))
    body = text[m.end():text.rindex("</svg>")].strip()

    view_box = attrs.pop("viewBox", "")
    attrs.pop("xmlns", None)
    tinted_root = attrs.get("fill") == "currentColor"
    attrs.pop("fill", None) if tinted_root else None

    # 单色判定：根标签没有别的绘制属性，正文里也没有硬编码颜色或渐变。
    mono = (tinted_root or "fill" not in attrs) and "stroke" not in attrs
    if HEX_COLOR_RE.search(body) or "Gradient" in body:
        mono = False

    root_attrs = " ".join('%s="%s"' % (k, v) for k, v in sorted(attrs.items()))
    return {
        "name": os.path.basename(path)[:-4],
        "viewBox": view_box,
        "rootAttrs": root_attrs,
        "body": body,
        "mono": mono,
    }


def emit_header(path):
    open(path, "w", encoding="utf-8", newline="\n").write(
        """// 由 tools/generate_icons.py 生成，不要手工编辑。
#pragma once

namespace kl {
namespace icons {

// 一条图标记录。body 是图形内部标记，viewBox/rootAttrs 取自原始根标签。
struct IconEntry {
    const char* id;
    const char* viewBox;
    const char* rootAttrs;
    const char* body;
    bool mono;  // true 表示可用主题色覆盖 fill
};

// 全部条目（按 id 升序）；count 可为空。
const IconEntry* entries(int* count);
// 按 id 精确查找，找不到返回 nullptr。
const IconEntry* find(const char* id);

}  // namespace icons
}  // namespace kl
"""
    )


def main():
    if len(sys.argv) != 3:
        print("usage: generate_icons.py <icon-dir> <output-dir>", file=sys.stderr)
        return 2
    icon_dir, out_dir = sys.argv[1], sys.argv[2]
    files = sorted(glob.glob(os.path.join(icon_dir, "*.svg")))
    if not files:
        print("error: no *.svg under %s（滴答图标资源只在本机，未入库）" % icon_dir, file=sys.stderr)
        return 1

    icons = [parse_svg(f) for f in files]
    icons = [i for i in icons if i["name"] not in EXCLUDED]
    icons.sort(key=lambda i: i["name"])

    for icon in icons:
        if ")icon\"" in icon["body"]:
            raise ValueError("raw string delimiter clash: %s" % icon["name"])

    os.makedirs(out_dir, exist_ok=True)
    emit_header(os.path.join(out_dir, "IconData.gen.h"))

    lines = [
        "// 由 tools/generate_icons.py 生成，不要手工编辑。",
        '#include "ui/icons/IconData.gen.h"',
        "",
        "#include <cstring>",
        "",
        "namespace kl {",
        "namespace icons {",
        "namespace {",
        "",
        "const IconEntry kEntries[] = {",
    ]
    for icon in icons:
        lines.append(
            '    {"%s", "%s", "%s", R"icon(%s)icon", %s},'
            % (
                icon["name"],
                icon["viewBox"],
                icon["rootAttrs"].replace('"', '\\"'),
                icon["body"],
                "true" if icon["mono"] else "false",
            )
        )
    lines += [
        "};",
        "",
        "const int kCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));",
        "",
        "}  // namespace",
        "",
        "const IconEntry* entries(int* count) {",
        "    if (count) *count = kCount;",
        "    return kEntries;",
        "}",
        "",
        "const IconEntry* find(const char* id) {",
        "    if (!id) return nullptr;",
        "    int lo = 0;",
        "    int hi = kCount - 1;",
        "    while (lo <= hi) {",
        "        const int mid = lo + (hi - lo) / 2;",
        "        const int cmp = std::strcmp(kEntries[mid].id, id);",
        "        if (cmp == 0) return &kEntries[mid];",
        "        if (cmp < 0) lo = mid + 1;",
        "        else hi = mid - 1;",
        "    }",
        "    return nullptr;",
        "}",
        "",
        "}  // namespace icons",
        "}  // namespace kl",
        "",
    ]
    cpp_path = os.path.join(out_dir, "IconData.gen.cpp")
    open(cpp_path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))

    mono = sum(1 for i in icons if i["mono"])
    print("icons: %d (mono %d, multi %d)" % (len(icons), mono, len(icons) - mono))
    print("excluded: %s" % ", ".join(sorted(EXCLUDED)))
    print("bytes: %d" % os.path.getsize(cpp_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
