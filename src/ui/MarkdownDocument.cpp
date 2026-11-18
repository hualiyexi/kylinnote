#include "MarkdownDocument.h"

#include <QFontMetrics>
#include <QRegularExpression>
#include <QSet>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFragment>

#include "../domain/InternalLink.h"
#include "Markdown.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace MarkdownDocument {
namespace {

bool escaped(const QString& text, int position) {
    int slashes = 0;
    for (int i = position - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i) ++slashes;
    return slashes % 2 != 0;
}

int findToken(const QString& text, const QString& token, int from) {
    int position = from;
    while ((position = text.indexOf(token, position)) >= 0) {
        if (!escaped(text, position)) return position;
        position += token.size();
    }
    return -1;
}

int findCodeEnd(const QString& text, int length, int from) {
    int position = from;
    while ((position = text.indexOf(QLatin1Char('`'), position)) >= 0) {
        int count = 1;
        while (position + count < text.size() && text.at(position + count) == QLatin1Char('`')) ++count;
        if (count == length) return position;
        position += count;
    }
    return -1;
}

QTextCharFormat withStyle(QTextCharFormat format, const QString& opening, const QString& closing) {
    QStringList styles = format.property(InlineStylesProperty).toStringList();
    styles.append(opening); styles.append(closing);
    format.setProperty(InlineStylesProperty, styles);
    return format;
}

void insertMapped(QTextCursor& cursor, const QString& text, const QTextCharFormat& format,
                  Block* data, int sourceStart) {
    cursor.insertText(text, format);
    if (!data) return;
    for (int i = 0; i < text.size(); ++i) {
        data->starts.append(sourceStart + i);
        data->ends.append(sourceStart + i + 1);
    }
}

int closingStyleTag(const QString& text, const QString& opening, const QString& closing, int from) {
    int depth = 0;
    for (int i = from; i < text.size(); ++i) {
        if (escaped(text, i)) continue;
        if (text.midRef(i, opening.size()) == opening) { ++depth; i += opening.size() - 1; }
        else if (text.midRef(i, closing.size()) == closing) {
            if (depth == 0) return i;
            --depth; i += closing.size() - 1;
        }
    }
    return -1;
}

void insertInline(QTextCursor& cursor, const QString& text, const QTextCharFormat& base,
                  int depth, Block* data, int sourceOffset, const QHash<QString, Entity>& targets,
                  ds::Theme theme) {
    if (depth > 16) { insertMapped(cursor, text, base, data, sourceOffset); return; }
    const QVector<MarkdownLink> links = markdownLinks(text);
    int linkIndex = 0;
    QString literal;
    int literalStart = 0;
    const auto flush = [&]() {
        if (literal.isEmpty()) return;
        insertMapped(cursor, literal, base, data, sourceOffset + literalStart);
        literal.clear();
    };
    int i = 0;
    while (i < text.size()) {
        while (linkIndex < links.size() && links.at(linkIndex).start < i) ++linkIndex;
        if (linkIndex < links.size() && links.at(linkIndex).start == i) {
            const MarkdownLink& link = links.at(linkIndex++);
            flush();
            QTextCharFormat format = base;
            const QString id = link.wiki ? internalLinkId(link.target) : QString();
            const auto target = targets.constFind(id);
            const bool resolved = !id.isEmpty() && target != targets.constEnd();
            QString label = link.label;
            if (label.isEmpty()) label = resolved ? target->title : QStringLiteral("关联内容不可用");
            if (resolved && target->deleted) label += QStringLiteral(" 回收站");
            if (resolved || (!link.wiki && isSafeLinkTarget(link.target))) {
                format.setAnchor(true);
                format.setAnchorHref(resolved ? id : link.target);
                format.setFontUnderline(true);
                format.setForeground(ds::color(ds::ColorToken::Primary, theme));
                format.setToolTip(resolved ? target->title : link.target);
            }
            format.setProperty(LinkSourceProperty, text.mid(link.start, link.length));
            static quint64 nextLinkToken = 0;
            format.setProperty(LinkTokenProperty, QVariant::fromValue(++nextLinkToken));
            format.setProperty(LinkOffsetProperty, sourceOffset + link.start);
            format.setProperty(LinkOuterStylesProperty, base.property(InlineStylesProperty));
            const int before = cursor.position();
            if (link.wiki) {
                format.setProperty(LinkLabelProperty, label);
                cursor.insertText(label, format);
            } else {
                QTextDocument display;
                QTextCursor displayCursor(&display);
                insertInline(displayCursor, link.labelSource, base, depth + 1, nullptr, 0, targets, theme);
                format.setProperty(LinkLabelProperty, display.toPlainText());
                insertInline(cursor, link.labelSource, format, depth + 1, nullptr, 0, targets, theme);
            }
            if (data) {
                for (int character = 0; character < cursor.position() - before; ++character) {
                    data->starts.append(sourceOffset + link.start);
                    data->ends.append(sourceOffset + link.start + link.length);
                }
            }
            i += link.length;
            continue;
        }
        if (text.at(i) == QLatin1Char('\\') && i + 1 < text.size()) {
            flush();
            QTextCharFormat format = base;
            format.setProperty(EscapedProperty, true);
            insertMapped(cursor, text.mid(i + 1, 1), format, data, sourceOffset + i + 1);
            i += 2;
            continue;
        }
        if (text.at(i) == QLatin1Char('`')) {
            int count = 1;
            while (i + count < text.size() && text.at(i + count) == QLatin1Char('`')) ++count;
            const QString delimiter(count, QLatin1Char('`'));
            const int end = findCodeEnd(text, count, i + count);
            if (end > i + count) {
                flush();
                QString content = text.mid(i + count, end - i - count);
                const bool padded = content.startsWith(QLatin1Char(' ')) && content.endsWith(QLatin1Char(' '))
                    && !content.trimmed().isEmpty();
                if (padded) content = content.mid(1, content.size() - 2);
                QTextCharFormat format = withStyle(base, delimiter + (padded ? QStringLiteral(" ") : QString()),
                                                   (padded ? QStringLiteral(" ") : QString()) + delimiter);
                format.setFontFamily(ds::qfont(ds::FontScene::Mono).family());
                format.setBackground(ds::color(ds::ColorToken::Field, theme));
                insertMapped(cursor, content, format, data, sourceOffset + i + count + (padded ? 1 : 0));
                i = end + count;
                continue;
            }
        }
        if (text.at(i) == QLatin1Char('<')) {
            QString opening, closing;
            QTextCharFormat format = base;
            if (text.midRef(i, 3) == QLatin1String("<u>")) {
                opening = QStringLiteral("<u>"); closing = QStringLiteral("</u>"); format.setFontUnderline(true);
            } else {
                static const QRegularExpression mark(QStringLiteral("^<mark data-color=\"(yellow|green|blue|pink|purple)\">"));
                const auto match = mark.match(text.mid(i));
                if (match.hasMatch()) {
                    opening = match.captured(0); closing = QStringLiteral("</mark>");
                    format.setBackground(Markdown::highlightColor(match.captured(1), theme));
                }
            }
            if (!opening.isEmpty()) {
                const int end = closingStyleTag(text, closing == QLatin1String("</mark>") ? QStringLiteral("<mark ") : opening,
                                                closing, i + opening.size());
                if (end > i + opening.size()) {
                    flush();
                    insertInline(cursor, text.mid(i + opening.size(), end - i - opening.size()),
                                 withStyle(format, opening, closing), depth + 1, data,
                                 sourceOffset + i + opening.size(), targets, theme);
                    i = end + closing.size();
                    continue;
                }
            }
        }
        QString delimiter;
        enum { None, Bold, Italic, Strike } style = None;
        if (text.midRef(i, 3) == QLatin1String("***") || text.midRef(i, 3) == QLatin1String("___")) {
            delimiter = text.mid(i, 3);
        } else if (text.midRef(i, 2) == QLatin1String("**") || text.midRef(i, 2) == QLatin1String("__")) {
            delimiter = text.mid(i, 2); style = Bold;
        } else if (text.midRef(i, 2) == QLatin1String("~~")) {
            delimiter = QStringLiteral("~~"); style = Strike;
        } else if (text.at(i) == QLatin1Char('*') || text.at(i) == QLatin1Char('_')) {
            delimiter = text.mid(i, 1); style = Italic;
        }
        if (!delimiter.isEmpty()) {
            const int end = findToken(text, delimiter, i + delimiter.size());
            if (end > i + delimiter.size()) {
                flush();
                QTextCharFormat format = withStyle(base, delimiter, delimiter);
                if (style == Bold || delimiter.size() == 3) format.setFontWeight(QFont::Bold);
                if (style == Italic || delimiter.size() == 3) format.setFontItalic(true);
                if (style == Strike) format.setFontStrikeOut(true);
                insertInline(cursor, text.mid(i + delimiter.size(), end - i - delimiter.size()), format,
                             depth + 1, data, sourceOffset + i + delimiter.size(), targets, theme);
                i = end + delimiter.size();
                continue;
            }
        }
        if (literal.isEmpty()) literalStart = i;
        literal += text.at(i++);
    }
    flush();
}

QString semanticSignature(const QTextBlock& block) {
    const QTextBlockFormat blockFormat = block.blockFormat();
    QString signature = QString::number(blockFormat.intProperty(KindProperty)) + QLatin1Char(':')
        + blockFormat.stringProperty(PrefixProperty) + QLatin1Char(':');
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid()) continue;
        const QTextCharFormat format = fragment.charFormat();
        signature += QString::number(fragment.length()) + QLatin1Char(':')
            + format.property(InlineStylesProperty).toStringList().join(QChar(0x1f)) + QLatin1Char(':')
            + QString::number(format.boolProperty(EscapedProperty)) + QLatin1Char(':')
            + format.stringProperty(LinkSourceProperty) + QLatin1Char(':') + format.anchorHref() + QChar(0x1e);
    }
    return signature;
}

