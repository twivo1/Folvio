#include "editor/rich/RichWorkspace.h"

#include "core/conversions/OutputValidator.h"
#include "core/files/PathUtils.h"
#include "core/files/SafeFileWriter.h"
#include "core/persistence/Database.h"
#include "ui/themes/Theme.h"
#include "ui/widgets/CommonWidgets.h"
#include "ui/widgets/Icons.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QShortcut>
#include <QSpinBox>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTextList>
#include <QTextTable>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>

namespace folvio {

// ---------------------------------------------------------------------------
// PagedDocumentView

PagedDocumentView::PagedDocumentView(QWidget* parent) : QGraphicsView(parent)
{
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    setFrameShape(QFrame::NoFrame);
    setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);

    m_edit = new QTextEdit;
    m_edit->setFrameShape(QFrame::NoFrame);
    m_edit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_edit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_edit->setLineWrapMode(QTextEdit::FixedPixelWidth);
    m_edit->setLineWrapColumnOrWidth(int(kPageWidth));
    m_edit->setFixedWidth(int(std::ceil(kPageWidth)));
    m_edit->setAcceptRichText(true);
    m_edit->setAutoFormatting(QTextEdit::AutoBulletList);
    QPalette pal = m_edit->palette();
    pal.setColor(QPalette::Base, Qt::white);
    pal.setColor(QPalette::Text, QColor("#111111"));
    pal.setColor(QPalette::Highlight, QColor("#9ED5C8"));
    pal.setColor(QPalette::HighlightedText, QColor("#111111"));
    m_edit->setPalette(pal);
    m_edit->setStyleSheet("QTextEdit { background: white; color: #111; border: none; }");
    m_proxy = m_scene->addWidget(m_edit);
    m_proxy->setPos(0, 0);
    applyPageSetup();
    connect(m_edit->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this](const QSizeF&) { updateHeight(); });
    connect(m_edit, &QTextEdit::cursorPositionChanged, this, &PagedDocumentView::ensureCursorVisible);
}

void PagedDocumentView::applyPageSetup()
{
    QTextDocument* doc = m_edit->document();
    doc->setDocumentMargin(0);
    QTextFrameFormat ff = doc->rootFrame()->frameFormat();
    ff.setMargin(kMargin);
    doc->rootFrame()->setFrameFormat(ff);
    // Finite page height turns on QTextDocumentLayout pagination.
    doc->setPageSize(QSizeF(kPageWidth, kPageHeight));
    if (doc->defaultFont().pointSizeF() <= 0 || doc->defaultFont().family().isEmpty()) {
        QFont f(QStringLiteral("Calibri"));
        f.setPointSizeF(11);
        doc->setDefaultFont(f);
    }
    updateHeight();
}

void PagedDocumentView::updateHeight()
{
    QTextDocument* doc = m_edit->document();
    if (doc->pageSize().height() <= 0 || doc->pageSize().height() > 1e6)
        doc->setPageSize(QSizeF(kPageWidth, kPageHeight)); // QTextEdit may reset it on relayout
    const int pages = std::max(1, doc->pageCount());
    const int h = int(std::ceil(pages * kPageHeight));
    if (m_edit->height() != h)
        m_edit->setFixedHeight(h);
    m_scene->setSceneRect(-40, -30, kPageWidth + 80, h + 60);
    viewport()->update();
}

int PagedDocumentView::pageCount() const
{
    return std::max(1, m_edit->document()->pageCount());
}

int PagedDocumentView::currentPage() const
{
    const QPointF center = mapToScene(viewport()->rect().center());
    return std::clamp(int(center.y() / kPageHeight), 0, pageCount() - 1);
}

void PagedDocumentView::goToPage(int page)
{
    page = std::clamp(page, 0, pageCount() - 1);
    centerOn(kPageWidth / 2, page * kPageHeight + viewport()->height() / (2 * m_zoom) - 20);
}

void PagedDocumentView::setZoom(double z)
{
    z = std::clamp(z, 0.3, 4.0);
    m_zoom = z;
    setTransform(QTransform::fromScale(z, z));
    Q_EMIT zoomChanged(z);
}

void PagedDocumentView::ensureCursorVisible()
{
    const QRect r = m_edit->cursorRect();
    ensureVisible(QRectF(r).adjusted(-20, -40, 20, 40), 10, 10);
}

void PagedDocumentView::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        setZoom(m_zoom * (e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1));
        e->accept();
        return;
    }
    QGraphicsView::wheelEvent(e);
}

void PagedDocumentView::drawBackground(QPainter* painter, const QRectF& rect)
{
    painter->fillRect(rect, tc().sunken);
    const int pages = pageCount();
    for (int i = 0; i < pages; ++i) {
        const QRectF page(0, i * kPageHeight, kPageWidth, kPageHeight);
        painter->fillRect(page.translated(0, 2).adjusted(-1, 0, 1, 1), QColor(0, 0, 0, Theme::instance().isDark() ? 110 : 26));
    }
}

