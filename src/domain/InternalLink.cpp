#include "InternalLink.h"

#include "../Common.h"
#include <QSet>
#include <QUrl>
#include <algorithm>

namespace kl {
namespace {
bool markdownPunctuation(QChar ch)
{
    const ushort c = ch.unicode();
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@')
        || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

bool wikiEscape(QChar ch)
{
    return ch == QLatin1Char('\\') || ch == QLatin1Char('[')
        || ch == QLatin1Char(']') || ch == QLatin1Char('|');
}

QString unescape(const QString& text, bool wiki)
{
    QString result;
    result.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\\') && i + 1 < text.size()
            && (wiki ? wikiEscape(text.at(i + 1)) : markdownPunctuation(text.at(i + 1)))) ++i;
        result += text.at(i);
    }
    return result;
}

QString escapeLabel(const QString& text, bool wiki)
{
    QString result;
    result.reserve(text.size());
    for (const QChar ch : text) {
        if (wiki ? wikiEscape(ch) : markdownPunctuation(ch)) result += QLatin1Char('\\');
        result += ch;
    }
    return result;
}

// Strip quote containers, but retain the indentation within the container.
int lineContent(const QString& body, int start, int end, int* quotes, int* indent)
{
    int p = start;
    *quotes = 0;
    for (;;) {
        int marker = p;
        while (marker < end && marker - p < 3 && body.at(marker) == QLatin1Char(' ')) ++marker;
        if (marker >= end || body.at(marker) != QLatin1Char('>')) break;
        ++*quotes;
        p = marker + 1;
        if (p < end && (body.at(p) == QLatin1Char(' ') || body.at(p) == QLatin1Char('\t'))) ++p;
    }
    *indent = 0;
    while (p < end) {
        if (body.at(p) == QLatin1Char(' ')) ++*indent;
        else if (body.at(p) == QLatin1Char('\t')) *indent += 4 - *indent % 4;
        else break;
        ++p;
    }
    return p;
}

int afterListMarker(const QString& body, int start, int end)
{
    if (start >= end) return -1;
    int p = start;
    const QChar ch = body.at(p);
    if (ch == QLatin1Char('-') || ch == QLatin1Char('+') || ch == QLatin1Char('*')) {
        ++p;
    } else {
        while (p < end && p - start < 9 && body.at(p) >= QLatin1Char('0')
               && body.at(p) <= QLatin1Char('9')) ++p;
        if (p == start || p >= end || (body.at(p) != QLatin1Char('.') && body.at(p) != QLatin1Char(')'))) return -1;
        ++p;
    }
    if (p >= end || (body.at(p) != QLatin1Char(' ') && body.at(p) != QLatin1Char('\t'))) return -1;
    return p + 1;
}

QVector<bool> blockCode(const QString& body)
{
    QVector<bool> code(body.size(), false);
    QChar fence;
    int fenceLength = 0, fenceQuotes = 0, fenceListIndent = 0;
    int listIndent = 0, listQuotes = 0;
    bool previousBlank = true, indented = false;
    for (int start = 0; start < body.size();) {
        const int newline = body.indexOf(QLatin1Char('\n'), start);
        const int next = newline < 0 ? body.size() : newline + 1;
        const int end = newline < 0 ? body.size() : newline;
        int quotes = 0, indent = 0;
        const int content = lineContent(body, start, end, &quotes, &indent);
        const bool blank = body.mid(content, end - content).trimmed().isEmpty();
        if (!fence.isNull() && (quotes < fenceQuotes
            || (!blank && fenceListIndent > 0 && indent < fenceListIndent))) fence = QChar();
        if (quotes != listQuotes || (!blank && indent < listIndent && fence.isNull())) listIndent = 0;
        int relativeIndent = listIndent > 0 ? qMax(0, indent - listIndent) : indent;
        int markerEnd = relativeIndent <= 3 ? afterListMarker(body, content, end) : -1;
        int fenceStart = content;
        if (markerEnd >= 0 && fence.isNull()) {
            listIndent = indent + markerEnd - content;
            listQuotes = quotes;
            fenceStart = markerEnd;
            relativeIndent = 0;
            while (fenceStart < end && body.at(fenceStart) == QLatin1Char(' ')) {
                ++relativeIndent;
                ++fenceStart;
            }
        }
        const QChar candidate = fenceStart < end ? body.at(fenceStart) : QChar();
        int runEnd = fenceStart;
        if (candidate == QLatin1Char('`') || candidate == QLatin1Char('~'))
            while (runEnd < end && body.at(runEnd) == candidate) ++runEnd;
        const int run = runEnd - fenceStart;
        bool protect = false;
        if (!fence.isNull()) {
            protect = true;
            if (quotes == fenceQuotes && relativeIndent <= 3 && candidate == fence && run >= fenceLength
                && body.mid(runEnd, end - runEnd).trimmed().isEmpty()) fence = QChar();
            indented = false;
        } else if (relativeIndent <= 3 && run >= 3
                   && (candidate != QLatin1Char('`') || !body.mid(runEnd, end - runEnd).contains(QLatin1Char('`')))) {
            fence = candidate;
            fenceLength = run;
            fenceQuotes = quotes;
            fenceListIndent = listIndent;
            protect = true;
            indented = false;
        } else if (!blank) {
            indented = markerEnd < 0 && relativeIndent >= 4 && (previousBlank || indented);
            protect = indented;
        }
        if (protect) std::fill(code.begin() + start, code.begin() + next, true);
        previousBlank = blank;
        start = next;
    }
    return code;
}

bool blankLineAfter(const QString& body, int newline)
{
    int p = newline + 1;
    while (p < body.size() && (body.at(p) == QLatin1Char(' ') || body.at(p) == QLatin1Char('\t')
                              || body.at(p) == QLatin1Char('\r'))) ++p;
    return p < body.size() && body.at(p) == QLatin1Char('\n');
}

int codeSpanEnd(const QString& body, int start, const QVector<bool>& code)
{
    int after = start;
    while (after < body.size() && body.at(after) == QLatin1Char('`')) ++after;
    const int count = after - start;
    for (int p = after; p < body.size();) {
        if (code.at(p) || (body.at(p) == QLatin1Char('\n') && blankLineAfter(body, p))) return -1;
        if (body.at(p) != QLatin1Char('`')) { ++p; continue; }
        int end = p;
        while (end < body.size() && body.at(end) == QLatin1Char('`')) ++end;
        if (end - p == count) return end;
        p = end;
    }
    return -1; // An unmatched backtick run is ordinary text, not a code span.
}

int labelEnd(const QString& body, int start, const QVector<bool>& code)
{
    int depth = 1;
    for (int p = start + 1; p < body.size(); ++p) {
        if (code.at(p) || (body.at(p) == QLatin1Char('\n') && blankLineAfter(body, p))) return -1;
        const QChar ch = body.at(p);
        if (ch == QLatin1Char('\\') && p + 1 < body.size() && markdownPunctuation(body.at(p + 1))) { ++p; continue; }
        if (ch == QLatin1Char('`')) {
            const int end = codeSpanEnd(body, p, code);
            if (end >= 0) { p = end - 1; continue; }
        }
        if (ch == QLatin1Char('[')) ++depth;
        else if (ch == QLatin1Char(']') && --depth == 0) return p;
    }
    return -1;
}

bool skipLinkSpace(const QString& body, int* p, const QVector<bool>& code)
{
    int newlines = 0;
    while (*p < body.size() && body.at(*p).isSpace()) {
        if (code.at(*p)) return false;
        if (body.at(*p) == QLatin1Char('\n')
            || (body.at(*p) == QLatin1Char('\r') && (*p + 1 == body.size() || body.at(*p + 1) != QLatin1Char('\n'))))
            if (++newlines > 1) return false;
        ++*p;
    }
    return *p < body.size() && !code.at(*p);
}

bool inlineLink(const QString& body, int start, const QVector<bool>& code, MarkdownLink* link)
{
    const int close = labelEnd(body, start, code);
    if (close < 0 || close + 1 >= body.size() || body.at(close + 1) != QLatin1Char('(')) return false;
    int p = close + 2;
    if (!skipLinkSpace(body, &p, code)) return false;
    int targetStart = p, targetEnd = p;
    if (body.at(p) == QLatin1Char('<')) {
        targetStart = ++p;
        while (p < body.size() && body.at(p) != QLatin1Char('>')) {
            if (code.at(p) || body.at(p) == QLatin1Char('\n') || body.at(p) == QLatin1Char('\r')
                || body.at(p) == QLatin1Char('<')) return false;
            if (body.at(p) == QLatin1Char('\\') && p + 1 < body.size() && markdownPunctuation(body.at(p + 1))) ++p;
            ++p;
        }
        if (p == body.size()) return false;
        targetEnd = p++;
    } else {
        int depth = 0;
        while (p < body.size()) {
            const QChar ch = body.at(p);
            if (code.at(p) || ch == QLatin1Char('<')) return false;
            if (ch == QLatin1Char('\\') && p + 1 < body.size() && markdownPunctuation(body.at(p + 1))) {
                p += 2;
                continue;
            }
            if (ch.isSpace() || (ch == QLatin1Char(')') && depth == 0)) break;
            if (ch == QLatin1Char('(')) { if (++depth > 32) return false; }
            else if (ch == QLatin1Char(')')) --depth;
            ++p;
        }
        if (depth != 0) return false;
        targetEnd = p;
    }
    const int beforeSpace = p;
    if (!skipLinkSpace(body, &p, code)) return false;
    if (body.at(p) != QLatin1Char(')')) {
        if (p == beforeSpace) return false;
        const QChar opener = body.at(p++);
        if (opener != QLatin1Char('"') && opener != QLatin1Char('\'') && opener != QLatin1Char('(')) return false;
        const QChar closer = opener == QLatin1Char('(') ? QLatin1Char(')') : opener;
        while (p < body.size() && body.at(p) != closer) {
            if (code.at(p) || (body.at(p) == QLatin1Char('\n') && blankLineAfter(body, p))) return false;
            if (body.at(p) == QLatin1Char('\\') && p + 1 < body.size() && markdownPunctuation(body.at(p + 1))) ++p;
            ++p;
        }
        if (p == body.size()) return false;
        ++p;
        if (!skipLinkSpace(body, &p, code) || body.at(p) != QLatin1Char(')')) return false;
    }
    link->start = start;
    link->length = p + 1 - start;
    link->target = unescape(body.mid(targetStart, targetEnd - targetStart), false);
    link->labelSource = body.mid(start + 1, close - start - 1);
    link->label = unescape(link->labelSource, false);
    link->wiki = false;
    return true;
}

bool wikiLink(const QString& body, int start, const QVector<bool>& code, MarkdownLink* link)
{
    const int targetStart = start + 2, afterTarget = targetStart + 36;
    if (start + 1 >= body.size() || body.at(start + 1) != QLatin1Char('[') || afterTarget >= body.size()) return false;
    const QString id = body.mid(targetStart, 36);
    if (!isUuid(id)) return false;
    int p = afterTarget;
    QString label;
    if (body.at(p) == QLatin1Char('|')) {
        const int labelStart = ++p;
        while (p + 1 < body.size()) {
            if (code.at(p) || body.at(p) == QLatin1Char('\n') || body.at(p) == QLatin1Char('\r')) return false;
            if (body.at(p) == QLatin1Char('\\') && wikiEscape(body.at(p + 1))) { p += 2; continue; }
            if (body.at(p) == QLatin1Char(']') && body.at(p + 1) == QLatin1Char(']')) break;
            ++p;
        }
        label = unescape(body.mid(labelStart, p - labelStart), true);
    }
    if (p + 1 >= body.size() || body.at(p) != QLatin1Char(']') || body.at(p + 1) != QLatin1Char(']')) return false;
    link->start = start;
    link->length = p + 2 - start;
    link->target = id;
    link->label = label;
    link->wiki = true;
    return true;
}
} // namespace