void transitionStyles(QString& source, QStringList& active, const QStringList& next) {
    int common = 0;
    while (common + 1 < active.size() && common + 1 < next.size()
           && active.at(common) == next.at(common) && active.at(common + 1) == next.at(common + 1)) common += 2;
    for (int i = active.size() - 1; i >= common; i -= 2) source += active.at(i);
    for (int i = common; i + 1 < next.size(); i += 2) source += next.at(i);
    active = next;
}

struct FenceState {
    bool inside = false;
    QChar marker;
    int openingLength = 0;
    CodeContext consume(const QString& line) {
        static const QRegularExpression fence(QStringLiteral("^ {0,3}(`{3,}|~{3,})(.*)$"));
        const auto match = fence.match(line);
        const bool opening = !inside && match.hasMatch();
        const bool closing = inside && match.hasMatch() && match.captured(1).at(0) == marker
            && match.captured(1).size() >= openingLength && match.captured(2).trimmed().isEmpty();
        const CodeContext result = opening || closing ? CodeContext::Fence : inside ? CodeContext::Content : CodeContext::Normal;
        if (opening) { inside = true; marker = match.captured(1).at(0); openingLength = match.captured(1).size(); }
        else if (closing) inside = false;
        return result;
    }
};

struct Run { QString text; QTextCharFormat format; };