void PagedDocumentView::drawForeground(QPainter* painter, const QRectF& rect)
{
    // Page breaks drawn on top of the (contiguous) editor surface; they fall
    // into the bottom/top page margins, so they never cover text.
    const int pages = pageCount();
    const QColor gap = tc().sunken;
    for (int i = 1; i < pages; ++i) {
        const double y = i * kPageHeight;
        const QRectF band(-40, y - 5, kPageWidth + 80, 10);
        if (!band.intersects(rect))
            continue;
        painter->fillRect(band, gap);
        painter->setPen(QPen(tc().border, 0));
        painter->drawLine(QPointF(0, y - 5), QPointF(kPageWidth, y - 5));
        painter->drawLine(QPointF(0, y + 5), QPointF(kPageWidth, y + 5));
    }
    painter->setPen(tc().textMuted);
    QFont f = painter->font();
    f.setPointSizeF(7.5);
    painter->setFont(f);
    for (int i = 0; i < pages; ++i)
        painter->drawText(QRectF(kPageWidth + 6, i * kPageHeight + kPageHeight - 24, 40, 16), Qt::AlignLeft, QString::number(i + 1));
}

// ---------------------------------------------------------------------------
// RichWorkspace

RichWorkspace::RichWorkspace(QWidget* parent) : QWidget(parent)
{
    buildUi();
}

RichWorkspace::~RichWorkspace()
{
    if (m_cancel)
        m_cancel->store(true);
    if (m_importWatcher)
        m_importWatcher->waitForFinished();
    if (!m_workDir.isEmpty())
        QDir(m_workDir).removeRecursively();
}

