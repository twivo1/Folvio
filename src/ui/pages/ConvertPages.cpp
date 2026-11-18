#include "ui/pages/ConvertPages.h"

#include "core/files/PathUtils.h"
#include "core/jobs/ConversionJob.h"
#include "core/persistence/Database.h"
#include "editor/rich/RichDocumentIO.h"
#include "ui/widgets/CommonWidgets.h"
#include "ui/widgets/ConversionSetupPanel.h"
#include "ui/themes/Theme.h"
#include "ui/widgets/Icons.h"

#include <QDesktopServices>
#include <QDir>
#include <QUrl>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace folvio {

namespace {

QString iconForFormat(const QString& f)
{
    switch (FormatRegistry::category(f)) {
    case FormatCategory::Image: return "image";
    case FormatCategory::Audio:
    case FormatCategory::Video: return "play";
    case FormatCategory::Spreadsheet:
    case FormatCategory::Data: return "table";
    default: return "file";
    }
}

QWidget* pageHeader(const QString& title, const QString& subtitle, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(2);
    auto* t = new QLabel(title, w);
    t->setObjectName("PageTitle");
    auto* s = new QLabel(subtitle, w);
    s->setObjectName("PageSubtitle");
    s->setWordWrap(true);
    l->addWidget(t);
    l->addWidget(s);
    return w;
}

void recordHistory(ConversionJob* job)
{
    HistoryEntry e;
    e.inputPath = job->request().inputPath;
    e.inputFormat = job->request().inputFormat;
    e.outputPath = job->result().outputs.value(0);
    e.outputFormat = job->request().outputFormat;
    e.status = job->state() == ConversionJob::State::Succeeded ? "ok"
               : job->state() == ConversionJob::State::Cancelled ? "cancelled" : "failed";
    e.durationMs = job->durationMs();
    e.message = job->result().ok ? QObject::tr("%n file(s)", nullptr, int(job->result().outputs.size())) : job->result().error;
    Database::instance().addHistory(e);
}

} // namespace

// ===========================================================================
// ConvertPage

ConvertPage::ConvertPage(JobQueue* queue, QWidget* parent) : QWidget(parent), m_queue(queue)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 22, 28, 22);
    root->setSpacing(14);
    root->addWidget(pageHeader(tr("Convert"), tr("Convert one file. Only conversions that the installed engines can really "
                                                 "perform are offered."), this));
    m_stack = new QStackedWidget(this);
    root->addWidget(m_stack, 1);

    m_drop = new DropZone(tr("Drop a file to convert"), tr("Or choose a file from your computer."), m_stack);
    connect(m_drop, &DropZone::filesDropped, this, [this](const QStringList& paths) {
        if (paths.size() > 1)
            Q_EMIT multipleFilesDropped(paths);
        else
            setFile(paths.first());
    });
    m_stack->addWidget(m_drop);

    auto* scroll = new QScrollArea(m_stack);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget;
    auto* cl = new QVBoxLayout(content);
    cl->setContentsMargins(0, 0, 8, 0);
    cl->setSpacing(14);

    auto* card = new QFrame(content);
    card->setProperty("card", true);
    auto* cardL = new QHBoxLayout(card);
    cardL->setContentsMargins(14, 12, 14, 12);
    m_fileIcon = new QLabel(card);
    auto* names = new QVBoxLayout;
    m_fileName = new QLabel(card);
    m_fileName->setStyleSheet("font-weight: 600; font-size: 11pt;");
    m_fileName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_fileMeta = new QLabel(card);
    m_fileMeta->setProperty("muted", true);
    // Long paths must never widen the page: let the labels shrink (full text in tooltip).
    m_fileMeta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_fileName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    names->addWidget(m_fileName);
    names->addWidget(m_fileMeta);
    m_edit = new QPushButton(Icons::get("edit"), tr("Open in editor"), card);
    auto* change = new QPushButton(tr("Choose another file…"), card);
    cardL->addWidget(m_fileIcon);
    cardL->addLayout(names, 1);
    cardL->addWidget(m_edit);
    cardL->addWidget(change);
    cl->addWidget(card);
    connect(change, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Choose a file"));
        if (!f.isEmpty())
            setFile(f);
    });
    connect(m_edit, &QPushButton::clicked, this, [this] { Q_EMIT openForEditing(m_file.path); });

    m_banner = new Banner(content);
    cl->addWidget(m_banner);

    auto* setupCard = new QFrame(content);
    setupCard->setProperty("card", true);
    auto* sl = new QVBoxLayout(setupCard);
    sl->setContentsMargins(16, 14, 16, 14);
    m_setup = new ConversionSetupPanel(setupCard);
    sl->addWidget(m_setup);
    cl->addWidget(setupCard);

    auto* actions = new QHBoxLayout;
    m_convert = makePrimaryButton(tr("Convert"), content);
    m_convert->setIcon(Icons::get("convert", Icons::Tone::OnAccent));
    m_convert->setEnabled(false);
    m_cancel = new QPushButton(Icons::get("cancel"), tr("Cancel"), content);
    m_cancel->hide();
    m_progress = new QProgressBar(content);
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_progress->hide();
    m_stage = new QLabel(content);
    m_stage->setProperty("muted", true);
    actions->addWidget(m_convert);
    actions->addWidget(m_cancel);
    actions->addWidget(m_progress, 1);
    actions->addWidget(m_stage);
    actions->addStretch();
    cl->addLayout(actions);
    cl->addStretch();
    scroll->setWidget(content);
    m_stack->addWidget(scroll);

    connect(m_setup, &ConversionSetupPanel::targetChanged, this, [this](const QString& t) { m_convert->setEnabled(!t.isEmpty()); });
    connect(m_convert, &QPushButton::clicked, this, &ConvertPage::start);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_job)
            m_job->cancel();
    });
    setAcceptDrops(true);
}