void normalizeCodeDelimiters(QVector<Run>& runs) {
    for (int first = 0; first < runs.size(); ++first) {
        QStringList styles = runs.at(first).format.property(InlineStylesProperty).toStringList();
        int code = -1;
        for (int i = 0; i + 1 < styles.size(); i += 2)
            if (styles.at(i).startsWith(QLatin1Char('`'))) { code = i; break; }
        if (code < 0) continue;
        int last = first;
        QString content = runs.at(first).text;
        while (last + 1 < runs.size() && runs.at(last + 1).format.property(InlineStylesProperty).toStringList() == styles
               && runs.at(last + 1).format.property(LinkTokenProperty) == runs.at(first).format.property(LinkTokenProperty))
            content += runs.at(++last).text;
        QSet<int> lengths;
        for (int i = 0; i < content.size(); ++i) {
            if (content.at(i) != QLatin1Char('`')) continue;
            int count = 1;
            while (i + count < content.size() && content.at(i + count) == QLatin1Char('`')) ++count;
            lengths.insert(count); i += count - 1;
        }
        int length = styles.at(code).trimmed().size();
        if (lengths.contains(length)) { length = 1; while (lengths.contains(length)) ++length; }
        const bool padded = content.startsWith(QLatin1Char('`')) || content.endsWith(QLatin1Char('`'))
            || (content.startsWith(QLatin1Char(' ')) && content.endsWith(QLatin1Char(' '))
                && content.count(QLatin1Char(' ')) != content.size());
        const QString delimiter(length, QLatin1Char('`'));
        styles[code] = delimiter + (padded ? QStringLiteral(" ") : QString());
        styles[code + 1] = (padded ? QStringLiteral(" ") : QString()) + delimiter;
        for (int i = first; i <= last; ++i) runs[i].format.setProperty(InlineStylesProperty, styles);
        first = last;
    }
}