void RichWorkspace::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Row 1: document commands
    auto* bar = new QWidget(this);
    bar->setObjectName("Toolbar");
    auto* l = new QHBoxLayout(bar);
    l->setContentsMargins(10, 4, 10, 4);
    l->setSpacing(4);
    auto* icon = new QLabel(bar);
    icon->setPixmap(Icons::pixmap("edit", 18, Icons::Tone::Accent, devicePixelRatioF()));
    m_title = new QLabel(bar);
    m_title->setObjectName("DocTitle");
    m_undo = makeToolButton("undo", tr("Undo (Ctrl+Z)"), bar);
    m_redo = makeToolButton("redo", tr("Redo (Ctrl+Y)"), bar);
    auto* findBtn = makeToolButton("search", tr("Find and replace (Ctrl+F / Ctrl+H)"), bar);
    auto* zoomOut = makeToolButton("zoom-out", tr("Zoom out"), bar);
    auto* zoomIn = makeToolButton("zoom-in", tr("Zoom in"), bar);
    auto* fit = makeToolButton("fit-width", tr("Fit width"), bar);
    m_saveBtn = makeToolButton("save", tr("Save to the original file (Ctrl+S)"), bar);
    m_saveBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_saveBtn->setText(tr("Save"));
    auto* saveAsBtn = makeToolButton("save-as", tr("Save as… (Ctrl+Shift+S)"), bar);
    saveAsBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    saveAsBtn->setText(tr("Save As"));
    auto* exportBtn = makeToolButton("export", tr("Export"), bar);
    exportBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    exportBtn->setText(tr("Export"));
    exportBtn->setPopupMode(QToolButton::InstantPopup);
    auto* em = new QMenu(exportBtn);
    em->addAction(tr("PDF (.pdf)…"), this, [this] { exportAs("pdf"); });
    em->addAction(tr("Word document (.docx)…"), this, [this] { exportAs("docx"); });
    em->addAction(tr("OpenDocument (.odt)…"), this, [this] { exportAs("odt"); });
    em->addAction(tr("HTML (.html)…"), this, [this] { exportAs("html"); });
    em->addAction(tr("Markdown (.md)…"), this, [this] { exportAs("md"); });
    em->addAction(tr("Plain text (.txt)…"), this, [this] { exportAs("txt"); });
    exportBtn->setMenu(em);
    auto* closeBtn = makeToolButton("close", tr("Close document"), bar);
    l->addWidget(icon);
    l->addWidget(m_title);
    l->addSpacing(8);
    l->addWidget(makeSeparator(bar));
    l->addWidget(m_undo);
    l->addWidget(m_redo);
    l->addWidget(makeSeparator(bar));
    l->addWidget(findBtn);
    l->addWidget(zoomOut);
    l->addWidget(zoomIn);
    l->addWidget(fit);
    l->addStretch();
    l->addWidget(m_saveBtn);
    l->addWidget(saveAsBtn);
    l->addWidget(exportBtn);
    l->addWidget(makeSeparator(bar));
    l->addWidget(closeBtn);
    root->addWidget(bar);

    // Row 2: formatting
    auto* fbar = new QWidget(this);
    fbar->setObjectName("Toolbar");
    auto* f = new QHBoxLayout(fbar);
    f->setContentsMargins(10, 3, 10, 3);
    f->setSpacing(3);
    m_styleBox = new QComboBox(fbar);
    m_styleBox->addItems({tr("Normal"), tr("Heading 1"), tr("Heading 2"), tr("Heading 3")});
    m_fontBox = new QFontComboBox(fbar);
    m_fontBox->setMaximumWidth(180);
    m_sizeBox = new QDoubleSpinBox(fbar);
    m_sizeBox->setRange(4, 144);
    m_sizeBox->setDecimals(1);
    m_sizeBox->setSuffix(" pt");
    m_bold = makeToolButton("bold", tr("Bold (Ctrl+B)"), fbar, true);
    m_italic = makeToolButton("italic", tr("Italic (Ctrl+I)"), fbar, true);
    m_underline = makeToolButton("underline", tr("Underline (Ctrl+U)"), fbar, true);
    m_color = makeToolButton("text-color", tr("Text colour"), fbar);
    m_alignL = makeToolButton("align-left", tr("Align left (Ctrl+L)"), fbar, true);
    m_alignC = makeToolButton("align-center", tr("Center (Ctrl+E)"), fbar, true);
    m_alignR = makeToolButton("align-right", tr("Align right (Ctrl+R)"), fbar, true);
    m_alignJ = makeToolButton("align-justify", tr("Justify (Ctrl+J)"), fbar, true);
    for (auto* b : {m_alignL, m_alignC, m_alignR, m_alignJ})
        b->setAutoExclusive(true);
    m_bullets = makeToolButton("list-bullet", tr("Bulleted list"), fbar, true);
    m_numbers = makeToolButton("list-number", tr("Numbered list"), fbar, true);
    auto* outdent = makeToolButton("outdent", tr("Decrease indent"), fbar);
    auto* indent = makeToolButton("indent", tr("Increase indent"), fbar);
    auto* spacing = makeToolButton("line-spacing", tr("Line and paragraph spacing"), fbar);
    spacing->setPopupMode(QToolButton::InstantPopup);
    auto* sm = new QMenu(spacing);
    for (int pct : {100, 115, 150, 200})
        sm->addAction(QString::number(pct / 100.0, 'f', pct % 100 ? 2 : 1).replace(".00", ".0"), this,
                      [this, pct] { setLineSpacing(pct); });
    sm->addSeparator();
    sm->addAction(tr("Add space after paragraph"), this, [this] {
        QTextBlockFormat bf;
        bf.setBottomMargin(8);
        m_edit->textCursor().mergeBlockFormat(bf);
    });
    sm->addAction(tr("Remove space after paragraph"), this, [this] {
        QTextBlockFormat bf;
        bf.setBottomMargin(0);
        m_edit->textCursor().mergeBlockFormat(bf);
    });
    spacing->setMenu(sm);
    auto* image = makeToolButton("image", tr("Insert image"), fbar);
    auto* table = makeToolButton("table", tr("Table"), fbar);
    table->setPopupMode(QToolButton::InstantPopup);
    auto* tm = new QMenu(table);
    tm->addAction(tr("Insert table…"), this, &RichWorkspace::insertTable);
    tm->addSeparator();
    tm->addAction(tr("Insert row above"), this, [this] { tableOperation("rowAbove"); });
    tm->addAction(tr("Insert row below"), this, [this] { tableOperation("rowBelow"); });
    tm->addAction(tr("Insert column left"), this, [this] { tableOperation("colLeft"); });
    tm->addAction(tr("Insert column right"), this, [this] { tableOperation("colRight"); });
    tm->addSeparator();
    tm->addAction(tr("Delete row"), this, [this] { tableOperation("deleteRow"); });
    tm->addAction(tr("Delete column"), this, [this] { tableOperation("deleteCol"); });
    tm->addAction(tr("Merge selected cells"), this, [this] { tableOperation("merge"); });
    table->setMenu(tm);

    f->addWidget(m_styleBox);
    f->addWidget(m_fontBox);
    f->addWidget(m_sizeBox);
    f->addWidget(makeSeparator(fbar));
    for (QWidget* w : std::initializer_list<QWidget*>{m_bold, m_italic, m_underline, m_color})
        f->addWidget(w);
    f->addWidget(makeSeparator(fbar));
    for (QWidget* w : std::initializer_list<QWidget*>{m_alignL, m_alignC, m_alignR, m_alignJ})
        f->addWidget(w);
    f->addWidget(makeSeparator(fbar));
    for (QWidget* w : std::initializer_list<QWidget*>{m_bullets, m_numbers, outdent, indent, spacing})
        f->addWidget(w);
    f->addWidget(makeSeparator(fbar));
    f->addWidget(image);
    f->addWidget(table);
    f->addStretch();
    root->addWidget(fbar);

    auto* bannerWrap = new QWidget(this);
    auto* bl = new QVBoxLayout(bannerWrap);
    bl->setContentsMargins(10, 6, 10, 0);
    m_banner = new Banner(bannerWrap);
    bl->addWidget(m_banner);
    root->addWidget(bannerWrap);

    // Find bar
    m_findBar = new QWidget(this);
    m_findBar->setObjectName("Toolbar");
    auto* fl = new QVBoxLayout(m_findBar);
    fl->setContentsMargins(10, 6, 10, 6);
    auto* fr = new QHBoxLayout;
    m_findEdit = new QLineEdit(m_findBar);
    m_findEdit->setPlaceholderText(tr("Find"));
    m_findCase = new QCheckBox(tr("Match case"), m_findBar);
    auto* prev = makeToolButton("chevron-up", tr("Previous"), m_findBar);
    auto* next = makeToolButton("chevron-down", tr("Next"), m_findBar);
    m_findStatus = new QLabel(m_findBar);
    m_findStatus->setProperty("muted", true);
    auto* closeFind = makeToolButton("close", tr("Close"), m_findBar);
    fr->addWidget(m_findEdit, 1);
    fr->addWidget(m_findCase);
    fr->addWidget(prev);
    fr->addWidget(next);
    fr->addWidget(m_findStatus);
    fr->addWidget(closeFind);
    m_replaceRow = new QWidget(m_findBar);
    auto* rr = new QHBoxLayout(m_replaceRow);
    rr->setContentsMargins(0, 0, 0, 0);
    m_replaceEdit = new QLineEdit(m_replaceRow);
    m_replaceEdit->setPlaceholderText(tr("Replace with"));
    auto* rep = new QPushButton(tr("Replace"), m_replaceRow);
    auto* repAll = new QPushButton(tr("Replace all"), m_replaceRow);
    rr->addWidget(m_replaceEdit, 1);
    rr->addWidget(rep);
    rr->addWidget(repAll);
    fl->addLayout(fr);
    fl->addWidget(m_replaceRow);
    m_findBar->hide();
    root->addWidget(m_findBar);

    m_view = new PagedDocumentView(this);
    m_edit = m_view->editor();
    root->addWidget(m_view, 1);

    // Wiring
    connect(m_undo, &QToolButton::clicked, m_edit, &QTextEdit::undo);
    connect(m_redo, &QToolButton::clicked, m_edit, &QTextEdit::redo);
    connect(m_edit->document(), &QTextDocument::undoAvailable, m_undo, &QToolButton::setEnabled);
    connect(m_edit->document(), &QTextDocument::redoAvailable, m_redo, &QToolButton::setEnabled);
    connect(m_edit->document(), &QTextDocument::modificationChanged, this, [this](bool m) {
        m_saveBtn->setEnabled(m);
        Q_EMIT statusChanged();
    });
    connect(m_edit, &QTextEdit::currentCharFormatChanged, this, &RichWorkspace::syncFormatControls);
    connect(m_edit, &QTextEdit::cursorPositionChanged, this, [this] {
        syncFormatControls();
        Q_EMIT statusChanged();
    });
    connect(findBtn, &QToolButton::clicked, this, [this] { showFind(true); });
    connect(zoomOut, &QToolButton::clicked, this, [this] { m_view->setZoom(m_view->zoom() / 1.1); });
    connect(zoomIn, &QToolButton::clicked, this, [this] { m_view->setZoom(m_view->zoom() * 1.1); });
    connect(fit, &QToolButton::clicked, this, [this] {
        m_view->setZoom((m_view->viewport()->width() - 60) / (PagedDocumentView::kPageWidth + 80));
    });
    connect(m_view, &PagedDocumentView::zoomChanged, this, [this] { Q_EMIT statusChanged(); });
    connect(m_saveBtn, &QToolButton::clicked, this, &RichWorkspace::save);
    connect(saveAsBtn, &QToolButton::clicked, this, &RichWorkspace::saveAs);
    connect(closeBtn, &QToolButton::clicked, this, [this] {
        if (maybeClose())
            closeDocument();
    });
    connect(m_styleBox, &QComboBox::activated, this, &RichWorkspace::setParagraphStyle);
    connect(m_fontBox, &QFontComboBox::currentFontChanged, this, [this](const QFont& font) {
        if (m_syncing)
            return;
        QTextCharFormat fmt;
        fmt.setFontFamilies({font.family()});
        mergeFormat(fmt);
    });
    connect(m_sizeBox, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (m_syncing)
            return;
        QTextCharFormat fmt;
        fmt.setFontPointSize(v);
        mergeFormat(fmt);
    });
    connect(m_bold, &QToolButton::clicked, this, [this](bool on) {
        QTextCharFormat fmt;
        fmt.setFontWeight(on ? QFont::Bold : QFont::Normal);
        mergeFormat(fmt);
    });
    connect(m_italic, &QToolButton::clicked, this, [this](bool on) {
        QTextCharFormat fmt;
        fmt.setFontItalic(on);
        mergeFormat(fmt);
    });
    connect(m_underline, &QToolButton::clicked, this, [this](bool on) {
        QTextCharFormat fmt;
        fmt.setFontUnderline(on);
        mergeFormat(fmt);
    });
    m_bold->setShortcut(QKeySequence::Bold);
    m_italic->setShortcut(QKeySequence::Italic);
    m_underline->setShortcut(QKeySequence::Underline);
    connect(m_color, &QToolButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(m_edit->textColor(), this, tr("Text colour"));
        if (c.isValid()) {
            QTextCharFormat fmt;
            fmt.setForeground(c);
            mergeFormat(fmt);
        }
    });
    connect(m_alignL, &QToolButton::clicked, this, [this] { setAlignment(Qt::AlignLeft | Qt::AlignAbsolute); });
    connect(m_alignC, &QToolButton::clicked, this, [this] { setAlignment(Qt::AlignHCenter); });
    connect(m_alignR, &QToolButton::clicked, this, [this] { setAlignment(Qt::AlignRight | Qt::AlignAbsolute); });
    connect(m_alignJ, &QToolButton::clicked, this, [this] { setAlignment(Qt::AlignJustify); });
    connect(m_bullets, &QToolButton::clicked, this, [this] { toggleList(false); });
    connect(m_numbers, &QToolButton::clicked, this, [this] { toggleList(true); });
    connect(indent, &QToolButton::clicked, this, [this] {
        QTextCursor c = m_edit->textCursor();
        QTextBlockFormat bf = c.blockFormat();
        bf.setIndent(bf.indent() + 1);
        c.setBlockFormat(bf);
    });
    connect(outdent, &QToolButton::clicked, this, [this] {
        QTextCursor c = m_edit->textCursor();
        QTextBlockFormat bf = c.blockFormat();
        bf.setIndent(std::max(0, bf.indent() - 1));
        c.setBlockFormat(bf);
    });
    connect(image, &QToolButton::clicked, this, &RichWorkspace::insertImage);
    connect(m_findEdit, &QLineEdit::returnPressed, this, [this] { find(QApplication::keyboardModifiers() & Qt::ShiftModifier); });
    connect(next, &QToolButton::clicked, this, [this] { find(false); });
    connect(prev, &QToolButton::clicked, this, [this] { find(true); });
    connect(closeFind, &QToolButton::clicked, m_findBar, &QWidget::hide);
    connect(rep, &QPushButton::clicked, this, &RichWorkspace::replaceOne);
    connect(repAll, &QPushButton::clicked, this, &RichWorkspace::replaceAll);

    auto shortcut = [this](const QKeySequence& k, auto fn) {
        auto* s = new QShortcut(k, this);
        s->setContext(Qt::WidgetWithChildrenShortcut);
        connect(s, &QShortcut::activated, this, fn);
    };
    shortcut(QKeySequence::Save, [this] { save(); });
    shortcut(QKeySequence::SaveAs, [this] { saveAs(); });
    shortcut(QKeySequence::Find, [this] { showFind(false); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_H), [this] { showFind(true); });
    shortcut(QKeySequence::FindNext, [this] { find(false); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_L), [this] { setAlignment(Qt::AlignLeft | Qt::AlignAbsolute); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_E), [this] { setAlignment(Qt::AlignHCenter); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_R), [this] { setAlignment(Qt::AlignRight | Qt::AlignAbsolute); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_J), [this] { setAlignment(Qt::AlignJustify); });
    shortcut(QKeySequence::ZoomIn, [this] { m_view->setZoom(m_view->zoom() * 1.1); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal), [this] { m_view->setZoom(m_view->zoom() * 1.1); });
    shortcut(QKeySequence::ZoomOut, [this] { m_view->setZoom(m_view->zoom() / 1.1); });
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), m_findBar);
    esc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(esc, &QShortcut::activated, this, [this] {
        m_findBar->hide();
        m_edit->setFocus();
    });
    m_undo->setEnabled(false);
    m_redo->setEnabled(false);
    m_saveBtn->setEnabled(false);
}