void ConvertPage::setFile(const QString& path)
{
    if (m_job && !m_job->isFinished()) {
        Q_EMIT statusMessage(tr("A conversion is still running."));
        return;
    }
    m_file = FormatDetector::detect(path);
    m_stack->setCurrentIndex(1);
    m_banner->hide();
    m_progress->hide();
    m_stage->clear();
    const QFileInfo fi(path);
    m_fileName->setText(fi.fileName());
    m_fileIcon->setPixmap(Icons::pixmap(iconForFormat(m_file.formatId), 32, Icons::Tone::Accent, devicePixelRatioF()));
    if (!m_file.error.isEmpty()) {
        m_fileMeta->setText(QDir::toNativeSeparators(fi.absolutePath()));
        m_banner->show(Banner::Kind::Error, m_file.error);
        m_setup->setSourceFormats({});
        m_convert->setEnabled(false);
        m_edit->hide();
        return;
    }
    const QString meta = QStringLiteral("%1 · %2 · %3")
                             .arg(m_file.formatId.isEmpty() ? tr("Unknown format") : FormatRegistry::displayName(m_file.formatId),
                                  humanSize(m_file.size), QDir::toNativeSeparators(fi.absolutePath()));
    m_fileMeta->setText(meta);
    m_fileMeta->setToolTip(meta);
    if (!m_file.contentMatchesExtension)
        m_banner->show(Banner::Kind::Warning, tr("The file extension says %1, but the content is %2. Folvio uses the real content type.")
                                                  .arg(m_file.extensionFormatId.toUpper(), m_file.formatId.toUpper()));
    else if (m_file.formatId.isEmpty())
        m_banner->show(Banner::Kind::Error, tr("This file type is not recognized."));
    m_edit->setVisible(RichDocumentIO::canEdit(m_file.formatId));
    m_setup->setSourceFormats(m_file.formatId.isEmpty() ? QStringList() : QStringList{m_file.formatId});
    m_convert->setEnabled(!m_setup->targetFormat().isEmpty());
    Database::instance().touchRecent(path, m_file.formatId, "convert");
}

void ConvertPage::start()
{
    const QString target = m_setup->targetFormat();
    const ConversionPlan plan = m_setup->planFor(m_file.formatId);
    if (!plan.isValid())
        return;
    ConversionRequest req;
    req.inputPath = m_file.path;
    req.inputFormat = m_file.formatId;
    req.outputFormat = target;
    req.outputPath = PathUtils::outputPathFor(m_file.path, m_setup->outputDirectory(), FormatRegistry::preferredExtension(target));
    req.options = m_setup->options();
    m_job = m_queue->submit(req, plan);
    m_convert->setEnabled(false);
    m_cancel->show();
    m_progress->show();
    m_progress->setValue(0);
    m_banner->hide();
    m_stage->setText(tr("Waiting…"));
    connect(m_job, &ConversionJob::progressChanged, this, &ConvertPage::onJobProgress);
    connect(m_job, &ConversionJob::finished, this, &ConvertPage::onJobFinished);
}