void serializeRuns(QVector<Run> runs, Block* result, bool links = true) {
    normalizeCodeDelimiters(runs);
    QStringList active;
    for (int i = 0; i < runs.size(); ++i) {
        const Run& run = runs.at(i);
        const QString linkSource = run.format.stringProperty(LinkSourceProperty);
        if (links && !linkSource.isEmpty()) {
            QString label;
            QVector<Run> labelRuns;
            const QStringList outer = run.format.property(LinkOuterStylesProperty).toStringList();
            int last = i;
            while (last + 1 < runs.size() && runs.at(last + 1).format.property(LinkTokenProperty) == run.format.property(LinkTokenProperty)
                   && runs.at(last + 1).format.stringProperty(LinkSourceProperty) == linkSource) ++last;
            for (int j = i; j <= last; ++j) {
                Run part = runs.at(j);
                label += part.text;
                QStringList styles = part.format.property(InlineStylesProperty).toStringList();
                bool startsWithOuter = styles.size() >= outer.size();
                for (int k = 0; startsWithOuter && k < outer.size(); ++k) startsWithOuter = styles.at(k) == outer.at(k);
                if (startsWithOuter) styles = styles.mid(outer.size());
                part.format.setProperty(InlineStylesProperty, styles);
                part.format.clearProperty(LinkSourceProperty); part.format.clearProperty(LinkTokenProperty);
                labelRuns.append(part);
            }
            transitionStyles(result->source, active, outer);
            QString source = linkSource;
            const auto parsed = markdownLinks(linkSource);
            if (!parsed.isEmpty() && !parsed.first().wiki) {
                Block caption;
                serializeRuns(labelRuns, &caption, false);
                // Replace only the label, retaining the original target spelling
                // and optional title rather than escaping away label formatting.
                source.replace(1, parsed.first().labelSource.size(), caption.source);
            } else if (label != run.format.stringProperty(LinkLabelProperty)) {
                source = parsed.isEmpty() ? label : markdownLink(parsed.first().target, label);
                if (source.isEmpty()) source = label;
            }
            const int start = result->source.size();
            result->source += source;
            for (int j = 0; j < label.size(); ++j) {
                result->starts.append(start); result->ends.append(result->source.size());
            }
            i = last;
            continue;
        }
        transitionStyles(result->source, active, run.format.property(InlineStylesProperty).toStringList());
        for (QChar character : run.text) {
            if (run.format.boolProperty(EscapedProperty)) result->source += QLatin1Char('\\');
            result->starts.append(result->source.size());
            result->source += character;
            result->ends.append(result->source.size());
        }
    }
    transitionStyles(result->source, active, {});
}

} // namespace

Kind kind(const QTextBlock& block) { return static_cast<Kind>(block.blockFormat().intProperty(KindProperty)); }

