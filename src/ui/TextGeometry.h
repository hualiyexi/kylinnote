#pragma once

#include <QFontMetricsF>
#include <QString>

namespace kl {

// Keep a stable baseline across labels, including punctuation-only names. The
// reference ink spans CJK, capitals and descenders in the actual resolved font.
inline qreal textContentCenterOffset(const QFontMetricsF& metrics) {
    const QRectF ink = metrics.tightBoundingRect(QStringLiteral("中Ag"));
    return ink.isEmpty() ? (metrics.descent() - metrics.ascent()) / 2.0 : ink.center().y();
}

inline qreal textBaselineForCenter(const QFontMetricsF& metrics, qreal center) {
    return center - textContentCenterOffset(metrics);
}

// Composite controls need the currently displayed ink, not the stable reference
// used by the editor gutter. CJK-only labels otherwise sit above labels with
// descenders, and changing a caption can move it by several pixels.
inline qreal textBaselineForCenter(const QFontMetricsF& metrics, qreal center,
                                   const QString& text) {
    if (text.isEmpty()) return textBaselineForCenter(metrics, center);
    const QRectF ink = metrics.tightBoundingRect(text);
    return ink.isEmpty() ? textBaselineForCenter(metrics, center) : center - ink.center().y();
}

} // namespace kl