void RichWorkspace::openFile(const QString& path, const QString& format)
{
    if (!maybeClose())
        return;
    closeDocument();
    m_pendingPath = path;
    m_pendingFormat = format;
    if (!m_workDir.isEmpty())
        QDir(m_workDir).removeRecursively();
    m_workDir = PathUtils::tempRoot() + "/edit-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_cancel = std::make_shared<std::atomic_bool>(false);
    if (!m_importWatcher) {
        m_importWatcher = new QFutureWatcher<RichDocumentIO::Imported>(this);
        connect(m_importWatcher, &QFutureWatcherBase::finished, this, &RichWorkspace::onImported);
    }
    Q_EMIT busy(format == "pdf" ? tr("Reconstructing an editable document from the PDF…")
                                : tr("Opening %1…").arg(QFileInfo(path).fileName()),
                -1);
    const QString work = m_workDir;
    auto cancel = m_cancel;
    m_importWatcher->setFuture(QtConcurrent::run([path, format, work, cancel] {
        return RichDocumentIO::importFile(path, format, work, cancel.get());
    }));
}

void RichWorkspace::onImported()
{
    Q_EMIT busy(QString(), 100);
    const RichDocumentIO::Imported im = m_importWatcher->result();
    if (!im.ok) {
        Q_EMIT failed(im.error);
        return;
    }
    m_path = QFileInfo(m_pendingPath).absoluteFilePath();
    m_format = m_pendingFormat;
    QTextDocument* doc = m_edit->document();
    doc->clear();
    doc->setBaseUrl(QUrl::fromLocalFile(im.baseDir + "/"));
    doc->setMetaInformation(QTextDocument::DocumentUrl, QUrl::fromLocalFile(im.baseDir + "/").toString());
    switch (im.kind) {
    case RichDocumentIO::Kind::Html: m_edit->setHtml(im.content); break;
    case RichDocumentIO::Kind::Markdown: doc->setMarkdown(im.content, QTextDocument::MarkdownDialectGitHub); break;
    case RichDocumentIO::Kind::PlainText: m_edit->setPlainText(im.content); break;
    }
    m_view->applyPageSetup();
    doc->setModified(false);
    doc->clearUndoRedoStacks();
    m_confirmedOverwrite = false;
    m_title->setText(QFileInfo(m_path).fileName() + (m_format == "pdf" ? tr("  (reflow editing)") : QString()));
    m_title->setToolTip(QDir::toNativeSeparators(m_path));
    // Fit the page width once the view has its final size.
    QTimer::singleShot(0, this, [this] {
        m_view->setZoom(std::clamp((m_view->viewport()->width() - 40) / (PagedDocumentView::kPageWidth + 80), 0.5, 1.5));
        m_view->goToPage(0);
    });
    QStringList notes = im.warnings;
    const QString fid = RichDocumentIO::fidelityWarning(m_format);
    if (m_format != "txt" && m_format != "md" && m_format != "html" && !fid.isEmpty())
        notes << fid;
    if (!notes.isEmpty())
        m_banner->show(m_format == "pdf" ? Banner::Kind::Warning : Banner::Kind::Info, notes.join("\n"));
    else
        m_banner->hide();
    Database::instance().touchRecent(m_path, m_format, "edit");
    m_edit->setFocus();
    m_edit->moveCursor(QTextCursor::Start);
    Q_EMIT opened();
    Q_EMIT statusChanged();
}

