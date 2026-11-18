#ifndef KYLINLIST_INTERNALLINK_H
#define KYLINLIST_INTERNALLINK_H

#include <QString>
#include <QStringList>
#include <QVector>

namespace kl {

struct MarkdownLink {
    int start = 0;
    int length = 0;
    QString target;
    QString label;
    bool wiki = false;
    QString labelSource; // External Markdown label before unescaping, for inline styling.
};

// Ordered, non-overlapping UTF-16 source ranges for safe external inline Markdown
// links and canonical [[UUID|label]] / [[UUID]] references, excluding images, code
// and escaped markers. Targets and labels have their syntax escapes removed; an
// empty wiki label means "display the current target title".
QVector<MarkdownLink> markdownLinks(const QString& body);
// Editing hints share the parser's fenced/indented code boundaries.
bool markdownBlockCodeAt(const QString& body, int offset);

// Only bare, lowercase canonical UUIDs qualify.
QString internalLinkId(const QString& target);
bool isSafeLinkTarget(const QString& target);
QString entityReference(const QString& entityId, const QString& label = QString());
QString markdownLink(const QString& target, const QString& label);

// This validates canonical internal targets only, not web URLs.
bool isValidLinkTarget(const QString& target);
// Canonical references, deduplicated in first-source order.
QStringList internalTargets(const QString& body);

} // namespace kl

#endif // KYLINLIST_INTERNALLINK_H
