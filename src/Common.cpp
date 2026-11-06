#include "Common.h"

#include <QTextDocument>

namespace kn {

QString deriveTitle(const QString &html)
{
    QTextDocument doc;
    doc.setHtml(html);
    const QString plain = doc.toPlainText();

    for (const QString &line : plain.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            // 标题只保留前 60 个字符，避免超长行撑爆数据库字段展示
            return trimmed.left(60);
        }
    }
    // 必须返回空串而非 null QString：null 会被 QSqlQuery 绑成 SQL NULL，
    // 撞上 notes.title 的 NOT NULL 约束（DEFAULT '' 只在省略该列时生效）
    return QStringLiteral("");
}

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