void ConvertPage::onJobProgress(double f, const QString& stage)
{
    m_progress->setValue(int(f * 1000));
    m_stage->setText(stage);
}

void ConvertPage::onJobFinished()
{
    if (!m_job)
        return;
    m_cancel->hide();
    m_convert->setEnabled(true);
    m_progress->hide();
    m_stage->clear();
    recordHistory(m_job);
    const ConversionResult& r = m_job->result();
    if (m_job->state() == ConversionJob::State::Succeeded) {
        QString text = tr("Done: %1").arg(r.outputs.size() == 1 ? QDir::toNativeSeparators(r.outputs.first())
                                                                : tr("%n files in %1", nullptr, int(r.outputs.size()))
                                                                      .arg(QDir::toNativeSeparators(QFileInfo(r.outputs.first()).absolutePath())));
        if (!r.warnings.isEmpty())
            text += "\n" + r.warnings.join("\n");
        m_banner->show(Banner::Kind::Success, text, {tr("Open"), tr("Show in folder")});
        const QStringList outputs = r.outputs;
        disconnect(m_banner, &Banner::actionTriggered, nullptr, nullptr);
        connect(m_banner, &Banner::actionTriggered, this, [outputs](int i) {
            if (i == 0 && outputs.size() == 1)
                QDesktopServices::openUrl(QUrl::fromLocalFile(outputs.first()));
            else
                revealInExplorer(outputs.first());
        });
        Q_EMIT statusMessage(tr("Conversion finished."));
    } else if (m_job->state() == ConversionJob::State::Cancelled) {
        m_banner->show(Banner::Kind::Info, tr("Conversion cancelled. No output was written."));
    } else {
        m_banner->show(Banner::Kind::Error, tr("Conversion failed: %1").arg(r.error), {tr("Try again")});
        disconnect(m_banner, &Banner::actionTriggered, nullptr, nullptr);
        connect(m_banner, &Banner::actionTriggered, this, [this](int) { start(); });
    }
}

// ===========================================================================
// BatchPage

enum Col { ColFile, ColFrom, ColStatus, ColProgress, ColOutput };

