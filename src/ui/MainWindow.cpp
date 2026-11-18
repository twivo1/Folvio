#include "ui/MainWindow.h"

#include "core/files/FileFormat.h"
#include "core/jobs/ConversionJob.h"
#include "core/persistence/Database.h"
#include "editor/pdf_native/PdfWorkspace.h"
#include "editor/rich/RichDocumentIO.h"
#include "editor/rich/RichWorkspace.h"
#include "ui/pages/ConvertPages.h"
#include "ui/pages/HomePage.h"
#include "ui/pages/OtherPages.h"
#include "ui/themes/Theme.h"
#include "ui/widgets/CommonWidgets.h"
#include "ui/widgets/Icons.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QScreen>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSvgRenderer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace folvio {

// ===========================================================================
// WorkspacePage

WorkspacePage::WorkspacePage(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget(this);
    root->addWidget(m_stack);

    auto* empty = new QWidget(m_stack);
    auto* el = new QVBoxLayout(empty);
    el->setContentsMargins(28, 22, 28, 22);
    el->setSpacing(12);
    auto* title = new QLabel(tr("Auto Change"), empty);
    title->setObjectName("PageTitle");
    auto* sub = new QLabel(tr("Open a PDF to change its text directly in place — the rest of the document stays exactly as it "
                              "was. Word, OpenDocument, RTF, Markdown, HTML and text files open in the document editor."),
                           empty);
    sub->setObjectName("PageSubtitle");
    sub->setWordWrap(true);
    m_drop = new DropZone(tr("Drop a PDF or document to edit"), tr("PDF · DOCX · DOC · ODT · RTF · TXT · Markdown · HTML"), empty);
    m_drop->setFileFilter(tr("Editable documents (*.pdf *.docx *.doc *.odt *.rtf *.txt *.md *.markdown *.html *.htm)"));
    el->addWidget(title);
    el->addWidget(sub);
    el->addWidget(m_drop, 1);
    m_stack->addWidget(empty);

    m_pdf = new PdfWorkspace(m_stack);
    m_rich = new RichWorkspace(m_stack);
    m_stack->addWidget(m_pdf);
    m_stack->addWidget(m_rich);

    connect(m_drop, &DropZone::filesDropped, this, [this](const QStringList& paths) { openDocument(paths.first()); });
    connect(m_pdf, &PdfWorkspace::statusChanged, this, &WorkspacePage::statusChanged);
    connect(m_rich, &RichWorkspace::statusChanged, this, &WorkspacePage::statusChanged);
    connect(m_pdf, &PdfWorkspace::busy, this, &WorkspacePage::busy);
    connect(m_rich, &RichWorkspace::busy, this, &WorkspacePage::busy);
    connect(m_pdf, &PdfWorkspace::documentClosed, this, [this] {
        m_stack->setCurrentIndex(0);
        Q_EMIT statusChanged();
    });
    connect(m_rich, &RichWorkspace::documentClosed, this, [this] {
        if (m_stack->currentWidget() == m_rich)
            m_stack->setCurrentIndex(0);
        Q_EMIT statusChanged();
    });
    connect(m_rich, &RichWorkspace::opened, this, [this] {
        m_stack->setCurrentWidget(m_rich);
        Q_EMIT documentOpened(m_rich->filePath());
        Q_EMIT statusChanged();
    });
    connect(m_rich, &RichWorkspace::failed, this, [this](const QString& err) {
        QMessageBox::warning(this, tr("Cannot open document"), err);
    });
    connect(m_pdf, &PdfWorkspace::reflowRequested, this, [this](const QString& path) {
        const auto answer = QMessageBox::question(
            this, tr("Edit as a reflowable document"),
            tr("Folvio will reconstruct an editable document from this PDF (using Microsoft Word if installed, otherwise "
               "LibreOffice). You can then edit it like a Word document.\n\nThis is not lossless: page layout, fonts and "
               "spacing will change, and saving writes a newly generated PDF. For small corrections, in-place editing "
               "keeps the original appearance.\n\nContinue?"));
        if (answer == QMessageBox::Yes)
            openDocument(path, true);
    });
}

