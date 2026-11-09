#ifndef KYLINLIST_MARKDOWN_H
#define KYLINLIST_MARKDOWN_H

#include <QColor>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTextBrowser>
#include "../domain/Entity.h"
namespace kl {
class TaskStore;
class EntityHoverPreview;
namespace Markdown {
using EntityMap = QHash<QString, Entity>;
EntityMap entityMap(const QVector<Entity>& entities);
// Persist only semantic color names in <mark data-color="…">; theme colors stay in the renderer.
QStringList highlightColors();
QString highlightLabel(const QString& name);
QColor highlightColor(const QString& name);
QString toHtml(const QString& source, const EntityMap& targets = EntityMap());
QString displayHtml(const Entity& task, const EntityMap& targets = EntityMap());
QString plainText(const Entity& task, const EntityMap& targets = EntityMap());
}  // namespace Markdown
// 预览不加载外部/本地图片；内链只在预览中激活，编辑区始终为纯文本。
class DocumentPreview : public QTextBrowser {
    Q_OBJECT
public:
    explicit DocumentPreview(QWidget* parent = nullptr);
    void setStore(TaskStore* store);
    QString internalLinkAt(const QPoint& position) const;
signals:
    void internalLinkActivated(QString entityId);
protected:
    QVariant loadResource(int type, const QUrl& url) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
private:
    void cancelHover();
    void refreshHover();
    QPointer<TaskStore> m_store;
    EntityHoverPreview* m_hover = nullptr;
    QString m_hoverEntity;
    QPoint m_hoverGlobal;
};
}  // namespace kl

#endif // KYLINLIST_MARKDOWN_H