bool RichWorkspace::isModified() const
{
    return hasDocument() && m_edit->document()->isModified();
}

int RichWorkspace::pageCount() const { return hasDocument() ? m_view->pageCount() : 0; }
int RichWorkspace::currentPage() const { return m_view->currentPage(); }
double RichWorkspace::zoom() const { return m_view->zoom(); }

QString RichWorkspace::saveStatus() const
{
    if (!hasDocument())
        return {};
    return isModified() ? tr("Unsaved changes") : tr("All changes saved");
}

bool RichWorkspace::maybeClose()
{
    if (!isModified())
        return true;
    const auto r = QMessageBox::warning(this, tr("Unsaved changes"),
                                        tr("\"%1\" has unsaved changes. Save them?").arg(QFileInfo(m_path).fileName()),
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (r == QMessageBox::Save)
        return save();
    return r == QMessageBox::Discard;
}

void RichWorkspace::closeDocument()
{
    m_edit->clear();
    m_edit->document()->setModified(false);
    m_path.clear();
    m_format.clear();
    m_banner->hide();
    m_findBar->hide();
    Q_EMIT documentClosed();
    Q_EMIT statusChanged();
}

void RichWorkspace::syncFormatControls()
{
    m_syncing = true;
    const QTextCharFormat f = m_edit->currentCharFormat();
    const QFont font = f.font();
    m_fontBox->setCurrentFont(font);
    m_sizeBox->setValue(font.pointSizeF() > 0 ? font.pointSizeF() : 11);
    m_bold->setChecked(font.bold());
    m_italic->setChecked(font.italic());
    m_underline->setChecked(font.underline());
    const Qt::Alignment a = m_edit->alignment();
    if (a & Qt::AlignHCenter)
        m_alignC->setChecked(true);
    else if (a & Qt::AlignRight)
        m_alignR->setChecked(true);
    else if (a & Qt::AlignJustify)
        m_alignJ->setChecked(true);
    else
        m_alignL->setChecked(true);
    QTextList* list = m_edit->textCursor().currentList();
    m_bullets->setChecked(list && list->format().style() == QTextListFormat::ListDisc);
    m_numbers->setChecked(list && list->format().style() == QTextListFormat::ListDecimal);
    const int heading = m_edit->textCursor().blockFormat().headingLevel();
    m_styleBox->setCurrentIndex(std::clamp(heading, 0, 3));
    m_syncing = false;
}

void RichWorkspace::mergeFormat(const QTextCharFormat& fmt)
{
    QTextCursor c = m_edit->textCursor();
    if (!c.hasSelection())
        c.select(QTextCursor::WordUnderCursor);
    c.mergeCharFormat(fmt);
    m_edit->mergeCurrentCharFormat(fmt);
    m_edit->setFocus();
}

void RichWorkspace::setAlignment(Qt::Alignment a)
{
    m_edit->setAlignment(a);
    syncFormatControls();
}

void RichWorkspace::toggleList(bool numbered)
{
    QTextCursor c = m_edit->textCursor();
    const QTextListFormat::Style style = numbered ? QTextListFormat::ListDecimal : QTextListFormat::ListDisc;
    c.beginEditBlock();
    if (QTextList* list = c.currentList(); list && list->format().style() == style) {
        // Remove from list
        QTextBlockFormat bf = c.blockFormat();
        list->remove(c.block());
        bf.setIndent(0);
        c.setBlockFormat(bf);
    } else {
        QTextListFormat lf;
        lf.setStyle(style);
        lf.setIndent(1);
        c.createList(lf);
    }
    c.endEditBlock();
    syncFormatControls();
}

void RichWorkspace::setLineSpacing(int percent)
{
    QTextBlockFormat bf;
    bf.setLineHeight(percent, QTextBlockFormat::ProportionalHeight);
    QTextCursor c = m_edit->textCursor();
    c.mergeBlockFormat(bf);
}

void RichWorkspace::setParagraphStyle(int level)
{
    QTextCursor c = m_edit->textCursor();
    c.beginEditBlock();
    QTextBlockFormat bf = c.blockFormat();
    bf.setHeadingLevel(level);
    c.setBlockFormat(bf);
    c.select(QTextCursor::BlockUnderCursor);
    QTextCharFormat cf;
    static const double sizes[] = {11, 20, 16, 13};
    cf.setFontWeight(level > 0 ? QFont::Bold : QFont::Normal);
    cf.setFontPointSize(sizes[std::clamp(level, 0, 3)]);
    c.mergeCharFormat(cf);
    c.endEditBlock();
}

void RichWorkspace::insertImage()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Insert image"), QString(),
                                                      tr("Images (*.png *.jpg *.jpeg *.bmp *.webp *.gif *.tif *.tiff *.svg)"));
    if (file.isEmpty())
        return;
    QImageReader reader(file);
    reader.setAutoTransform(true);
    QImage img = reader.read();
    if (img.isNull()) {
        m_banner->show(Banner::Kind::Error, tr("The image could not be read: %1").arg(reader.errorString()));
        return;
    }
    const QString name = QStringLiteral("folvio-image-%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_edit->document()->addResource(QTextDocument::ImageResource, QUrl(name), img);
    QTextImageFormat fmt;
    fmt.setName(name);
    const double maxW = PagedDocumentView::kPageWidth - 2 * PagedDocumentView::kMargin;
    double w = img.width(), h = img.height();
    if (w > maxW) {
        h *= maxW / w;
        w = maxW;
    }
    fmt.setWidth(w);
    fmt.setHeight(h);
    m_edit->textCursor().insertImage(fmt);
}

