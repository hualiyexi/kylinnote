#include "Markdown.h"
#include <md4c-html.h>
#include <QDesktopServices>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QTextDocument>
#include <QUrl>
#include <algorithm>
#include "../app/TaskStore.h"
#include "../domain/InternalLink.h"
#include "AppStyle.h"
#include "EntityHoverPreview.h"
#include "design/ThemeManager.h"

namespace kl {
namespace {
struct HighlightSpec {
    const char* name;
    const char* label;
    ds::ColorToken token;
};
const HighlightSpec highlights[] = {
    {"yellow", "黄色", ds::ColorToken::HighlightYellow},
    {"green", "绿色", ds::ColorToken::HighlightGreen},
    {"blue", "蓝色", ds::ColorToken::HighlightBlue},
    {"pink", "粉色", ds::ColorToken::HighlightPink},
    {"purple", "紫色", ds::ColorToken::HighlightPurple},
};

// MD4C still runs with NOHTML. Only balanced, exact editor-owned tags are promoted
// from escaped text; code and HTML attributes are never candidates for promotion.
QString renderInlineExtensions(const QString& html) {
    static const QRegularExpression tokens(QStringLiteral(
        "<code\\b[^>]*>[\\s\\S]*?</code>|<[^>]+>|"
        "&lt;mark data-color=&quot;(yellow|green|blue|pink|purple)&quot;&gt;|"
        "&lt;(/?)(u|mark)&gt;"));
    struct Opening { QString tag; QString color; int start; int length; };
    struct Replacement { int start; int length; QString text; };
    QVector<Opening> stack;
    QVector<Replacement> replacements;
    QRegularExpressionMatchIterator matches = tokens.globalMatch(html);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        if (match.captured(0).startsWith(QLatin1Char('<'))) {
            // Inline extensions cannot cross block boundaries. Generated inline tags
            // (strong, em, links, code) may safely remain inside the styled range.
            if (match.captured(0).startsWith(QStringLiteral("</p"))
                || match.captured(0).startsWith(QStringLiteral("</li"))
                || match.captured(0).startsWith(QStringLiteral("</h"))) stack.clear();
            continue;
        }
        const bool highlight = !match.captured(1).isEmpty();
        const QString tag = highlight ? QStringLiteral("mark") : match.captured(3);
        if (highlight || (tag == QLatin1String("u") && match.captured(2).isEmpty())) {
            stack.append({tag, match.captured(1), match.capturedStart(), match.capturedLength()});
        } else if (!stack.isEmpty() && stack.last().tag == tag && match.captured(2) == QLatin1String("/")) {
            const Opening opening = stack.takeLast();
            const QString start = tag == QLatin1String("u") ? QStringLiteral("<u>")
                : QStringLiteral("<span style=\"background-color:%1;\">")
                    .arg(Markdown::highlightColor(opening.color).name());
            replacements.append({opening.start, opening.length, start});
            replacements.append({match.capturedStart(), match.capturedLength(),
                                 tag == QLatin1String("u") ? QStringLiteral("</u>") : QStringLiteral("</span>")});
        }
    }
    std::sort(replacements.begin(), replacements.end(), [](const Replacement& left, const Replacement& right) {
        return left.start < right.start;
    });
    QString result;
    int previous = 0;
    for (const Replacement& replacement : replacements) {
        result += html.mid(previous, replacement.start - previous);
        result += replacement.text;
        previous = replacement.start + replacement.length;
    }
    result += html.mid(previous);
    return result;
}

QString literalMarker(const QString& prefix) {
    return prefix + QStringLiteral("escaped-lt");
}

QString protectEscapedMarkup(const QString& source, const QString& prefix) {
    QString result;
    for (int p = 0; p < source.size(); ++p) {
        if (source.at(p) == QLatin1Char('\\') && p + 1 < source.size()) {
            if (source.at(p + 1) == QLatin1Char('<')) {
                result += literalMarker(prefix);
                ++p;
                continue;
            }
            if (source.at(p + 1) == QLatin1Char('\\')) {
                result += QStringLiteral("\\\\");
                ++p;
                continue;
            }
        }
        result += source.at(p);
    }
    return result;
}

QString restoreEscapedMarkup(const QString& html, const QString& prefix) {
    // Markdown's escaped '<' must not become an editor-owned <u>/<mark> tag.
    // Within code the backslash is literal, so preserve it there as well.
    static const QRegularExpression code(QStringLiteral("<code\\b[^>]*>[\\s\\S]*?</code>"));
    const QString marker = literalMarker(prefix);
    QString result;
    int previous = 0;
    QRegularExpressionMatchIterator matches = code.globalMatch(html);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        QString text = html.mid(previous, match.capturedStart() - previous);
        result += text.replace(marker, QStringLiteral("&lt;"));
        text = match.captured(0);
        result += text.replace(marker, QStringLiteral("\\&lt;"));
        previous = match.capturedEnd();
    }
    QString tail = html.mid(previous);
    result += tail.replace(marker, QStringLiteral("&lt;"));
    return result;
}