QVector<MarkdownLink> markdownLinks(const QString& body)
{
    const QVector<bool> code = blockCode(body);
    QVector<MarkdownLink> links;
    for (int p = 0; p < body.size();) {
        if (code.at(p)) { ++p; continue; }
        const QChar ch = body.at(p);
        if (ch == QLatin1Char('\\') && p + 1 < body.size() && markdownPunctuation(body.at(p + 1))) { p += 2; continue; }
        if (ch == QLatin1Char('`')) {
            const int end = codeSpanEnd(body, p, code);
            if (end >= 0) { p = end; continue; }
            do { ++p; } while (p < body.size() && body.at(p) == QLatin1Char('`'));
            continue;
        }
        MarkdownLink link;
        if (ch == QLatin1Char('!') && p + 1 < body.size() && body.at(p + 1) == QLatin1Char('[')) {
            // Image alt text (including nested references) and ![[embeds]] are not links.
            if (wikiLink(body, p + 1, code, &link) || inlineLink(body, p + 1, code, &link)) p = link.start + link.length;
            else {
                const int close = labelEnd(body, p + 1, code);
                p = close >= 0 ? close + 1 : p + 2;
            }
            continue;
        }
        if (ch == QLatin1Char('[') && (wikiLink(body, p, code, &link) || inlineLink(body, p, code, &link))) {
            if (link.wiki || (internalLinkId(link.target).isEmpty() && isSafeLinkTarget(link.target))) links.append(link);
            p += link.length;
        } else ++p;
    }
    return links;
}