void RichWorkspace::insertTable()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Insert table"));
    auto* form = new QFormLayout(&dlg);
    auto* rows = new QSpinBox(&dlg);
    rows->setRange(1, 200);
    rows->setValue(3);
    auto* cols = new QSpinBox(&dlg);
    cols->setRange(1, 30);
    cols->setValue(3);
    form->addRow(tr("Rows"), rows);
    form->addRow(tr("Columns"), cols);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QTextTableFormat tf;
    tf.setBorder(1);
    tf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    tf.setBorderBrush(QColor("#808080"));
    tf.setCellPadding(4);
    tf.setCellSpacing(0);
    tf.setBorderCollapse(true);
    tf.setWidth(QTextLength(QTextLength::PercentageLength, 100));
    m_edit->textCursor().insertTable(rows->value(), cols->value(), tf);
}

void RichWorkspace::tableOperation(const QString& op)
{
    QTextCursor c = m_edit->textCursor();
    QTextTable* t = c.currentTable();
    if (!t) {
        m_banner->show(Banner::Kind::Info, tr("Place the cursor inside a table first."));
        return;
    }
    const QTextTableCell cell = t->cellAt(c);
    if (op == "rowAbove")
        t->insertRows(cell.row(), 1);
    else if (op == "rowBelow")
        t->insertRows(cell.row() + 1, 1);
    else if (op == "colLeft")
        t->insertColumns(cell.column(), 1);
    else if (op == "colRight")
        t->insertColumns(cell.column() + 1, 1);
    else if (op == "deleteRow")
        t->removeRows(cell.row(), 1);
    else if (op == "deleteCol")
        t->removeColumns(cell.column(), 1);
    else if (op == "merge")
        t->mergeCells(c);
}