CodeContext codeContext(const QStringList& lines, int line) {
    FenceState state;
    CodeContext result = CodeContext::Normal;
    for (int i = 0; i <= line && i < lines.size(); ++i) result = state.consume(lines.at(i));
    return result;
}

QVector<CodeContext> codeContexts(const QStringList& lines) {
    FenceState state;
    QVector<CodeContext> result;
    result.reserve(lines.size());
    for (const QString& line : lines) result.append(state.consume(line));
    return result;
}

bool isHorizontalRule(const QString& source) {
    static const QRegularExpression rule(QStringLiteral("^ {0,3}(?:(?:\\*[ \\t]*){3,}|(?:-[ \\t]*){3,}|(?:_[ \\t]*){3,})$"));
    return rule.match(source).hasMatch();
}

bool hasSnapshot(const QTextBlock& block) {
    const auto format = block.blockFormat();
    return format.hasProperty(SourceProperty) && format.stringProperty(RenderedTextProperty) == block.text()
        && format.stringProperty(SignatureProperty) == semanticSignature(block);
}

Block readBlock(const QTextBlock& block) {
    Block result;
    const QTextBlockFormat format = block.blockFormat();
    if (hasSnapshot(block)) {
        result.source = format.stringProperty(SourceProperty);
        result.starts = format.property(SourceStartsProperty).value<QVector<int>>();
        result.ends = format.property(SourceEndsProperty).value<QVector<int>>();
        result.emptyOffset = format.intProperty(EmptyOffsetProperty);
        return result;
    }
    if (!format.hasProperty(KindProperty) && !format.hasProperty(SourceProperty)) {
        result.source = block.text();
        for (int i = 0; i < result.source.size(); ++i) { result.starts.append(i); result.ends.append(i + 1); }
        return result;
    }
    result.source = format.stringProperty(PrefixProperty);
    result.emptyOffset = result.source.size();
    QVector<Run> runs;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (fragment.isValid()) runs.append({fragment.text(), fragment.charFormat()});
    }
    serializeRuns(runs, &result);
    return result;
}

void importMarkdown(QTextDocument* document, const QString& source, const QHash<QString, Entity>& targets,
                    ds::Theme theme) {
    const QStringList lines = source.split(QLatin1Char('\n'), QString::KeepEmptyParts);
    const auto contexts = codeContexts(lines);
    document->setPlainText(source);
    for (int i = 0; i < lines.size(); ++i)
        renderBlock(document->findBlockByNumber(i), lines.at(i), contexts.at(i), targets, false, theme);
}

QString selectedMarkdown(const QTextCursor& selection) {
    if (!selection.hasSelection()) return QString();
    QTextDocument fragment;
    QTextCursor(&fragment).insertFragment(QTextDocumentFragment(selection));
    QStringList lines;
    for (QTextBlock block = fragment.begin(); block.isValid(); block = block.next()) lines.append(readBlock(block).source);
    return lines.join(QLatin1Char('\n'));
}

int insertionOffset(const QTextBlock& block, int visibleOffset, const QTextCharFormat& format) {
    const Block data = readBlock(block);
    if (data.starts.isEmpty()) return data.emptyOffset;
    const int visible = qBound(0, visibleOffset, data.starts.size());
    if (visible == 0) return data.starts.first();
    if (visible < data.starts.size() && data.starts.at(visible - 1) == data.starts.at(visible)
        && data.ends.at(visible - 1) == data.ends.at(visible)) return data.ends.at(visible - 1);
    QTextCursor previous(block);
    previous.setPosition(block.position() + visible);
    const QStringList before = previous.charFormat().property(InlineStylesProperty).toStringList();
    const QStringList after = format.property(InlineStylesProperty).toStringList();
    int common = 0;
    while (common + 1 < before.size() && common + 1 < after.size()
           && before.at(common) == after.at(common) && before.at(common + 1) == after.at(common + 1)) common += 2;
    int offset = data.ends.at(visible - 1);
    for (int i = before.size() - 1; i >= common; i -= 2) offset += before.at(i).size();
    const int limit = visible == data.starts.size() ? data.source.size() : data.starts.at(visible);
    return qMin(offset, limit);
}