QString internalLinkId(const QString& target)
{
    return isUuid(target) ? target : QString();
}

bool isSafeLinkTarget(const QString& target)
{
    if (!internalLinkId(target).isEmpty()) return true;
    if (target.isEmpty() || target != target.trimmed()) return false;
    for (const QChar ch : target) if (ch.category() == QChar::Other_Control) return false;
    const QUrl url(target, QUrl::StrictMode);
    if (!url.isValid() || url.isRelative()) return false;
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) return !url.host().isEmpty();
    return scheme == QLatin1String("mailto") && (!url.path().isEmpty() || !url.query().isEmpty());
}

bool markdownBlockCodeAt(const QString& body, int offset)
{
    return offset >= 0 && offset < body.size() && blockCode(body).at(offset);
}

QString entityReference(const QString& entityId, const QString& label)
{
    const QString id = internalLinkId(entityId);
    if (id.isEmpty()) return QString();
    QString display = label;
    display.replace(QStringLiteral("\r\n"), QStringLiteral(" "));
    display.replace(QLatin1Char('\r'), QLatin1Char(' '));
    display.replace(QLatin1Char('\n'), QLatin1Char(' '));
    display.replace(QChar::LineSeparator, QLatin1Char(' '));
    display.replace(QChar::ParagraphSeparator, QLatin1Char(' '));
    if (display.isEmpty()) return QStringLiteral("[[%1]]").arg(id);
    return QStringLiteral("[[%1|%2]]").arg(id, escapeLabel(display, true));
}