void RichWorkspace::showFind(bool replace)
{
    m_findBar->show();
    m_replaceRow->setVisible(replace);
    if (m_edit->textCursor().hasSelection())
        m_findEdit->setText(m_edit->textCursor().selectedText());
    m_findEdit->setFocus();
    m_findEdit->selectAll();
}

void RichWorkspace::find(bool backwards)
{
    const QString needle = m_findEdit->text();
    if (needle.isEmpty())
        return;
    QTextDocument::FindFlags flags;
    if (backwards)
        flags |= QTextDocument::FindBackward;
    if (m_findCase->isChecked())
        flags |= QTextDocument::FindCaseSensitively;
    if (!m_edit->find(needle, flags)) {
        // Wrap around
        QTextCursor c = m_edit->textCursor();
        c.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
        m_edit->setTextCursor(c);
        if (!m_edit->find(needle, flags)) {
            m_findStatus->setText(tr("No matches"));
            return;
        }
    }
    m_findStatus->clear();
    m_view->ensureCursorVisible();
}

void RichWorkspace::replaceOne()
{
    QTextCursor c = m_edit->textCursor();
    const Qt::CaseSensitivity cs = m_findCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (c.hasSelection() && c.selectedText().compare(m_findEdit->text(), cs) == 0)
        c.insertText(m_replaceEdit->text());
    find(false);
}

void RichWorkspace::replaceAll()
{
    const QString needle = m_findEdit->text();
    if (needle.isEmpty())
        return;
    QTextDocument::FindFlags flags;
    if (m_findCase->isChecked())
        flags |= QTextDocument::FindCaseSensitively;
    QTextCursor c(m_edit->document());
    c.beginEditBlock();
    int n = 0;
    QTextCursor found = m_edit->document()->find(needle, 0, flags);
    while (!found.isNull()) {
        found.insertText(m_replaceEdit->text());
        ++n;
        found = m_edit->document()->find(needle, found, flags);
    }
    c.endEditBlock();
    m_findStatus->setText(tr("%n replacement(s)", nullptr, n));
}

