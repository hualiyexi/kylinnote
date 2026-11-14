#ifndef KYLINLIST_LISTEDITORDIALOG_H
#define KYLINLIST_LISTEDITORDIALOG_H

#include "ModalDialog.h"
#include "../domain/Entity.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

namespace kl {

// 清单新建/编辑表单（对齐滴答“添加清单 / 编辑清单”弹窗）。
//
// 纯表单组件：不写 Store，只维护草稿；构造时传入现有清单（用于同路径重名校验）
// 与文件夹列表（填充下拉）。字段仅：名称（左端 emoji 图标按钮 + 分隔线）、
// 清单颜色（无颜色 + 7 预设 + 彩色自定义入口）、文件夹（无 + 真实 folderId）。
// 保留原 kind/inbox/sortOrder/时间戳/revision/archivedAtUtcMs 等字段原样带回。
//
// 规则：
//   - 空名称禁用保存；
//   - 同一路径（相同 folderId，含都为空）下 trim + 忽略大小写重名 → 就地报错
//     且不接受（编辑时排除自身；已归档清单不占名）；
//   - Enter 仅在可保存时 accept，Esc/关闭为 reject。
class ListEditorDialog : public ModalDialog {
    Q_OBJECT
public:
    ListEditorDialog(const QVector<TaskList>& lists, const QVector<Folder>& folders,
                     QWidget* parent = nullptr);

    // 切换为编辑模式并回填现有清单（可在 exec 前多次调用）。
    void setList(const TaskList& existing);

    // 仅用于新建：重置为新建态并预选文件夹与清单种类（如新建笔记清单传
    // EntityKind::Note）。保持标题“添加清单”、确认按钮“添加”、isEditing=false。
    void setNewListDefaults(const QString& folderId, EntityKind kind);

    bool isEditing() const { return m_editing; }
    QString editingListId() const { return m_editing ? m_original.id : QString(); }

    // 草稿 getter：accept 后由调用方读取并落库。
    TaskList draft() const;
    QString draftName() const;       // 未改名时保留旧值；改名或新建时 trim
    QString draftColorHex() const;   // 空 = 无颜色，否则 #rrggbb
    QString draftIcon() const;       // 空 = 无 emoji 图标
    QString draftFolderId() const;   // 空 = 无文件夹

protected:
    void applyTheme() override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void updateIconButton();
    void applyColorSelection(const QColor& color);
    void openIconPicker();
    void openColorPicker();
    void revalidate();
    void tryAccept();

    QVector<TaskList> m_lists;
    QVector<Folder> m_folders;
    bool m_editing = false;
    TaskList m_original;
    EntityKind m_newKind = EntityKind::Task; // 新建草稿的清单种类（Task/Note）

    QWidget* m_nameField = nullptr;
    QToolButton* m_iconButton = nullptr;
    QLineEdit* m_nameEdit = nullptr;
    QToolButton* m_noneColorButton = nullptr;
    class ListSwatch* m_customColorButton = nullptr;
    QComboBox* m_folderCombo = nullptr;
    QLabel* m_errorLabel = nullptr;
    QPushButton* m_saveButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QVector<class ListSwatch*> m_presetColorButtons;

    QString m_icon;              // 空 = 无图标
    QColor m_color;              // 无效 = 无颜色
    QColor m_customColor;        // 自定义入口最近一次选过的颜色
};

} // namespace kl

#endif // KYLINLIST_LISTEDITORDIALOG_H
