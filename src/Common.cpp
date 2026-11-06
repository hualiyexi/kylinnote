#include "Common.h"

namespace kn {

QString layerLabel(NoteLayer layer)
{
    switch (layer) {
    case NoteLayer::OnDesktop:
        return QString::fromUtf8("\u8d34\u5728\u684c\u9762");   // 贴在桌面
    case NoteLayer::Normal:
        return QString::fromUtf8("\u666e\u901a\u7a97\u53e3");   // 普通窗口
    case NoteLayer::OnTop:
        return QString::fromUtf8("\u59cb\u7ec8\u7f6e\u9876");    // 始终置顶
    }
    return QString::fromUtf8("\u8d34\u5728\u684c\u9762");
}

} // namespace kn