BatchPage::BatchPage(JobQueue* queue, QWidget* parent) : QWidget(parent), m_queue(queue)
{
    setAcceptDrops(true);
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(28, 22, 28, 22);
    root->setSpacing(18);

    auto* left = new QVBoxLayout;
    left->setSpacing(10);
    left->addWidget(pageHeader(tr("Batch"), tr("Convert many files at once. Drop files or folders here; each compatible file "
                                               "is converted to the chosen format."), this));
    auto* tools = new QHBoxLayout;
    auto* add = new QPushButton(Icons::get("plus"), tr("Add files…"), this);
    auto* addDir = new QPushButton(Icons::get("folder"), tr("Add folder…"), this);
    auto* remove = new QPushButton(Icons::get("trash"), tr("Remove"), this);
    auto* clear = new QPushButton(tr("Clear finished"), this);
    tools->addWidget(add);
    tools->addWidget(addDir);
    tools->addWidget(remove);
    tools->addWidget(clear);
    tools->addStretch();
    left->addLayout(tools);

    m_table = new QTreeWidget(this);
    m_table->setHeaderLabels({tr("File"), tr("Format"), tr("Status"), tr("Progress"), tr("Output")});
    m_table->setRootIsDecorated(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->header()->setSectionResizeMode(ColFile, QHeaderView::Stretch);
    m_table->header()->setSectionResizeMode(ColFrom, QHeaderView::ResizeToContents);
    m_table->header()->setSectionResizeMode(ColStatus, QHeaderView::ResizeToContents);
    m_table->setColumnWidth(ColProgress, 140);
    m_table->setColumnWidth(ColOutput, 200);
    left->addWidget(m_table, 1);

    auto* actions = new QHBoxLayout;
    m_start = makePrimaryButton(tr("Start all"), this);
    m_start->setIcon(Icons::get("play", Icons::Tone::OnAccent));
    m_cancelAll = new QPushButton(Icons::get("cancel"), tr("Cancel all"), this);
    m_retry = new QPushButton(Icons::get("retry"), tr("Retry failed"), this);
    m_openFolder = new QPushButton(Icons::get("folder"), tr("Open output folder"), this);
    m_summary = new QLabel(this);
    m_summary->setProperty("muted", true);
    actions->addWidget(m_start);
    actions->addWidget(m_cancelAll);
    actions->addWidget(m_retry);
    actions->addWidget(m_openFolder);
    actions->addStretch();
    actions->addWidget(m_summary);
    left->addLayout(actions);
    root->addLayout(left, 3);

    auto* setupCard = new QFrame(this);
    setupCard->setProperty("card", true);
    auto* sl = new QVBoxLayout(setupCard);
    sl->setContentsMargins(16, 14, 16, 14);
    auto* scroll = new QScrollArea(setupCard);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_setup = new ConversionSetupPanel;
    scroll->setWidget(m_setup);
    sl->addWidget(scroll);
    setupCard->setMinimumWidth(320);
    setupCard->setMaximumWidth(420);
    root->addWidget(setupCard, 2);

    connect(add, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add files"));
        addFiles(files);
    });
    connect(addDir, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Add folder"));
        if (dir.isEmpty())
            return;
        QStringList files;
        for (const QFileInfo& fi : QDir(dir).entryInfoList(QDir::Files))
            files << fi.absoluteFilePath();
        addFiles(files);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        for (QTreeWidgetItem* item : m_table->selectedItems()) {
            auto* job = item->data(0, Qt::UserRole + 2).value<ConversionJob*>();
            if (job && !job->isFinished())
                continue; // running jobs are cancelled first
            delete item;
        }
        refreshTargets();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        for (int i = m_table->topLevelItemCount() - 1; i >= 0; --i) {
            auto* item = m_table->topLevelItem(i);
            auto* job = item->data(0, Qt::UserRole + 2).value<ConversionJob*>();
            if (job && job->isFinished())
                delete item;
        }
        refreshTargets();
    });
    connect(m_start, &QPushButton::clicked, this, &BatchPage::startAll);
    connect(m_cancelAll, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < m_table->topLevelItemCount(); ++i)
            if (auto* job = m_table->topLevelItem(i)->data(0, Qt::UserRole + 2).value<ConversionJob*>())
                job->cancel();
    });
    connect(m_retry, &QPushButton::clicked, this, &BatchPage::retryFailed);
    connect(m_openFolder, &QPushButton::clicked, this, [this] {
        if (!m_lastOutputDir.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastOutputDir));
    });
    connect(m_table, &QTreeWidget::customContextMenuRequested, this, &BatchPage::showContextMenu);
    connect(m_setup, &ConversionSetupPanel::targetChanged, this, [this] {
        for (int i = 0; i < m_table->topLevelItemCount(); ++i)
            updateRow(m_table->topLevelItem(i));
        updateSummary();
    });
    updateSummary();
}

void BatchPage::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void BatchPage::dropEvent(QDropEvent* e)
{
    QStringList files;
    for (const QUrl& u : e->mimeData()->urls()) {
        const QFileInfo fi(u.toLocalFile());
        if (fi.isDir()) {
            for (const QFileInfo& f : QDir(fi.absoluteFilePath()).entryInfoList(QDir::Files))
                files << f.absoluteFilePath();
        } else if (fi.isFile()) {
            files << fi.absoluteFilePath();
        }
    }
    addFiles(files);
}

