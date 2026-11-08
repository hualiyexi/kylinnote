#include "InternalLink.h"

#include "../Common.h"
#include <QRegularExpression>
#include <QSet>

namespace kl {

bool isValidLinkTarget(const QString &target)
{
    if (target.startsWith(QStringLiteral("kl:"))) return isUuid(target.mid(3));
    return isUuid(target);
}

QStringList internalTargets(const QString &body)
{
    QStringList result;
    QSet<QString> seen;
    const QRegularExpression pattern(QString::fromLatin1(R"((?<!!)\[(?:\\.|[^\]\n])*\]\(\s*kl:([0-9a-f-]{36})\s*\))"));
    QRegularExpressionMatchIterator matches = pattern.globalMatch(body);
    while (matches.hasNext()) {
        const QString id = matches.next().captured(1);
        if (isValidLinkTarget(id) && !seen.contains(id)) { seen.insert(id); result.append(id); }
    }
    return result;
}

} // namespace kl