QTextCharFormat insertionFormat(const QTextBlock& block, int sourceOffset) {
    const Block data = readBlock(block);
    if (data.starts.isEmpty()) return block.charFormat();
    int visible = data.starts.size();
    for (int i = 0; i < data.starts.size(); ++i) {
        if (sourceOffset <= data.starts.at(i)) { visible = i; break; }
        if (sourceOffset <= data.ends.at(i)) {
            visible = i + 1;
            while (visible < data.starts.size() && data.starts.at(visible) == data.starts.at(i)
                   && data.ends.at(visible) == data.ends.at(i)) ++visible;
            break;
        }
    }
    QTextCursor cursor(block);
    if (visible == 0) {
        cursor.movePosition(QTextCursor::NextCharacter);
        return sourceOffset < data.starts.first() ? block.charFormat() : cursor.charFormat();
    }
    cursor.setPosition(block.position() + visible);
    const auto previous = cursor.charFormat();
    if (sourceOffset > data.ends.at(visible - 1)
        || (!previous.stringProperty(LinkSourceProperty).isEmpty() && sourceOffset >= data.ends.at(visible - 1))) {
        if (visible == data.starts.size()) return block.charFormat();
        cursor.movePosition(QTextCursor::NextCharacter);
        return cursor.charFormat();
    }
    return previous;
}