QString markdownLink(const QString& target, const QString& label)
{
    if (!internalLinkId(target).isEmpty()) return entityReference(target, label);
    if (!isSafeLinkTarget(target)) return QString();
    QString destination;
    destination.reserve(target.size());
    for (const QChar ch : target) {
        // Parentheses must survive both Markdown's delimiter balancing and URL
        // parsing. Ampersands must not become Markdown character references.
        if (wikiEscape(ch) || ch == QLatin1Char('(') || ch == QLatin1Char(')')
            || ch == QLatin1Char('<') || ch == QLatin1Char('>') || ch == QLatin1Char('&')
            || ch == QLatin1Char('`')) destination += QLatin1Char('\\');
        destination += ch;
    }
    return QStringLiteral("[%1](%2)").arg(escapeLabel(label.isEmpty() ? target : label, false), destination);
}

bool isValidLinkTarget(const QString& target)
{
    return !internalLinkId(target).isEmpty();
}

QStringList internalTargets(const QString& body)
{
    QStringList result;
    QSet<QString> seen;
    for (const MarkdownLink& link : markdownLinks(body)) {
        const QString id = internalLinkId(link.target);
        if (!id.isEmpty() && !seen.contains(id)) { seen.insert(id); result.append(id); }
    }
    return result;
}

} // namespace kl