bool RichWorkspace::writeTo(const QString& target, const QString& format, bool replaceOriginal)
{
    QString err;
    const QString work = m_workDir + "/save-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(work);
    QString produced;
    // 1. Serialize in the GUI thread (QTextDocument is not thread safe).
    const bool office = format == "docx" || format == "doc" || format == "odt" || format == "rtf";
    const QString intermediate = work + "/document." + (office ? QStringLiteral("odt") : format);
    if (!RichDocumentIO::writeIntermediate(m_edit->document(), office ? "odt" : format, intermediate, &err)) {
        QMessageBox::critical(this, tr("Save failed"), err);
        return false;
    }
    produced = intermediate;
    // 2. Office formats: convert ODT via LibreOffice in the background, keeping the UI responsive.
    if (office && format != "odt") {
        const QString out = work + "/document." + format;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        Q_EMIT busy(tr("Writing %1…").arg(format.toUpper()), -1);
        auto future = QtConcurrent::run([intermediate, format, out, work] {
            QString e;
            std::atomic_bool cancel{false};
            const bool ok = RichDocumentIO::convertFromOdt(intermediate, format, out, work, &cancel, &e);
            return std::make_pair(ok, e);
        });
        while (!future.isFinished())
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        QApplication::restoreOverrideCursor();
        Q_EMIT busy(QString(), 100);
        const auto [ok, e] = future.result();
        if (!ok) {
            QMessageBox::critical(this, tr("Save failed"), e);
            return false;
        }
        produced = out;
    }
    auto producer = [produced](const QString& tmp, QString* e) {
        if (!QFile::copy(produced, tmp)) {
            *e = QObject::tr("Could not write the temporary file.");
            return false;
        }
        return true;
    };
    auto validator = [format](const QString& tmp, QString* e) {
        const ValidationResult v = OutputValidator::validate(tmp, format);
        if (!v.ok)
            *e = v.error;
        return v.ok;
    };
    SafeFileWriter::Result r;
    if (replaceOriginal) {
        SafeSaveOptions o;
        o.createBackup = Database::instance().setting("backup.enabled", true).toBool();
        if (Database::instance().setting("backup.location", "appdata").toString() == "beside")
            o.backupDir = QFileInfo(target).absolutePath();
        r = SafeFileWriter::replace(target, producer, validator, o);
    } else {
        r = SafeFileWriter::writeNew(target, producer, validator, true);
    }
    QDir(work).removeRecursively();
    if (!r.ok()) {
        QMessageBox::critical(this, tr("Save failed"), r.error + "\n\n" + tr("The existing file was not changed."));
        return false;
    }
    m_banner->show(Banner::Kind::Success, r.backupPath.isEmpty() ? tr("Saved to %1.").arg(QDir::toNativeSeparators(target))
                                                                 : tr("Saved. Backup: %1").arg(QDir::toNativeSeparators(r.backupPath)));
    Q_EMIT savedTo(target);
    return true;
}

bool RichWorkspace::save()
{
    if (!hasDocument())
        return false;
    QString reason;
    if (!RichDocumentIO::canSaveBack(m_format, &reason)) {
        QMessageBox::warning(this, tr("Save"), reason + "\n" + tr("Use Save As or Export instead."));
        return false;
    }
    if (!m_confirmedOverwrite) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Save to original file"));
        box.setText(tr("Save your changes into the original file?"));
        QString info = QDir::toNativeSeparators(m_path);
        const QString fid = RichDocumentIO::fidelityWarning(m_format);
        if (!fid.isEmpty())
            info += "\n\n⚠ " + fid;
        info += "\n\n" + tr("A backup copy of the current file is kept before it is replaced.");
        box.setInformativeText(info);
        box.setStandardButtons(QMessageBox::Save | QMessageBox::Cancel);
        if (box.exec() != QMessageBox::Save)
            return false;
        m_confirmedOverwrite = true;
    }
    if (!writeTo(m_path, m_format, true))
        return false;
    m_edit->document()->setModified(false);
    Q_EMIT statusChanged();
    return true;
}

bool RichWorkspace::saveAs()
{
    if (!hasDocument())
        return false;
    const QFileInfo fi(m_path);
    const QString filter = tr("Word document (*.docx);;OpenDocument text (*.odt);;Rich Text (*.rtf);;PDF (*.pdf);;"
                              "HTML (*.html);;Markdown (*.md);;Plain text (*.txt)");
    QString selected;
    const QString def = m_format == "pdf" ? "pdf" : m_format == "doc" ? "docx" : m_format;
    QString path = QFileDialog::getSaveFileName(this, tr("Save as"), PathUtils::uniquePath(fi.absolutePath() + "/" + fi.completeBaseName() + " (edited)." + def), filter, &selected);
    if (path.isEmpty())
        return false;
    QString fmt = QFileInfo(path).suffix().toLower();
    if (fmt == "htm")
        fmt = "html";
    if (!QStringList{"docx", "odt", "rtf", "pdf", "html", "md", "txt"}.contains(fmt)) {
        QMessageBox::warning(this, tr("Save as"), tr("Please choose one of the listed file types."));
        return false;
    }
    if (!writeTo(path, fmt, false))
        return false;
    Database::instance().touchRecent(path, fmt, "edit");
    return true;
}

void RichWorkspace::exportAs(const QString& format)
{
    if (!hasDocument())
        return;
    const QFileInfo fi(m_path);
    const QString path = QFileDialog::getSaveFileName(this, tr("Export"), PathUtils::uniquePath(fi.absolutePath() + "/" + fi.completeBaseName() + "." + format),
                                                      tr("%1 files (*.%2)").arg(format.toUpper(), format));
    if (path.isEmpty())
        return;
    if (PathUtils::samePath(path, m_path)) {
        save();
        return;
    }
    writeTo(path, format, false);
}

} // namespace folvio