// A reference alias is literal text, not Markdown. Render-only placeholders keep
// MD4C from interpreting its brackets, pipes, HTML-looking text or emphasis. The
// prefix does not occur in the source, so source text cannot impersonate a slot.
QString referenceSource(const QString& source, const QVector<MarkdownLink>& links,
                        QString* prefix, QVector<MarkdownLink>* references) {
    int nonce = 0;
    do { *prefix = QStringLiteral("kylin-reference-%1-").arg(nonce++); }
    while (source.contains(*prefix));
    QString result;
    int previous = 0;
    for (const MarkdownLink& link : links) {
        if (!link.wiki) continue;
        result += protectEscapedMarkup(source.mid(previous, link.start - previous), *prefix);
        result += QStringLiteral("[[%1%2]]").arg(*prefix).arg(references->size());
        references->append(link);
        previous = link.start + link.length;
    }
    result += protectEscapedMarkup(source.mid(previous), *prefix);
    return result;
}

QString safeExternalLinks(const QString& html) {
    static const QRegularExpression anchor(QStringLiteral("<a\\s+[^>]*href=\"([^\"]*)\"[^>]*>([\\s\\S]*?)</a>"));
    QString result;
    int previous = 0;
    QRegularExpressionMatchIterator matches = anchor.globalMatch(html);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += html.mid(previous, match.capturedStart() - previous);
        const QString href = match.captured(1);
        // Only generated wiki references may create internal anchors. Ordinary
        // Markdown links and autolinks are restricted to safe external schemes.
        result += internalLinkId(href).isEmpty() && isSafeLinkTarget(href)
            ? match.captured(0) : match.captured(2);
        previous = match.capturedEnd();
    }
    result += html.mid(previous);
    return result;
}

QString referenceHtml(const MarkdownLink& link, const Markdown::EntityMap& targets) {
    const auto target = targets.constFind(link.target);
    QString label = link.label;
    if (target == targets.constEnd()) {
        if (label.trimmed().isEmpty()) label = QStringLiteral("关联内容不可用");
        return QStringLiteral("<span class=\"broken-link\">%1</span>").arg(label.toHtmlEscaped());
    }
    if (label.isEmpty()) label = target->title;
    if (label.trimmed().isEmpty()) label = target->kind == EntityKind::Note
        ? QStringLiteral("未命名笔记") : QStringLiteral("未命名任务");
    if (target->deleted) {
        return QStringLiteral("<a href=\"%1\" class=\"internal-link trash-link\" data-entity-id=\"%1\">%2 <span class=\"trash-marker\">回收站</span></a>")
            .arg(link.target, label.toHtmlEscaped());
    }
    return QStringLiteral("<a href=\"%1\" class=\"internal-link\" data-entity-id=\"%1\">%2</a>")
        .arg(link.target, label.toHtmlEscaped());
}