void renderBlock(const QTextBlock& original, const QString& source, CodeContext context,
                 const QHash<QString, Entity>& targets, bool force, ds::Theme theme) {
    if (!original.isValid()) return;
    const auto previous = original.blockFormat();
    if (!force && hasSnapshot(original) && previous.stringProperty(SourceProperty) == source
        && previous.intProperty(ContextProperty) == static_cast<int>(context)) return;
    if (theme == ds::Theme::Count) theme = ThemeManager::instance().resolvedTheme();
    QTextCursor cursor(original);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    QTextCharFormat base;
    base.setFont(ds::qfont(ds::FontScene::Body));
    base.setFontWeight(QFont::Normal);
    base.setFontItalic(false); base.setFontUnderline(false); base.setFontStrikeOut(false);
    base.setProperty(InlineStylesProperty, QStringList());
    QTextBlockFormat format;
    Kind blockKind = Kind::Paragraph;
    int contentStart = 0;
    int headingLevel = 0;
    bool checked = false;
    QString marker;
    static const QRegularExpression heading(QStringLiteral("^( {0,3})(#{1,6})[ \\t]+(.*)$"));
    static const QRegularExpression task(QStringLiteral("^([ \\t]*[-+*][ \\t]+\\[)([ xX])(\\][ \\t]+)(.*)$"));
    static const QRegularExpression unordered(QStringLiteral("^[ \\t]*[-+*][ \\t]+(.*)$"));
    static const QRegularExpression ordered(QStringLiteral("^[ \\t]*([0-9]+[.)])[ \\t]+(.*)$"));
    static const QRegularExpression quote(QStringLiteral("^[ \\t]*>[ \\t]?(.*)$"));
    if (context == CodeContext::Fence) {
        blockKind = Kind::CodeFence; contentStart = source.size();
        base.setFontFamily(ds::qfont(ds::FontScene::Mono).family());
        format.setBackground(ds::color(ds::ColorToken::Field, theme));
        format.setLeftMargin(8); format.setRightMargin(8);
    } else if (context == CodeContext::Content) {
        blockKind = Kind::Code;
        base.setFontFamily(ds::qfont(ds::FontScene::Mono).family());
        format.setBackground(ds::color(ds::ColorToken::Field, theme));
        format.setLeftMargin(8); format.setRightMargin(8);
    } else if (isHorizontalRule(source)) {
        blockKind = Kind::HorizontalRule; contentStart = source.size();
        format.setTopMargin(6); format.setBottomMargin(6);
        format.setProperty(HorizontalRuleProperty, true);
    } else {
        const auto headingMatch = heading.match(source);
        const auto taskMatch = task.match(source);
        const auto unorderedMatch = unordered.match(source);
        const auto orderedMatch = ordered.match(source);
        const auto quoteMatch = quote.match(source);
        if (headingMatch.hasMatch()) {
            blockKind = Kind::Heading; headingLevel = headingMatch.captured(2).size();
            contentStart = headingMatch.capturedStart(3);
            const ds::TypeScale scale = headingLevel == 1 ? ds::TypeScale::Title : headingLevel == 2 ? ds::TypeScale::DetailTitle
                : headingLevel == 3 ? ds::TypeScale::Subtitle : ds::TypeScale::Body;
            base.setFontPointSize(ds::pointSize(scale)); base.setFontWeight(QFont::Bold);
            format.setTopMargin(headingLevel <= 2 ? 10 : 6); format.setBottomMargin(5);
        } else if (taskMatch.hasMatch()) {
            blockKind = Kind::TaskList; contentStart = taskMatch.capturedStart(4);
            checked = taskMatch.captured(2) != QLatin1String(" ");
            format.setLeftMargin(22); format.setBottomMargin(2);
            if (checked) { base.setFontStrikeOut(true); base.setForeground(ds::color(ds::ColorToken::TextTertiary, theme)); }
        } else if (unorderedMatch.hasMatch()) {
            blockKind = Kind::UnorderedList; contentStart = unorderedMatch.capturedStart(1);
            marker = QStringLiteral("•"); format.setLeftMargin(22); format.setBottomMargin(2);
        } else if (orderedMatch.hasMatch()) {
            blockKind = Kind::OrderedList; contentStart = orderedMatch.capturedStart(2);
            marker = orderedMatch.captured(1); format.setLeftMargin(qMax(22, QFontMetrics(base.font()).horizontalAdvance(marker) + 8));
            format.setBottomMargin(2);
        } else if (quoteMatch.hasMatch()) {
            blockKind = Kind::Quote; contentStart = quoteMatch.capturedStart(1);
            base.setFontItalic(true); base.setForeground(ds::color(ds::ColorToken::TextSecondary, theme));
            format.setLeftMargin(12); format.setBackground(ds::color(ds::ColorToken::Field, theme));
            format.setTopMargin(3); format.setBottomMargin(3);
        }
    }
    format.setProperty(KindProperty, static_cast<int>(blockKind));
    format.setProperty(PrefixProperty, source.left(contentStart));
    format.setProperty(HeadingLevelProperty, headingLevel);
    format.setProperty(CheckedProperty, checked);
    format.setProperty(ListMarkerProperty, marker);
    format.setProperty(ContextProperty, static_cast<int>(context));
    cursor.setBlockFormat(format);
    cursor.setBlockCharFormat(base);
    Block data;
    data.source = source; data.emptyOffset = contentStart;
    if (context == CodeContext::Content) insertMapped(cursor, source, base, &data, 0);
    else if (context != CodeContext::Fence && blockKind != Kind::HorizontalRule)
        insertInline(cursor, source.mid(contentStart), base, 0, &data, contentStart, targets, theme);
    const QTextBlock block = cursor.block();
    format = block.blockFormat();
    format.setProperty(SourceProperty, source);
    format.setProperty(RenderedTextProperty, block.text());
    format.setProperty(SourceStartsProperty, QVariant::fromValue(data.starts));
    format.setProperty(SourceEndsProperty, QVariant::fromValue(data.ends));
    format.setProperty(EmptyOffsetProperty, data.emptyOffset);
    format.setProperty(SignatureProperty, semanticSignature(block));
    cursor.setBlockFormat(format);
    // Qt 5.12 may invalidate an empty block layout after setBlockCharFormat.
    cursor.document()->markContentsDirty(block.position(), block.length());
}

QString plainLine(const QString& source, CodeContext context, const QHash<QString, Entity>& targets) {
    QTextDocument document;
    renderBlock(document.begin(), source, context, targets);
    return document.toPlainText();
}

} // namespace MarkdownDocument
} // namespace kl
