#include "ui/pages/HomePage.h"

#include "core/files/FileFormat.h"
#include "core/persistence/Database.h"
#include "ui/widgets/CommonWidgets.h"
#include "ui/widgets/Icons.h"

#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace folvio {

HomePage::HomePage(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 22, 28, 22);
    root->setSpacing(14);

    auto* title = new QLabel(tr("Start"), this);
    title->setObjectName("PageTitle");
    auto* sub = new QLabel(tr("Drop a file to convert it, or a PDF or document to edit it in place."), this);
    sub->setObjectName("PageSubtitle");
    root->addWidget(title);
    root->addWidget(sub);

    auto* drop = new DropZone(tr("Drop files here"),
                              tr("PDF and Office documents open in the editor. Other files and multiple files open in "
                                 "the converter.\nDocuments · Spreadsheets · Presentations · Images · Audio · Video"),
                              this);
    drop->setMinimumHeight(250);
    drop->setMaximumHeight(330);
    connect(drop, &DropZone::filesDropped, this, &HomePage::filesDropped);
    setFocusProxy(drop);
    root->addWidget(drop);

    auto* recentTitle = new QLabel(tr("Recent files"), this);
    recentTitle->setObjectName("SectionTitle");
    root->addWidget(recentTitle);
    m_recent = new QTreeWidget(this);
    m_recent->setHeaderLabels({tr("Name"), tr("Folder"), tr("Type"), tr("Opened")});
    m_recent->setRootIsDecorated(false);
    m_recent->setAlternatingRowColors(true);
    m_recent->header()->setStretchLastSection(false);
    m_recent->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_recent->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_recent->setMinimumHeight(140);
    root->addWidget(m_recent, 1);
    connect(m_recent, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        const QString path = item->data(0, Qt::UserRole).toString();
        if (item->data(0, Qt::UserRole + 1).toString() == "edit")
            Q_EMIT openForEditing(path);
        else
            Q_EMIT openForConversion(path);
    });
    refresh();
}

void HomePage::refresh()
{
    m_recent->clear();
    for (const RecentFile& r : Database::instance().recentFiles(12)) {
        const QFileInfo fi(r.path);
        auto* item = new QTreeWidgetItem(m_recent);
        item->setText(0, fi.fileName());
        item->setIcon(0, Icons::get(r.mode == "edit" ? "edit" : "convert"));
        item->setText(1, QDir::toNativeSeparators(fi.absolutePath()));
        item->setText(2, r.format.toUpper());
        item->setText(3, QLocale().toString(r.lastOpened, QLocale::ShortFormat));
        item->setData(0, Qt::UserRole, r.path);
        item->setData(0, Qt::UserRole + 1, r.mode);
        if (!fi.exists()) {
            item->setDisabled(true);
            item->setToolTip(0, tr("File no longer exists"));
        }
    }
    if (m_recent->topLevelItemCount() == 0) {
        auto* item = new QTreeWidgetItem(m_recent);
        item->setText(0, tr("No recent files yet."));
        item->setDisabled(true);
    }
}

} // namespace folvio