void BatchPage::addFiles(const QStringList& paths)
{
    constexpr int kMaxFiles = 2000;
    for (const QString& p : paths) {
        if (m_table->topLevelItemCount() >= kMaxFiles) {
            Q_EMIT statusMessage(tr("At most %1 files can be queued at once.").arg(kMaxFiles));
            break;
        }
        bool dup = false;
        for (int i = 0; i < m_table->topLevelItemCount() && !dup; ++i)
            dup = PathUtils::samePath(m_table->topLevelItem(i)->data(0, Qt::UserRole).toString(), p);
        if (dup)
            continue;
        const DetectedFile d = FormatDetector::detect(p);
        auto* item = new QTreeWidgetItem(m_table);
        item->setText(ColFile, QFileInfo(p).fileName());
        item->setToolTip(ColFile, QDir::toNativeSeparators(p));
        item->setIcon(ColFile, Icons::get(iconForFormat(d.formatId)));
        item->setText(ColFrom, d.formatId.toUpper());
        item->setData(0, Qt::UserRole, p);
        item->setData(0, Qt::UserRole + 1, d.ok() ? d.formatId : QString());
        if (!d.ok())
            item->setText(ColStatus, d.error.isEmpty() ? tr("Unknown type") : d.error);
    }
    refreshTargets();
}

void BatchPage::refreshTargets()
{
    QStringList formats;
    for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
        const QString f = m_table->topLevelItem(i)->data(0, Qt::UserRole + 1).toString();
        if (!f.isEmpty())
            formats << f;
    }
    m_setup->setSourceFormats(formats);
    for (int i = 0; i < m_table->topLevelItemCount(); ++i)
        updateRow(m_table->topLevelItem(i));
    updateSummary();
}

void BatchPage::updateRow(QTreeWidgetItem* item)
{
    auto* job = item->data(0, Qt::UserRole + 2).value<ConversionJob*>();
    const QString from = item->data(0, Qt::UserRole + 1).toString();
    if (job) {
        item->setText(ColStatus, ConversionJob::stateName(job->state()));
        item->setToolTip(ColStatus, job->state() == ConversionJob::State::Failed ? job->result().error : job->stage());
        auto* bar = qobject_cast<QProgressBar*>(m_table->itemWidget(item, ColProgress));
        if (!bar) {
            bar = new QProgressBar;
            bar->setRange(0, 1000);
            bar->setTextVisible(false);
            m_table->setItemWidget(item, ColProgress, bar);
        }
        bar->setValue(int(job->progress() * 1000));
        const QColor c = job->state() == ConversionJob::State::Failed ? tc().danger
                         : job->state() == ConversionJob::State::Succeeded ? tc().success : tc().text;
        item->setForeground(ColStatus, c);
        if (job->state() == ConversionJob::State::Succeeded && !job->result().outputs.isEmpty()) {
            item->setText(ColOutput, QFileInfo(job->result().outputs.first()).fileName()
                                         + (job->result().outputs.size() > 1 ? QStringLiteral(" (+%1)").arg(job->result().outputs.size() - 1) : QString()));
            item->setToolTip(ColOutput, QDir::toNativeSeparators(job->result().outputs.first()));
        }
        return;
    }
    if (from.isEmpty())
        return;
    const QString target = m_setup->targetFormat();
    if (target.isEmpty()) {
        item->setText(ColStatus, tr("Ready"));
        item->setForeground(ColStatus, tc().textMuted);
    } else if (!ConversionPlanner().plan(from, target).isValid()) {
        item->setText(ColStatus, tr("Not supported for %1").arg(target.toUpper()));
        item->setForeground(ColStatus, tc().warning);
    } else {
        item->setText(ColStatus, tr("Ready → %1").arg(target.toUpper()));
        item->setForeground(ColStatus, tc().text);
    }
}

QTreeWidgetItem* BatchPage::itemForJob(ConversionJob* job) const
{
    for (int i = 0; i < m_table->topLevelItemCount(); ++i)
        if (m_table->topLevelItem(i)->data(0, Qt::UserRole + 2).value<ConversionJob*>() == job)
            return m_table->topLevelItem(i);
    return nullptr;
}

