#include "ui/pages/OtherPages.h"

#include "core/deps/DependencyManager.h"
#include "core/deps/EngineInstaller.h"
#include "core/files/PathUtils.h"
#include "core/persistence/Database.h"
#include "editor/rich/RichDocumentIO.h"
#include "engines/ocr/TesseractEngine.h"
#include "ui/themes/Theme.h"
#include "ui/widgets/CommonWidgets.h"
#include "ui/widgets/Icons.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QApplication>
#include <QHBoxLayout>
#include <QPointer>
#include <QClipboard>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QProgressDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace folvio {

namespace {
QLabel* title(const QString& t, QWidget* p, const char* name = "PageTitle")
{
    auto* l = new QLabel(t, p);
    l->setObjectName(name);
    return l;
}
} // namespace

// ===========================================================================

RecentPage::RecentPage(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 22, 28, 22);
    root->setSpacing(12);
    root->addWidget(title(tr("Recent"), this));
    auto* sub = new QLabel(tr("Only file locations and conversion results are stored locally — never document contents."), this);
    sub->setObjectName("PageSubtitle");
    root->addWidget(sub);

    auto* tabs = new QTabWidget(this);
    root->addWidget(tabs, 1);

    auto* filesTab = new QWidget(tabs);
    auto* fl = new QVBoxLayout(filesTab);
    m_recent = new QTreeWidget(filesTab);
    m_recent->setHeaderLabels({tr("Name"), tr("Folder"), tr("Type"), tr("Mode"), tr("Opened")});
    m_recent->setRootIsDecorated(false);
    m_recent->setAlternatingRowColors(true);
    m_recent->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_recent->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* clearRecent = new QPushButton(tr("Clear list"), filesTab);
    fl->addWidget(m_recent, 1);
    fl->addWidget(clearRecent, 0, Qt::AlignRight);
    tabs->addTab(filesTab, Icons::get("recent"), tr("Files"));

    auto* histTab = new QWidget(tabs);
    auto* hl = new QVBoxLayout(histTab);
    m_history = new QTreeWidget(histTab);
    m_history->setHeaderLabels({tr("Time"), tr("Source"), tr("Result"), tr("Status"), tr("Duration"), tr("Details")});
    m_history->setRootIsDecorated(false);
    m_history->setAlternatingRowColors(true);
    m_history->header()->setSectionResizeMode(5, QHeaderView::Stretch);
    auto* clearHistory = new QPushButton(tr("Clear history"), histTab);
    hl->addWidget(m_history, 1);
    hl->addWidget(clearHistory, 0, Qt::AlignRight);
    tabs->addTab(histTab, Icons::get("convert"), tr("Conversion history"));

    connect(m_recent, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        const QString path = item->data(0, Qt::UserRole).toString();
        if (item->data(0, Qt::UserRole + 1).toString() == "edit")
            Q_EMIT openForEditing(path);
        else
            Q_EMIT openForConversion(path);
    });
    connect(m_recent, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* item = m_recent->itemAt(pos);
        if (!item)
            return;
        const QString path = item->data(0, Qt::UserRole).toString();
        QMenu menu(this);
        menu.addAction(Icons::get("edit"), tr("Open in editor"), this, [this, path] { Q_EMIT openForEditing(path); })
            ->setEnabled(RichDocumentIO::canEdit(QFileInfo(path).suffix().toLower()) || path.endsWith(".pdf", Qt::CaseInsensitive));
        menu.addAction(Icons::get("convert"), tr("Convert…"), this, [this, path] { Q_EMIT openForConversion(path); });
        menu.addAction(Icons::get("folder"), tr("Show in folder"), this, [path] { revealInExplorer(path); });
        menu.addAction(Icons::get("trash"), tr("Remove from list"), this, [this, path] {
            Database::instance().removeRecent(path);
            refresh();
        });
        menu.exec(m_recent->viewport()->mapToGlobal(pos));
    });
    connect(m_history, &QTreeWidget::itemActivated, this, [](QTreeWidgetItem* item) {
        const QString out = item->data(0, Qt::UserRole).toString();
        if (!out.isEmpty() && QFileInfo::exists(out))
            revealInExplorer(out);
    });
    connect(clearRecent, &QPushButton::clicked, this, [this] {
        Database::instance().clearRecent();
        refresh();
    });
    connect(clearHistory, &QPushButton::clicked, this, [this] {
        Database::instance().clearHistory();
        refresh();
    });
    refresh();
}

