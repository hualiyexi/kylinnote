#pragma once

#include <QToolButton>

namespace kl {

// 名称输入框中的统一图形入口：默认 SVG 与自定义 emoji 共享绘制和焦点几何。
class GlyphButton : public QToolButton {
public:
    explicit GlyphButton(QWidget* parent = nullptr);

    void setGlyph(const QString& glyph);
    void setDefaultIconName(const QString& name);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_glyph;
    QString m_defaultIconName = QStringLiteral("normal-list");
};

} // namespace kl
