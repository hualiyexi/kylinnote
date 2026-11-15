#pragma once

#include "ModalDialog.h"
#include "../domain/Entity.h"

class QLineEdit;
class QPushButton;
class QToolButton;

namespace kl {

class FolderEditorDialog : public ModalDialog {
    Q_OBJECT
public:
    explicit FolderEditorDialog(QWidget* parent = nullptr);

    void setNewFolderDefaults();
    void setFolder(const Folder& folder);
    Folder draft() const;

protected:
    void applyTheme() override;

private:
    void openIconPicker();
    void revalidate();

    Folder m_original;
    QLineEdit* m_nameEdit = nullptr;
    QToolButton* m_iconButton = nullptr;
    QPushButton* m_saveButton = nullptr;
    QString m_icon;
    bool m_editing = false;
};

} // namespace kl