void RecentPage::refresh()
{
    m_recent->clear();
    for (const RecentFile& r : Database::instance().recentFiles(100)) {
        const QFileInfo fi(r.path);
        auto* item = new QTreeWidgetItem(m_recent);
        item->setText(0, fi.fileName());
        item->setIcon(0, Icons::get(r.mode == "edit" ? "edit" : "convert"));
        item->setText(1, QDir::toNativeSeparators(fi.absolutePath()));
        item->setText(2, r.format.toUpper());
        item->setText(3, r.mode == "edit" ? tr("Edited") : tr("Converted"));
        item->setText(4, QLocale().toString(r.lastOpened, QLocale::ShortFormat));
        item->setData(0, Qt::UserRole, r.path);
        item->setData(0, Qt::UserRole + 1, r.mode);
        if (!fi.exists())
            item->setDisabled(true);
    }
    m_history->clear();
    for (const HistoryEntry& h : Database::instance().history(500)) {
        auto* item = new QTreeWidgetItem(m_history);
        item->setText(0, QLocale().toString(h.when, QLocale::ShortFormat));
        item->setText(1, QFileInfo(h.inputPath).fileName());
        item->setToolTip(1, QDir::toNativeSeparators(h.inputPath));
        item->setText(2, h.outputPath.isEmpty() ? h.outputFormat.toUpper() : QFileInfo(h.outputPath).fileName());
        item->setToolTip(2, QDir::toNativeSeparators(h.outputPath));
        item->setText(3, h.status == "ok" ? tr("Done") : h.status == "cancelled" ? tr("Cancelled") : tr("Failed"));
        item->setForeground(3, h.status == "ok" ? tc().success : h.status == "failed" ? tc().danger : tc().textMuted);
        item->setText(4, QStringLiteral("%1 s").arg(h.durationMs / 1000.0, 0, 'f', 1));
        item->setText(5, h.message);
        item->setData(0, Qt::UserRole, h.outputPath);
    }
}

// ===========================================================================