bool WorkspacePage::openDocument(const QString& path, bool reflow)
{
    const DetectedFile d = FormatDetector::detect(path);
    if (!d.ok()) {
        QMessageBox::warning(this, tr("Cannot open"), d.error.isEmpty() ? tr("Unknown file type.") : d.error);
        return false;
    }
    if (d.formatId == "pdf" && !reflow) {
        if (m_stack->currentWidget() == m_rich && !m_rich->maybeClose())
            return false;
        QString err;
        if (!m_pdf->openFile(path, &err)) {
            if (!err.isEmpty())
                QMessageBox::warning(this, tr("Cannot open PDF"), err);
            return false;
        }
        if (m_rich->hasDocument())
            m_rich->closeDocument();
        m_stack->setCurrentWidget(m_pdf);
        Q_EMIT documentOpened(path);
        Q_EMIT statusChanged();
        return true;
    }
    if (!RichDocumentIO::canEdit(d.formatId)) {
        QMessageBox::information(this, tr("Not editable"), tr("%1 files cannot be edited. You can convert them instead.")
                                                               .arg(d.formatId.toUpper()));
        return false;
    }
    if (m_pdf->hasDocument()) {
        if (!m_pdf->maybeClose())
            return false;
        m_pdf->closeDocument();
    }
    m_rich->openFile(path, d.formatId);
    return true;
}

bool WorkspacePage::maybeClose()
{
    return m_pdf->maybeClose() && m_rich->maybeClose();
}

bool WorkspacePage::hasDocument() const
{
    return m_pdf->hasDocument() || m_rich->hasDocument();
}

QString WorkspacePage::statusFormat() const
{
    if (m_stack->currentWidget() == m_pdf && m_pdf->hasDocument())
        return tr("PDF · in-place editing");
    if (m_stack->currentWidget() == m_rich && m_rich->hasDocument())
        return tr("%1 · document editor").arg(m_rich->format().toUpper());
    return {};
}

int WorkspacePage::pageCount() const
{
    return m_stack->currentWidget() == m_pdf ? m_pdf->pageCount() : m_rich->pageCount();
}

int WorkspacePage::currentPage() const
{
    return m_stack->currentWidget() == m_pdf ? m_pdf->currentPage() : m_rich->currentPage();
}

double WorkspacePage::zoom() const
{
    return m_stack->currentWidget() == m_pdf ? m_pdf->zoom() : m_rich->zoom();
}

QString WorkspacePage::saveStatus() const
{
    return m_stack->currentWidget() == m_pdf ? m_pdf->saveStatus() : m_rich->saveStatus();
}

// ===========================================================================
// MainWindow

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Folvio"));
    setWindowIcon(Icons::appIcon());
    setAcceptDrops(true);
    setMinimumSize(980, 640);
    m_queue = new JobQueue(this);
    m_queue->setMaxConcurrent(Database::instance().setting("convert.parallel", m_queue->maxConcurrent()).toInt());

    auto* central = new QWidget(this);
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(buildSidebar());

    m_pages = new QStackedWidget(central);
    m_home = new HomePage(m_pages);
    m_convert = new ConvertPage(m_queue, m_pages);
    m_workspace = new WorkspacePage(m_pages);
    m_batch = new BatchPage(m_queue, m_pages);
    m_recent = new RecentPage(m_pages);
    m_settings = new SettingsPage(m_pages);
    for (QWidget* w : std::initializer_list<QWidget*>{m_home, m_convert, m_workspace, m_batch, m_recent, m_settings})
        m_pages->addWidget(w);
    layout->addWidget(m_pages, 1);
    setCentralWidget(central);
    buildStatusBar();

    connect(m_home, &HomePage::filesDropped, this, &MainWindow::openFiles);
    connect(m_home, &HomePage::openForEditing, this, &MainWindow::openForEditing);
    connect(m_home, &HomePage::openForConversion, this, &MainWindow::openForConversion);
    connect(m_recent, &RecentPage::openForEditing, this, &MainWindow::openForEditing);
    connect(m_recent, &RecentPage::openForConversion, this, &MainWindow::openForConversion);
    connect(m_convert, &ConvertPage::multipleFilesDropped, this, [this](const QStringList& p) {
        m_batch->addFiles(p);
        showPage(Batch);
    });
    connect(m_convert, &ConvertPage::openForEditing, this, &MainWindow::openForEditing);
    connect(m_convert, &ConvertPage::statusMessage, m_statusMessage, &QLabel::setText);
    connect(m_batch, &BatchPage::statusMessage, m_statusMessage, &QLabel::setText);
    connect(m_workspace, &WorkspacePage::statusChanged, this, &MainWindow::updateStatus);
    connect(m_workspace, &WorkspacePage::documentOpened, this, [this](const QString& path) {
        setWindowTitle(QStringLiteral("%1 — Folvio").arg(QFileInfo(path).fileName()));
        showPage(AutoChange);
    });
    connect(m_workspace, &WorkspacePage::busy, this, [this](const QString& msg, int pct) {
        if (pct >= 100 || msg.isEmpty()) {
            m_statusProgress->hide();
            m_statusMessage->setText(msg);
            return;
        }
        m_statusProgress->show();
        m_statusProgress->setRange(0, pct < 0 ? 0 : 100);
        if (pct >= 0)
            m_statusProgress->setValue(pct);
        m_statusMessage->setText(msg);
    });
    connect(m_queue, &JobQueue::jobAdded, this, [this](ConversionJob* job) {
        connect(job, &ConversionJob::stateChanged, this, &MainWindow::updateStatus);
        updateStatus();
    });
    connect(m_queue, &JobQueue::jobFinished, this, [this] {
        updateStatus();
        m_recent->refresh();
    });
    connect(m_settings, &SettingsPage::themeChanged, this, [this] {
        m_home->refresh();
        m_recent->refresh();
    });
    connect(&Theme::instance(), &Theme::changed, this, [this] {
        QSvgRenderer r(Theme::instance().isDark() ? QStringLiteral(":/branding/mark-dark-bg.svg")
                                                  : QStringLiteral(":/branding/mark-light-bg.svg"));
        Q_UNUSED(r)
        if (m_brand)
            m_brand->update();
    });

    showPage(Home);
    restoreGeometryFromSettings();
    updateStatus();
    // Start with focus in the page content, not on a navigation button.
    QTimer::singleShot(0, this, [this] {
        if (QWidget* w = m_pages->currentWidget())
            w->setFocus(Qt::OtherFocusReason);
    });
}

