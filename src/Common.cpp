#include "Common.h"

namespace kl {

QString layerLabel(NoteLayer layer)
{
    switch (layer) {
    case NoteLayer::OnDesktop: return QStringLiteral("贴在桌面");
    case NoteLayer::Normal: return QStringLiteral("普通窗口");
    case NoteLayer::OnTop: return QStringLiteral("始终置顶");
    }
    return QStringLiteral("贴在桌面");
}

} // namespace kl