SettingsPage::SettingsPage(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(28, 22, 28, 22);
    root->setSpacing(14);
    root->addWidget(title(tr("Settings"), content));
    Database& db = Database::instance();

    auto card = [content, root](const QString& heading) {
        auto* f = new QFrame(content);
        f->setProperty("card", true);
        auto* l = new QVBoxLayout(f);
        l->setContentsMargins(16, 14, 16, 14);
        l->setSpacing(8);
        l->addWidget(title(heading, f, "SectionTitle"));
        root->addWidget(f);
        return l;
    };

    // Appearance
    {
        auto* l = card(tr("Appearance"));
        m_theme = new QButtonGroup(this);
        auto* row = new QHBoxLayout;
        const QString current = db.setting("theme", "system").toString();
        int id = 0;
        for (const auto& [key, label] : QList<std::pair<QString, QString>>{{"system", tr("Follow Windows")}, {"light", tr("Light")}, {"dark", tr("Dark")}}) {
            auto* rb = new QRadioButton(label);
            rb->setProperty("key", key);
            rb->setChecked(current == key);
            m_theme->addButton(rb, id++);
            row->addWidget(rb);
        }
        row->addStretch();
        l->addLayout(row);
        connect(m_theme, &QButtonGroup::idClicked, this, [this](int) {
            const QString key = m_theme->checkedButton()->property("key").toString();
            Database::instance().setSetting("theme", key);
            Theme::instance().apply(Theme::modeFromString(key));
            Q_EMIT themeChanged();
        });
    }

    // Saving
    {
        auto* l = card(tr("Saving & backups"));
        auto* backup = new QCheckBox(tr("Keep a backup copy before overwriting an original file"), content);
        backup->setChecked(db.setting("backup.enabled", true).toBool());
        auto* where = new QComboBox(content);
        where->addItem(tr("In Folvio's backup folder (keeps your folders clean)"), "appdata");
        where->addItem(tr("Next to the original file"), "beside");
        where->setCurrentIndex(std::max(0, where->findData(db.setting("backup.location", "appdata"))));
        auto* confirm = new QCheckBox(tr("Ask for confirmation before the first save into an original file"), content);
        confirm->setChecked(db.setting("save.confirmFirst", true).toBool());
        auto* openBackups = new QPushButton(Icons::get("folder"), tr("Open backup folder"), content);
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
        form->addRow(backup);
        form->addRow(tr("Backup location"), where);
        form->addRow(confirm);
        l->addLayout(form);
        l->addWidget(openBackups, 0, Qt::AlignLeft);
        connect(backup, &QCheckBox::toggled, this, [](bool on) { Database::instance().setSetting("backup.enabled", on); });
        connect(where, &QComboBox::currentIndexChanged, this, [where] { Database::instance().setSetting("backup.location", where->currentData()); });
        connect(confirm, &QCheckBox::toggled, this, [](bool on) { Database::instance().setSetting("save.confirmFirst", on); });
        connect(openBackups, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(PathUtils::backupDir())); });
    }

    // Conversion
    {
        auto* l = card(tr("Conversion"));
        auto* parallel = new QSpinBox(content);
        parallel->setRange(1, std::max(1, std::min(8, QThread::idealThreadCount())));
        parallel->setValue(db.setting("convert.parallel", std::clamp(QThread::idealThreadCount() / 2, 1, 3)).toInt());
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
        form->addRow(tr("Parallel conversions"), parallel);
        auto* hint = new QLabel(tr("Video and office conversions are CPU and memory intensive. Each external engine runs "
                                   "with a memory cap and is stopped completely when cancelled."), content);
        hint->setWordWrap(true);
        hint->setProperty("muted", true);
        l->addLayout(form);
        l->addWidget(hint);
        connect(parallel, &QSpinBox::valueChanged, this, [](int v) { Database::instance().setSetting("convert.parallel", v); });
    }

    // Engines
    {
        auto* l = card(tr("Engines"));
        auto* info = new QLabel(tr("Folvio uses these local engines. Missing engines only disable the conversions that need "
                                   "them. Double-click a row to choose a different executable."), content);
        info->setWordWrap(true);
        info->setProperty("muted", true);
        l->addWidget(info);
        m_engines = new QTableWidget(0, 5, content);
        m_engines->setHorizontalHeaderLabels({tr("Engine"), tr("Status"), tr("Version"), tr("Purpose"), tr("Licence")});
        m_engines->verticalHeader()->hide();
        m_engines->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_engines->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_engines->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        m_engines->setMinimumHeight(230);
        l->addWidget(m_engines);
        auto* refresh = new QPushButton(Icons::get("retry"), tr("Detect again"), content);
        m_installEngines = new QPushButton(Icons::get("plus"), tr("Install missing engines…"), content);
        m_installEngines->setToolTip(tr("FFmpeg (audio/video), LibreOffice (office documents) and Tesseract (OCR) are "
                                        "separate free programs. Folvio can install them for you."));
        auto* engineButtons = new QHBoxLayout;
        engineButtons->addWidget(refresh);
        engineButtons->addWidget(m_installEngines);
        engineButtons->addStretch();
        l->addLayout(engineButtons);
        connect(m_installEngines, &QPushButton::clicked, this, &SettingsPage::installEngines);
        auto* ocr = new QLabel(content);
        ocr->setWordWrap(true);
        ocr->setProperty("muted", true);
        const QStringList langs = TesseractEngine::availableLanguages();
        ocr->setText(tr("OCR languages: %1").arg(langs.isEmpty() ? tr("none") : langs.join(", ")));
        l->addWidget(ocr);
        connect(refresh, &QPushButton::clicked, this, [this] {
            DependencyManager::instance().refresh();
            refreshEngines();
        });
        connect(m_engines, &QTableWidget::cellDoubleClicked, this, [this](int row) {
            const QString id = m_engines->item(row, 0)->data(Qt::UserRole).toString();
            if (id == "pdfium" || id == "word")
                return;
#ifdef Q_OS_WIN
            const QString filter = tr("Programs (*.exe *.com)");
#else
            const QString filter = tr("All files (*)");
#endif
            const QString exe = QFileDialog::getOpenFileName(this, tr("Choose executable"), QString(), filter);
            if (exe.isEmpty())
                return;
            DependencyManager::instance().setOverride(id, exe);
            Database::instance().setSetting("engine." + id, exe);
            refreshEngines();
        });
        refreshEngines();
    }

    // Privacy
    {
        auto* l = card(tr("Privacy"));
        auto* text = new QLabel(tr("Folvio works entirely on this computer. It has no account, sends no telemetry and never "
                                   "uploads documents. Macros in office documents are never executed (LibreOffice runs in a "
                                   "locked-down profile; Word automation runs with macros force-disabled).\n\n"
                                   "Stored locally: recent file paths, conversion history (paths and status), settings, "
                                   "backups of overwritten files, and — only while a document has unsaved edits — a recovery "
                                   "file that is deleted on save or close."), content);
        text->setWordWrap(true);
        l->addWidget(text);
        auto* clearTemp = new QPushButton(tr("Delete temporary files now"), content);
        l->addWidget(clearTemp, 0, Qt::AlignLeft);
        connect(clearTemp, &QPushButton::clicked, this, [this] {
            const int n = PathUtils::cleanupStaleTemp(0);
            QMessageBox::information(this, tr("Temporary files"), tr("Removed %n temporary folder(s).", nullptr, n));
        });
    }

    // About
    {
        auto* l = card(tr("About"));
        auto* text = new QLabel(tr("Folvio %1 · Qt %2 · PDFium\nThird-party licences are listed in THIRD_PARTY_LICENSES.md "
                                   "in the installation folder.")
                                    .arg(QCoreApplication::applicationVersion(), QString::fromLatin1(qVersion())), content);
        text->setProperty("muted", true);
        l->addWidget(text);
        auto* lic = new QPushButton(tr("Open licences"), content);
        l->addWidget(lic, 0, Qt::AlignLeft);
        connect(lic, &QPushButton::clicked, this, [] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(PathUtils::documentationDir() + "/THIRD_PARTY_LICENSES.md"));
        });
    }
    root->addStretch();
}