QString renderReferences(const QString& html, const QString& prefix,
                         const QVector<MarkdownLink>& references, const Markdown::EntityMap& targets,
                         const QString& source) {
    // NOHTML remains enabled. Only these exact MD4C-generated tags are promoted;
    // a literal <x-wikilink> in the source stays escaped, including inside code.
    static const QRegularExpression wiki(QStringLiteral(
        "<x-wikilink data-target=\"([^\"]*)\">([\\s\\S]*?)</x-wikilink>"));
    QString prepared = html;
    // If a surrounding Markdown block made MD4C treat a slot as literal text,
    // keep the source readable rather than ever exposing a render-only marker.
    // Restore before inserting titles/aliases, which may themselves contain text
    // that happens to resemble a marker.
    for (int slot = 0; slot < references.size(); ++slot) {
        const MarkdownLink& link = references.at(slot);
        prepared.replace(QStringLiteral("[[%1%2]]").arg(prefix).arg(slot),
                         source.mid(link.start, link.length).toHtmlEscaped());
    }
    QString result;
    int previous = 0;
    QRegularExpressionMatchIterator matches = wiki.globalMatch(prepared);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += prepared.mid(previous, match.capturedStart() - previous);
        const QString target = match.captured(1);
        bool ok = false;
        const int slot = target.startsWith(prefix) ? target.mid(prefix.size()).toInt(&ok) : -1;
        if (ok && slot >= 0 && slot < references.size() && target == prefix + QString::number(slot))
            result += referenceHtml(references.at(slot), targets);
        else
            result += match.captured(2); // Non-UUID wiki text is never resolved by title.
        previous = match.capturedEnd();
    }
    result += prepared.mid(previous);
    return result;
}
} // namespace

