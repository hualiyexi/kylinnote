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

QString rewriteInternalLinks(const QString& html, const Markdown::EntityMap& targets) {
    const QRegularExpression anchor(QStringLiteral("<a\\s+[^>]*href=\"([^\"]+)\"[^>]*>([\\s\\S]*?)</a>"));
    QString result; int previous = 0;
    QRegularExpressionMatchIterator matches = anchor.globalMatch(html);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += html.mid(previous, match.capturedStart() - previous);
        const QString href = match.captured(1), label = match.captured(2);
        if (!isValidLinkTarget(href) || !href.startsWith(QStringLiteral("kl:"))) {
            result += match.captured(0);
        } else {
            const QString id = href.mid(3);
            const auto target = targets.constFind(id);
            if (target == targets.constEnd()) {
                result += QStringLiteral("<span class=\"broken-link\">%1</span>").arg(label);
            } else {
                const QString title = target->title.toHtmlEscaped();
                if (target->deleted) {
                    result += QStringLiteral("<a href=\"kl:%1\" class=\"internal-link trash-link\" data-entity-id=\"%1\">%2 <span class=\"trash-marker\">回收站</span></a>")
                        .arg(id, title);
                } else {
                    result += QStringLiteral("<a href=\"kl:%1\" class=\"internal-link\" data-entity-id=\"%1\">%2</a>")
                        .arg(id, title);
                }
            }
        }
        previous = match.capturedEnd();
    }
    result += html.mid(previous);
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
    const QByteArray input = source.toUtf8(); QByteArray html;
    const auto append = [](const MD_CHAR* text, MD_SIZE size, void* data) {
        static_cast<QByteArray*>(data)->append(text, static_cast<int>(size));
    };
    const unsigned flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                           MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_NOHTML;
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
    EntityMap resolved;
    const QStringList ids = internalTargets(source);
    for (const QString& id : ids) if (targets.contains(id)) resolved.insert(id, targets.value(id));
    return rewriteInternalLinks(renderInlineExtensions(result), resolved);
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
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("kl")) {
            const QString target = url.toString();
            if (isValidLinkTarget(target)) {
                const QString id = target.mid(3);
                cancelHover();
                if (!m_store || !m_store->entity(id).id.isEmpty()) emit internalLinkActivated(id);
            }
            return; // 内链绝不交给 QDesktopServices
        }
        if (scheme == QLatin1String("https") || scheme == QLatin1String("http") || scheme == QLatin1String("mailto"))
            QDesktopServices::openUrl(url);
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
    const QString href = cursor.charFormat().anchorHref();
    return href.startsWith(QStringLiteral("kl:")) && isValidLinkTarget(href) ? href.mid(3) : QString();
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