MainWindow::~MainWindow() = default;

QWidget* MainWindow::buildSidebar()
{
    auto* side = new QWidget(this);
    side->setObjectName("Sidebar");
    side->setFixedWidth(196);
    auto* l = new QVBoxLayout(side);
    l->setContentsMargins(10, 14, 10, 10);
    l->setSpacing(2);

    m_brand = new QLabel(side);
    auto paintBrand = [this] {
        const qreal dpr = devicePixelRatioF();
        QPixmap pm(QSize(150, 34) * dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(dpr, dpr);
        QSvgRenderer mark(Theme::instance().isDark() ? QStringLiteral(":/branding/mark-dark-bg.svg")
                                                     : QStringLiteral(":/branding/mark-light-bg.svg"));
        mark.render(&p, QRectF(4, 3, 26, 28.7));
        QFont f(QStringLiteral("Segoe UI"));
        f.setPixelSize(19);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(tc().text);
        p.drawText(QRectF(38, 0, 112, 34), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Folvio"));
        p.end();
        pm.setDevicePixelRatio(dpr);
        m_brand->setPixmap(pm);
    };
    paintBrand();
    connect(&Theme::instance(), &Theme::changed, this, paintBrand);
    l->addWidget(m_brand);
    l->addSpacing(16);

    m_nav = new QButtonGroup(this);
    m_nav->setExclusive(true);
    const QList<std::tuple<Page, QString, QString, QString>> items = {
        {Home, "home", tr("Home"), "Ctrl+1"},
        {Convert, "convert", tr("Convert"), "Ctrl+2"},
        {AutoChange, "edit", tr("Auto Change"), "Ctrl+3"},
        {Batch, "batch", tr("Batch"), "Ctrl+4"},
        {Recent, "recent", tr("Recent Files"), "Ctrl+5"},
        {Settings, "settings", tr("Settings"), "Ctrl+,"},
    };
    for (const auto& [page, icon, text, keys] : items) {
        auto* b = new QToolButton(side);
        b->setIcon(Icons::get(icon));
        b->setIconSize(QSize(18, 18));
        b->setText("  " + text);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setCheckable(true);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setToolTip(QStringLiteral("%1 (%2)").arg(text, keys));
        b->setShortcut(QKeySequence(keys));
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::TabFocus);
        m_nav->addButton(b, page);
        l->addWidget(b);
        if (page == Batch)
            l->addSpacing(10);
    }
    connect(m_nav, &QButtonGroup::idClicked, this, [this](int id) { showPage(Page(id)); });
    l->addStretch();
    auto* footer = new QLabel(tr("Version %1\nOffline · no telemetry").arg(QApplication::applicationVersion()), side);
    footer->setObjectName("SidebarFooter");
    l->addWidget(footer);
    return side;
}

void MainWindow::buildStatusBar()
{
    auto* sb = statusBar();
    sb->setSizeGripEnabled(true);
    m_statusMessage = new QLabel(sb);
    m_statusFormat = new QLabel(sb);
    m_statusPages = new QLabel(sb);
    m_statusSave = new QLabel(sb);
    m_statusZoom = new QLabel(sb);
    m_statusJobs = new QLabel(sb);
    m_statusProgress = new QProgressBar(sb);
    m_statusProgress->setMaximumWidth(160);
    m_statusProgress->setTextVisible(false);
    m_statusProgress->hide();
    sb->addWidget(m_statusMessage, 1);
    sb->addPermanentWidget(m_statusProgress);
    sb->addPermanentWidget(m_statusJobs);
    sb->addPermanentWidget(m_statusFormat);
    sb->addPermanentWidget(m_statusPages);
    sb->addPermanentWidget(m_statusSave);
    sb->addPermanentWidget(m_statusZoom);
}

void MainWindow::updateStatus()
{
    const bool inWorkspace = m_pages->currentWidget() == m_workspace && m_workspace->hasDocument();
    m_statusFormat->setText(inWorkspace ? m_workspace->statusFormat() : QString());
    m_statusPages->setText(inWorkspace ? tr("Page %1 of %2").arg(m_workspace->currentPage() + 1).arg(m_workspace->pageCount()) : QString());
    m_statusSave->setText(inWorkspace ? m_workspace->saveStatus() : QString());
    m_statusZoom->setText(inWorkspace ? tr("%1%").arg(int(std::round(m_workspace->zoom() * 100))) : QString());
    const int active = m_queue->activeCount();
    m_statusJobs->setText(active > 0 ? tr("%n conversion(s) running", nullptr, active) : QString());
    for (QLabel* l : {m_statusFormat, m_statusPages, m_statusSave, m_statusZoom, m_statusJobs})
        l->setVisible(!l->text().isEmpty());
    if (inWorkspace) {
        const bool modified = m_workspace->saveStatus() == tr("Unsaved changes");
        QString name = m_workspace->pdf()->hasDocument() ? QFileInfo(m_workspace->pdf()->filePath()).fileName()
                                                         : QFileInfo(m_workspace->rich()->filePath()).fileName();
        setWindowTitle(QStringLiteral("%1%2 — Folvio").arg(modified ? "• " : "", name));
    } else if (!m_workspace->hasDocument()) {
        setWindowTitle(QStringLiteral("Folvio"));
    }
}

void MainWindow::showPage(Page page)
{
    m_pages->setCurrentIndex(page);
    if (auto* b = m_nav->button(page))
        b->setChecked(true);
    if (page == Home)
        m_home->refresh();
    if (page == Recent)
        m_recent->refresh();
    updateStatus();
}

void MainWindow::openFiles(const QStringList& paths)
{
    if (paths.isEmpty())
        return;
    if (paths.size() > 1) {
        m_batch->addFiles(paths);
        showPage(Batch);
        return;
    }
    const DetectedFile d = FormatDetector::detect(paths.first());
    if (d.ok() && RichDocumentIO::canEdit(d.formatId) && d.formatId != "txt" && d.formatId != "html" && d.formatId != "md")
        openForEditing(paths.first());
    else
        openForConversion(paths.first());
}

void MainWindow::openForEditing(const QString& path)
{
    showPage(AutoChange);
    if (!m_workspace->openDocument(path) && !m_workspace->hasDocument())
        showPage(AutoChange);
}

void MainWindow::demoEditFirstParagraph()
{
    if (m_workspace->pdf()->hasDocument())
        m_workspace->pdf()->demoEditFirstParagraph();
}

void MainWindow::openForConversion(const QString& path)
{
    m_convert->setFile(path);
    showPage(Convert);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e)
{
    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isFile())
            paths << u.toLocalFile();
    if (m_pages->currentWidget() == m_batch)
        m_batch->addFiles(paths);
    else
        openFiles(paths);
}

void MainWindow::restoreGeometryFromSettings()
{
    const QByteArray geo = QByteArray::fromBase64(Database::instance().setting("window.geometry").toByteArray());
    if (geo.isEmpty() || !restoreGeometry(geo)) {
        const QRect avail = screen() ? screen()->availableGeometry() : QRect(0, 0, 1400, 900);
        resize(std::min(1360, int(avail.width() * 0.88)), std::min(880, int(avail.height() * 0.88)));
        move(avail.center() - rect().center());
    }
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (!m_workspace->maybeClose()) {
        e->ignore();
        return;
    }
    if (m_queue->activeCount() > 0) {
        const auto r = QMessageBox::question(this, tr("Conversions running"),
                                             tr("%n conversion(s) are still running. Cancel them and quit?", nullptr, m_queue->activeCount()));
        if (r != QMessageBox::Yes) {
            e->ignore();
            return;
        }
    }
    m_statusMessage->setText(tr("Stopping…"));
    m_queue->shutdown(15000); // cancels jobs and kills external process trees
    Database::instance().setSetting("window.geometry", QString::fromLatin1(saveGeometry().toBase64()));
    e->accept();
}

} // namespace folvio
