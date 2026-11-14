// 由 tools/generate_icons.py 生成，不要手工编辑。
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
