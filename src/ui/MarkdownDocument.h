#pragma once

#include <QHash>
#include <QStringList>
#include <QTextBlock>
#include <QVector>
#include "../domain/Entity.h"
#include "design/DesignTokens.h"

class QTextCursor;
class QTextDocument;

namespace kl {
namespace MarkdownDocument {

// Semantic properties participate in QTextDocument's native undo transactions.
// Presentation colors and fonts are deliberately not part of the stored markup.
enum Property {
    SourceProperty = QTextFormat::UserProperty + 41,
    RenderedTextProperty,
    SourceStartsProperty,
    SourceEndsProperty,
    HorizontalRuleProperty,
    EmptyOffsetProperty,
    KindProperty,
    PrefixProperty,
    HeadingLevelProperty,
    CheckedProperty,
    ListMarkerProperty,
    InlineStylesProperty,
    EscapedProperty,
    LinkSourceProperty,
    LinkLabelProperty,
    LinkOuterStylesProperty,
    ContextProperty,
    SignatureProperty,
    LinkTokenProperty,
    LinkOffsetProperty
};

enum class Kind { Paragraph, Heading, UnorderedList, OrderedList, TaskList, Quote, CodeFence, Code, HorizontalRule };
enum class CodeContext { Normal, Fence, Content };

struct Block {
    QString source;
    QVector<int> starts;
    QVector<int> ends;
    int emptyOffset = 0;
};

CodeContext codeContext(const QStringList& lines, int line);
QVector<CodeContext> codeContexts(const QStringList& lines);
bool isHorizontalRule(const QString& source);
bool hasSnapshot(const QTextBlock& block);
Block readBlock(const QTextBlock& block);
void importMarkdown(QTextDocument* document, const QString& source, const QHash<QString, Entity>& targets,
                    ds::Theme theme = ds::Theme::Count);
QString selectedMarkdown(const QTextCursor& selection);
int insertionOffset(const QTextBlock& block, int visibleOffset, const QTextCharFormat& format);
QTextCharFormat insertionFormat(const QTextBlock& block, int sourceOffset);
void renderBlock(const QTextBlock& block, const QString& source, CodeContext context,
                 const QHash<QString, Entity>& targets, bool force = false, ds::Theme theme = ds::Theme::Count);
QString plainLine(const QString& source, CodeContext context, const QHash<QString, Entity>& targets);
Kind kind(const QTextBlock& block);

} // namespace MarkdownDocument
} // namespace kl