void SettingsPage::refreshEngines()
{
    const QList<ToolInfo> tools = DependencyManager::instance().all();
    m_engines->setRowCount(int(tools.size()));
    for (int i = 0; i < tools.size(); ++i) {
        const ToolInfo& t = tools[i];
        auto* name = new QTableWidgetItem(t.displayName);
        name->setData(Qt::UserRole, t.id);
        name->setToolTip(QDir::toNativeSeparators(t.path));
        auto* status = new QTableWidgetItem(t.available ? tr("Available") : (t.optional ? tr("Not installed (optional)") : tr("Missing")));
        status->setForeground(t.available ? tc().success : tc().warning);
        if (!t.available)
            status->setToolTip(t.installHint);
        m_engines->setItem(i, 0, name);
        m_engines->setItem(i, 1, status);
        m_engines->setItem(i, 2, new QTableWidgetItem(t.version));
        m_engines->setItem(i, 3, new QTableWidgetItem(t.purpose));
        m_engines->setItem(i, 4, new QTableWidgetItem(t.license));
    }
    m_engines->resizeColumnsToContents();
    m_engines->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    if (m_installEngines)
        m_installEngines->setEnabled(!EngineInstaller::missingEngines().isEmpty());
}

void SettingsPage::installEngines()
{
    const QStringList missing = EngineInstaller::missingEngines();
    if (missing.isEmpty())
        return;
    QStringList names;
    for (const QString& id : missing)
        names << DependencyManager::instance().tool(id).displayName;
#ifdef Q_OS_WIN
    if (!EngineInstaller::wingetAvailable()) {
        QMessageBox box(QMessageBox::Information, tr("Install engines"),
                        tr("The Windows package manager (winget) is not available. Download the engines from their "
                           "official pages, install them, then choose \"Detect again\":\n\n%1").arg(names.join("\n")),
                        QMessageBox::Ok, this);
        auto* open = box.addButton(tr("Open download pages"), QMessageBox::ActionRole);
        box.exec();
        if (box.clickedButton() == open)
            for (const QString& id : missing)
                QDesktopServices::openUrl(QUrl(EngineInstaller::downloadPage(id)));
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Install engines"),
        tr("Folvio will install these free programs with the Windows package manager (winget). They are downloaded "
           "from their publishers over the internet (FFmpeg about 100 MB, LibreOffice about 350 MB, Tesseract about "
           "60 MB). Windows may ask for administrator permission.\n\n%1\n\nContinue?").arg(names.join("\n")));
    if (answer != QMessageBox::Yes)
        return;
    m_installEngines->setEnabled(false);
    auto* progress = new QProgressDialog(tr("Installing engines…"), tr("Cancel"), 0, int(missing.size()), this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setValue(0);
    auto cancel = std::make_shared<std::atomic_bool>(false);
    connect(progress, &QProgressDialog::canceled, this, [cancel] { cancel->store(true); });
    QPointer<SettingsPage> self(this);
    auto* watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, progress, names] {
        const QStringList failures = watcher->result();
        watcher->deleteLater();
        progress->close();
        progress->deleteLater();
        DependencyManager::instance().refresh();
        refreshEngines();
        if (failures.isEmpty())
            QMessageBox::information(this, tr("Install engines"), tr("Installed: %1").arg(names.join(", ")));
        else
            QMessageBox::warning(this, tr("Install engines"),
                                 tr("Some engines were not installed:\n\n%1").arg(failures.join("\n\n")));
    });
    watcher->setFuture(QtConcurrent::run([missing, cancel, self, progress] {
        QStringList failures;
        int done = 0;
        for (const QString& id : missing) {
            if (cancel->load())
                break;
            const QString name = DependencyManager::instance().tool(id).displayName;
            QMetaObject::invokeMethod(qApp, [self, progress, name, done] {
                if (self && progress) {
                    progress->setLabelText(QObject::tr("Installing %1…").arg(name));
                    progress->setValue(done);
                }
            }, Qt::QueuedConnection);
            QString log;
            if (!EngineInstaller::installWithWinget(id, &log, cancel.get()))
                failures << name + ": " + log.right(400);
            ++done;
        }
        return failures;
    }));
#else
    const QString command = EngineInstaller::linuxInstallCommand(missing);
    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(tr("Install engines"));
    if (command.isEmpty()) {
        box.setText(tr("Install these programs with your distribution's package manager, then choose \"Detect again\":"
                       "\n\n%1").arg(names.join("\n")));
    } else {
        box.setText(tr("Install the missing engines with your package manager (in a terminal), then choose "
                       "\"Detect again\":"));
        box.setInformativeText(command);
        box.setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* copy = box.addButton(tr("Copy command"), QMessageBox::ActionRole);
        box.addButton(QMessageBox::Close);
        box.exec();
        if (box.clickedButton() == copy)
            QApplication::clipboard()->setText(command);
        return;
    }
    box.exec();
#endif
}

} // namespace folvio