void BatchPage::startAll()
{
    const QString target = m_setup->targetFormat();
    if (target.isEmpty()) {
        Q_EMIT statusMessage(tr("Choose a target format first."));
        return;
    }
    const int parallel = Database::instance().setting("convert.parallel", m_queue->maxConcurrent()).toInt();
    m_queue->setMaxConcurrent(parallel);
    int started = 0;
    for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_table->topLevelItem(i);
        auto* existing = item->data(0, Qt::UserRole + 2).value<ConversionJob*>();
        if (existing && !existing->isFinished())
            continue;
        if (existing && existing->state() == ConversionJob::State::Succeeded)
            continue;
        const QString from = item->data(0, Qt::UserRole + 1).toString();
        const ConversionPlan plan = ConversionPlanner().plan(from, target);
        if (from.isEmpty() || !plan.isValid())
            continue;
        ConversionRequest req;
        req.inputPath = item->data(0, Qt::UserRole).toString();
        req.inputFormat = from;
        req.outputFormat = target;
        req.outputPath = PathUtils::outputPathFor(req.inputPath, m_setup->outputDirectory(), FormatRegistry::preferredExtension(target));
        req.options = m_setup->options();
        m_lastOutputDir = QFileInfo(req.outputPath).absolutePath();
        ConversionJob* job = m_queue->submit(req, plan);
        item->setData(0, Qt::UserRole + 2, QVariant::fromValue(job));
        connect(job, &ConversionJob::progressChanged, this, [this, job] {
            if (auto* it = itemForJob(job))
                updateRow(it);
        });
        connect(job, &ConversionJob::stateChanged, this, [this, job] {
            if (auto* it = itemForJob(job))
                updateRow(it);
            updateSummary();
        });
        connect(job, &ConversionJob::finished, this, [job] { recordHistory(job); });
        updateRow(item);
        ++started;
    }
    Q_EMIT statusMessage(started ? tr("Started %n conversion(s).", nullptr, started) : tr("Nothing to convert."));
    updateSummary();
}

void BatchPage::retryFailed()
{
    for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
        auto* job = m_table->topLevelItem(i)->data(0, Qt::UserRole + 2).value<ConversionJob*>();
        if (job && (job->state() == ConversionJob::State::Failed || job->state() == ConversionJob::State::Cancelled))
            m_queue->retry(job);
    }
    updateSummary();
}

void BatchPage::showContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = m_table->itemAt(pos);
    if (!item)
        return;
    auto* job = item->data(0, Qt::UserRole + 2).value<ConversionJob*>();
    QMenu menu(this);
    if (job && !job->isFinished())
        menu.addAction(Icons::get("cancel"), tr("Cancel"), this, [job] { job->cancel(); });
    if (job && (job->state() == ConversionJob::State::Failed || job->state() == ConversionJob::State::Cancelled))
        menu.addAction(Icons::get("retry"), tr("Retry"), this, [this, job] { m_queue->retry(job); });
    if (job && job->state() == ConversionJob::State::Succeeded) {
        const QString out = job->result().outputs.value(0);
        menu.addAction(tr("Open result"), this, [out] { QDesktopServices::openUrl(QUrl::fromLocalFile(out)); });
        menu.addAction(Icons::get("folder"), tr("Show in folder"), this, [out] { revealInExplorer(out); });
    }
    if (job && job->state() == ConversionJob::State::Failed)
        menu.addAction(tr("Show error"), this, [this, job] { Q_EMIT statusMessage(job->result().error); });
    const QString src = item->data(0, Qt::UserRole).toString();
    menu.addAction(tr("Show source in folder"), this, [src] { revealInExplorer(src); });
    if (!job || job->isFinished())
        menu.addAction(Icons::get("trash"), tr("Remove from list"), this, [this, item] {
            delete item;
            refreshTargets();
        });
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void BatchPage::updateSummary()
{
    int total = m_table->topLevelItemCount(), done = 0, failed = 0, running = 0;
    for (int i = 0; i < total; ++i) {
        auto* job = m_table->topLevelItem(i)->data(0, Qt::UserRole + 2).value<ConversionJob*>();
        if (!job)
            continue;
        switch (job->state()) {
        case ConversionJob::State::Succeeded: ++done; break;
        case ConversionJob::State::Failed:
        case ConversionJob::State::Cancelled: ++failed; break;
        default: ++running; break;
        }
    }
    m_summary->setText(tr("%1 files · %2 done · %3 running · %4 failed").arg(total).arg(done).arg(running).arg(failed));
    m_start->setEnabled(total > 0 && !m_setup->targetFormat().isEmpty());
    m_cancelAll->setEnabled(running > 0);
    m_retry->setEnabled(failed > 0);
    m_openFolder->setEnabled(!m_lastOutputDir.isEmpty());
}

} // namespace folvio