Markdown::EntityMap Markdown::entityMap(const QVector<Entity>& entities) {
    EntityMap result;
    for (const Entity& entity : entities) result.insert(entity.id, entity);
    return result;
}
QStringList Markdown::highlightColors() {
    QStringList result;
    for (const HighlightSpec& spec : highlights) result.append(QLatin1String(spec.name));
    return result;
}
QString Markdown::highlightLabel(const QString& name) {
    for (const HighlightSpec& spec : highlights)
        if (name == QLatin1String(spec.name)) return QString::fromUtf8(spec.label);
    return QString();
}
QColor Markdown::highlightColor(const QString& name) {
    for (const HighlightSpec& spec : highlights)
        if (name == QLatin1String(spec.name)) return ThemeManager::instance().color(spec.token);
    return QColor();
}
QString Markdown::toHtml(const QString& source, const EntityMap& targets) {
    QString prefix;
    QVector<MarkdownLink> references;
    const QString renderedSource = referenceSource(source, markdownLinks(source), &prefix, &references);
    const QByteArray input = renderedSource.toUtf8(); QByteArray html;
    const auto append = [](const MD_CHAR* text, MD_SIZE size, void* data) {
        static_cast<QByteArray*>(data)->append(text, static_cast<int>(size));
    };
    const unsigned flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                           MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_WIKILINKS | MD_FLAG_NOHTML;
    if (md_html(input.constData(), static_cast<MD_SIZE>(input.size()), append, &html, flags, 0) != 0)
        return QStringLiteral("<p>%1</p>").arg(source.toHtmlEscaped());
    QString result = QString::fromUtf8(html);
    // MD4C adds a layout newline after its final block. Under Qt's pre-wrap
    // preview CSS that newline becomes a phantom trailing space. Remove only
    // whitespace outside the closing block; body/code whitespace stays intact.
    static const QRegularExpression trailingBlockWhitespace(QStringLiteral(
        "(</(?:p|h[1-6]|ul|ol|blockquote|pre|table)>)[ \\t\\r\\n]+$"));
    result.replace(trailingBlockWhitespace, QStringLiteral("\\1"));
    result.replace(QStringLiteral("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled checked>"),
                   QStringLiteral("&#9745; "));
    result.replace(QStringLiteral("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled>"),
                   QStringLiteral("&#9744; "));
    // Resolve references last: literal alias text must never be promoted into
    // underline/highlight markup, and its generated anchors must not be treated
    // as ordinary external Markdown links.
    result = restoreEscapedMarkup(renderInlineExtensions(result), prefix);
    return renderReferences(safeExternalLinks(result), prefix, references, targets, source);
}
QString Markdown::displayHtml(const Entity& task, const EntityMap& targets) {
    return toHtml(task.body, targets);
}
QString Markdown::plainText(const Entity& task, const EntityMap& targets) {
    QTextDocument doc; doc.setHtml(displayHtml(task, targets)); return doc.toPlainText();
}
DocumentPreview::DocumentPreview(QWidget* parent) : QTextBrowser(parent) {
    setOpenLinks(false); setOpenExternalLinks(false); setFrameShape(QFrame::NoFrame);
    const auto applyTheme = [this]() {
        document()->setDefaultStyleSheet(AppStyle::previewCss());
        QPalette canvas = palette();
        canvas.setColor(QPalette::Base, ThemeManager::instance().color(ds::ColorToken::Surface));
        canvas.setColor(QPalette::Text, ThemeManager::instance().color(ds::ColorToken::Text));
        canvas.setColor(QPalette::WindowText, ThemeManager::instance().color(ds::ColorToken::Text));
        setPalette(canvas);
    };
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [applyTheme](ds::Theme) { applyTheme(); });
    setMouseTracking(true); viewport()->setMouseTracking(true);
    connect(this, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        const QString target = url.toString();
        const QString id = internalLinkId(target);
        if (!id.isEmpty()) {
            cancelHover();
            if (!m_store || !m_store->entity(id).id.isEmpty()) emit internalLinkActivated(id);
            return; // Internal navigation never goes through QDesktopServices.
        }
        if (isSafeLinkTarget(target)) QDesktopServices::openUrl(url);
    });
}
void DocumentPreview::setStore(TaskStore* store) {
    if (m_store == store && m_hover) return;
    cancelHover();
    if (m_store) disconnect(m_store, nullptr, this, nullptr);
    if (m_hover) { m_hover->deleteLater(); m_hover = nullptr; }
    m_store = store;
    if (!m_store) return;
    m_hover = new EntityHoverPreview(m_store, this);
    connect(m_store, &TaskStore::taskChanged, this, [this](QString) { refreshHover(); });
    connect(m_store, &TaskStore::tasksReset, this, &DocumentPreview::refreshHover);
}
QString DocumentPreview::internalLinkAt(const QPoint& position) const {
    const QTextCursor cursor = cursorForPosition(position);
    if (cursor.isNull()) return QString();
    return internalLinkId(cursor.charFormat().anchorHref());
}
void DocumentPreview::cancelHover() {
    m_hoverEntity.clear(); if (m_hover) m_hover->cancel();
}
void DocumentPreview::refreshHover() {
    if (!m_hoverEntity.isEmpty() && (!m_store || m_store->entity(m_hoverEntity).id.isEmpty())) { cancelHover(); return; }
    if (m_hover) m_hover->refresh();
}
void DocumentPreview::mouseMoveEvent(QMouseEvent* event) {
    const QString id = internalLinkAt(event->pos());
    if (id != m_hoverEntity) {
        cancelHover(); m_hoverEntity = id;
        if (!id.isEmpty() && m_hover) m_hover->schedule(id, event->globalPos());
    }
    QTextBrowser::mouseMoveEvent(event);
}
void DocumentPreview::mousePressEvent(QMouseEvent* event) {
    cancelHover(); QTextBrowser::mousePressEvent(event);
}
void DocumentPreview::leaveEvent(QEvent* event) {
    cancelHover(); QTextBrowser::leaveEvent(event);
}
QVariant DocumentPreview::loadResource(int, const QUrl&) {
    return QVariant();
}
} // namespace kl
