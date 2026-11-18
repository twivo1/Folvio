#include "editor/pdf_native/PdfWorkspace.h"

#include "core/conversions/ConversionRegistry.h"
#include "core/files/PathUtils.h"
#include "core/jobs/ConversionJob.h"
#include "core/persistence/Database.h"
#include "editor/pdf_native/FontMatcher.h"
#include "editor/pdf_native/PdfCanvas.h"
#include "ui/themes/Theme.h"
#include "ui/widgets/CommonWidgets.h"
#include "ui/widgets/Icons.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QDir>
#include <QStringListModel>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace folvio {

// ---------------------------------------------------------------------------

InlineTextEditor::InlineTextEditor(QWidget* parent) : QPlainTextEdit(parent)
{
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    document()->setDocumentMargin(1);
}

void InlineTextEditor::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        Q_EMIT cancelRequested();
        return;
    }
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (e->modifiers() & Qt::ControlModifier)) {
        Q_EMIT commitRequested();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

// ---------------------------------------------------------------------------

PdfWorkspace::PdfWorkspace(QWidget* parent) : QWidget(parent)
{
    buildUi();
    m_recoveryTimer = new QTimer(this);
    m_recoveryTimer->setSingleShot(true);
    m_recoveryTimer->setInterval(1500);
    connect(m_recoveryTimer, &QTimer::timeout, this, [this] {
        if (m_session)
            m_session->writeRecovery();
    });
}

PdfWorkspace::~PdfWorkspace()
{
    if (m_ocrCancel)
        m_ocrCancel->store(true);
    if (m_ocrWatcher)
        m_ocrWatcher->waitForFinished();
}

void PdfWorkspace::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* bar = new QWidget(this);
    bar->setObjectName("Toolbar");
    m_formatBar = new QWidget(this);
    m_formatBar->setObjectName("Toolbar");
    buildToolbar(bar, m_formatBar);
    root->addWidget(bar);
    root->addWidget(m_formatBar);

    auto* bannerWrap = new QWidget(this);
    auto* bannerLay = new QVBoxLayout(bannerWrap);
    bannerLay->setContentsMargins(10, 6, 10, 0);
    m_banner = new Banner(bannerWrap);
    bannerLay->addWidget(m_banner);
    root->addWidget(bannerWrap);

    // Find / replace bar
    m_findBar = new QWidget(this);
    m_findBar->setObjectName("Toolbar");
    auto* fl = new QVBoxLayout(m_findBar);
    fl->setContentsMargins(10, 6, 10, 6);
    fl->setSpacing(4);
    auto* findRow = new QHBoxLayout;
    m_findEdit = new QLineEdit(m_findBar);
    m_findEdit->setPlaceholderText(tr("Find in document"));
    m_findEdit->setClearButtonEnabled(true);
    m_caseSensitive = new QCheckBox(tr("Match case"), m_findBar);
    auto* prev = makeToolButton("chevron-up", tr("Previous match (Shift+F3)"), m_findBar);
    auto* next = makeToolButton("chevron-down", tr("Next match (F3)"), m_findBar);
    m_findStatus = new QLabel(m_findBar);
    m_findStatus->setProperty("muted", true);
    auto* closeFind = makeToolButton("close", tr("Close (Esc)"), m_findBar);
    auto* findIcon = new QLabel(m_findBar);
    findIcon->setPixmap(Icons::pixmap("search", 16, Icons::Tone::Muted, devicePixelRatioF()));
    findRow->addWidget(findIcon);
    findRow->addWidget(m_findEdit, 1);
    findRow->addWidget(m_caseSensitive);
    findRow->addWidget(prev);
    findRow->addWidget(next);
    findRow->addWidget(m_findStatus);
    findRow->addWidget(closeFind);
    m_replaceRow = new QWidget(m_findBar);
    auto* rr = new QHBoxLayout(m_replaceRow);
    rr->setContentsMargins(22, 0, 0, 0);
    m_replaceEdit = new QLineEdit(m_replaceRow);
    m_replaceEdit->setPlaceholderText(tr("Replace with"));
    auto* replaceOne = new QPushButton(tr("Replace"), m_replaceRow);
    auto* replaceAllBtn = new QPushButton(tr("Replace all"), m_replaceRow);
    rr->addWidget(m_replaceEdit, 1);
    rr->addWidget(replaceOne);
    rr->addWidget(replaceAllBtn);
    fl->addLayout(findRow);
    fl->addWidget(m_replaceRow);
    m_findBar->hide();
    root->addWidget(m_findBar);
    connect(m_findEdit, &QLineEdit::textChanged, this, &PdfWorkspace::runFind);
    connect(m_caseSensitive, &QCheckBox::toggled, this, &PdfWorkspace::runFind);
    connect(m_findEdit, &QLineEdit::returnPressed, this, [this] { findNext(QApplication::keyboardModifiers() & Qt::ShiftModifier); });
    connect(next, &QToolButton::clicked, this, [this] { findNext(false); });
    connect(prev, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(closeFind, &QToolButton::clicked, this, [this] {
        m_findBar->hide();
        m_canvas->setHighlights({});
    });
    connect(replaceOne, &QPushButton::clicked, this, &PdfWorkspace::replaceCurrent);
    connect(replaceAllBtn, &QPushButton::clicked, this, &PdfWorkspace::replaceAll);

    // Main area: thumbnails | views | side panel
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    m_thumbs = new QListWidget(split);
    m_thumbs->setViewMode(QListView::ListMode);
    m_thumbs->setIconSize(QSize(112, 150));
    m_thumbs->setSpacing(6);
    m_thumbs->setMinimumWidth(130);
    m_thumbs->setMaximumWidth(180);
    m_thumbs->setFrameShape(QFrame::NoFrame);
    m_thumbs->setUniformItemSizes(true);
    connect(m_thumbs, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row != currentPage())
            goToPage(row);
    });

    m_viewSplitter = new QSplitter(Qt::Horizontal, split);
    m_scrollOriginal = new QScrollArea(m_viewSplitter);
    m_canvasOriginal = new PdfCanvas;
    m_scrollOriginal->setWidget(m_canvasOriginal);
    m_scrollOriginal->setWidgetResizable(false);
    m_scrollOriginal->setAlignment(Qt::AlignHCenter);
    m_scrollOriginal->setFrameShape(QFrame::NoFrame);
    m_scroll = new QScrollArea(m_viewSplitter);
    m_canvas = new PdfCanvas;
    m_canvas->setInteractive(true);
    m_scroll->setWidget(m_canvas);
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignHCenter);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->viewport()->installEventFilter(this);
    m_scrollOriginal->hide();

    // Synchronized scrolling for side by side comparison.
    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        if (m_viewMode == ViewMode::SideBySide)
            m_scrollOriginal->verticalScrollBar()->setValue(v);
        Q_EMIT statusChanged();
        if (m_thumbs->currentRow() != currentPage()) {
            QSignalBlocker b(m_thumbs);
            m_thumbs->setCurrentRow(currentPage());
        }
    });
    connect(m_scrollOriginal->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        if (m_viewMode == ViewMode::SideBySide)
            m_scroll->verticalScrollBar()->setValue(v);
    });

    m_sidePanel = new QWidget(split);
    m_sidePanel->setMinimumWidth(240);
    m_sidePanel->setMaximumWidth(380);
    auto* sl = new QVBoxLayout(m_sidePanel);
    sl->setContentsMargins(10, 10, 10, 10);
    sl->setSpacing(8);
    auto* infoTitle = new QLabel(tr("Document"), m_sidePanel);
    infoTitle->setObjectName("SectionTitle");
    m_analysis = new QLabel(m_sidePanel);
    m_analysis->setWordWrap(true);
    m_analysis->setProperty("muted", true);
    m_warningsLabel = new QLabel(m_sidePanel);
    m_warningsLabel->setWordWrap(true);
    m_warningsLabel->setProperty("muted", true);
    sl->addWidget(infoTitle);
    sl->addWidget(m_analysis);
    sl->addWidget(m_warningsLabel);

    m_ocrBox = new QWidget(m_sidePanel);
    auto* ol = new QVBoxLayout(m_ocrBox);
    ol->setContentsMargins(0, 8, 0, 0);
    auto* ocrTitle = new QLabel(tr("Text recognition (OCR)"), m_ocrBox);
    ocrTitle->setObjectName("SectionTitle");
    m_ocrSummary = new QLabel(m_ocrBox);
    m_ocrSummary->setWordWrap(true);
    m_ocrOnlyDoubtful = new QCheckBox(tr("Show only uncertain words"), m_ocrBox);
    m_ocrOnlyDoubtful->setChecked(true);
    m_ocrTable = new QTableWidget(0, 3, m_ocrBox);
    m_ocrTable->setHorizontalHeaderLabels({tr("Page"), tr("Recognized text"), tr("Confidence")});
    m_ocrTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_ocrTable->verticalHeader()->hide();
    m_ocrTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    auto* applyOcr = makePrimaryButton(tr("Apply text layer"), m_ocrBox);
    applyOcr->setToolTip(tr("Adds the (corrected) recognized text as an invisible, searchable layer. The page images stay unchanged."));
    ol->addWidget(ocrTitle);
    ol->addWidget(m_ocrSummary);
    ol->addWidget(m_ocrOnlyDoubtful);
    ol->addWidget(m_ocrTable, 1);
    ol->addWidget(applyOcr);
    sl->addWidget(m_ocrBox, 1);
    sl->addStretch();
    m_ocrBox->hide();
    connect(m_ocrOnlyDoubtful, &QCheckBox::toggled, this, &PdfWorkspace::populateOcrTable);
    connect(applyOcr, &QPushButton::clicked, this, &PdfWorkspace::applyOcrLayer);
    connect(m_ocrTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item->column() != 1)
            return;
        const int page = item->data(Qt::UserRole).toInt();
        const int idx = item->data(Qt::UserRole + 1).toInt();
        if (m_ocr.contains(page) && idx >= 0 && idx < m_ocr[page].size())
            m_ocr[page][idx].text = item->text().trimmed();
    });
    connect(m_ocrTable, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row < 0)
            return;
        const auto* item = m_ocrTable->item(row, 1);
        if (!item)
            return;
        const int page = item->data(Qt::UserRole).toInt();
        const int idx = item->data(Qt::UserRole + 1).toInt();
        if (!m_ocr.contains(page) || idx < 0 || idx >= m_ocr[page].size())
            return;
        m_canvas->setHighlights({{page, {m_ocr[page][idx].rect}}});
        const QRectF w = m_canvas->pdfRectToWidget(page, m_ocr[page][idx].rect);
        m_scroll->ensureVisible(int(w.center().x()), int(w.center().y()), 100, 120);
    });

    split->addWidget(m_thumbs);
    split->addWidget(m_viewSplitter);
    split->addWidget(m_sidePanel);
    split->setStretchFactor(1, 1);
    split->setSizes({150, 900, 260});
    root->addWidget(split, 1);

    m_editor = new InlineTextEditor(m_canvas);
    m_editor->hide();
    connect(m_editor, &InlineTextEditor::commitRequested, this, &PdfWorkspace::commitEdit);
    connect(m_editor, &InlineTextEditor::cancelRequested, this, &PdfWorkspace::cancelEdit);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &PdfWorkspace::positionEditor);
    // Commit when focus leaves the editor for anything other than the formatting controls.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (m_editKind == EditKind::None || !m_editor->isVisible())
            return;
        if (!now || now == m_editor || m_formatBar->isAncestorOf(now))
            return;
        if (qobject_cast<QColorDialog*>(now->window()))
            return;
        commitEdit();
    });

    connect(m_canvas, &PdfCanvas::clicked, this, &PdfWorkspace::onCanvasClicked);
    connect(m_canvas, &PdfCanvas::zoomRequested, this, [this](int steps) { setZoom(zoom() * (steps > 0 ? 1.15 : 1 / 1.15)); });
    connect(m_canvasOriginal, &PdfCanvas::zoomRequested, this, [this](int steps) { setZoom(zoom() * (steps > 0 ? 1.15 : 1 / 1.15)); });

    // Keyboard shortcuts scoped to this workspace.
    auto shortcut = [this](const QKeySequence& k, auto fn) {
        auto* s = new QShortcut(k, this);
        s->setContext(Qt::WidgetWithChildrenShortcut);
        connect(s, &QShortcut::activated, this, fn);
    };
    shortcut(QKeySequence::Save, [this] { save(); });
    shortcut(QKeySequence::SaveAs, [this] { saveAs(); });
    shortcut(QKeySequence::Find, [this] { showFind(false); });
    shortcut(QKeySequence::Replace, [this] { showFind(true); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_H), [this] { showFind(true); });
    shortcut(QKeySequence::FindNext, [this] { findNext(false); });
    shortcut(QKeySequence::FindPrevious, [this] { findNext(true); });
    shortcut(QKeySequence::ZoomIn, [this] { setZoom(zoom() * 1.15); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal), [this] { setZoom(zoom() * 1.15); });
    shortcut(QKeySequence::ZoomOut, [this] { setZoom(zoom() / 1.15); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_0), [this] { fitWidth(); });
    shortcut(QKeySequence(Qt::Key_PageDown), [this] { goToPage(currentPage() + 1); });
    shortcut(QKeySequence(Qt::Key_PageUp), [this] { goToPage(currentPage() - 1); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Home), [this] { goToPage(0); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_End), [this] { goToPage(pageCount() - 1); });
    auto* undoShortcut = new QShortcut(QKeySequence::Undo, this);
    undoShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(undoShortcut, &QShortcut::activated, this, [this] {
        if (m_editor->hasFocus())
            m_editor->undo();
        else
            undo();
    });
    // Ctrl+Y and Ctrl+Shift+Z on every platform (QKeySequence::Redo differs between Windows and
    // Linux; two shortcuts with the same key would be ambiguous and neither would fire).
    auto* redoShortcut = new QShortcut(this);
    redoShortcut->setKeys({QKeySequence(Qt::CTRL | Qt::Key_Y), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    redoShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(redoShortcut, &QShortcut::activated, this, [this] {
        if (m_editor->hasFocus())
            m_editor->redo();
        else
            redo();
    });
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), m_findBar);
    esc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(esc, &QShortcut::activated, this, [this] {
        m_findBar->hide();
        m_canvas->setHighlights({});
        m_canvas->setFocus();
    });

    updateActions();
}

void PdfWorkspace::buildToolbar(QWidget* bar, QWidget* formatBar)
{
    auto* l = new QHBoxLayout(bar);
    l->setContentsMargins(10, 4, 10, 4);
    l->setSpacing(4);
    auto* fileIcon = new QLabel(bar);
    fileIcon->setPixmap(Icons::pixmap("file", 18, Icons::Tone::Accent, devicePixelRatioF()));
    m_title = new QLabel(bar);
    m_title->setObjectName("DocTitle");
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_undoBtn = makeToolButton("undo", tr("Undo (Ctrl+Z)"), bar);
    m_redoBtn = makeToolButton("redo", tr("Redo (Ctrl+Y)"), bar);
    connect(m_undoBtn, &QToolButton::clicked, this, &PdfWorkspace::undo);
    connect(m_redoBtn, &QToolButton::clicked, this, &PdfWorkspace::redo);

    auto* findBtn = makeToolButton("search", tr("Find and replace (Ctrl+F / Ctrl+H)"), bar);
    connect(findBtn, &QToolButton::clicked, this, [this] { showFind(true); });

    m_viewCombo = new QComboBox(bar);
    m_viewCombo->addItem(Icons::get("edit"), tr("Edit"));
    m_viewCombo->addItem(Icons::get("file"), tr("Original"));
    m_viewCombo->addItem(Icons::get("compare"), tr("Side by side"));
    m_viewCombo->addItem(Icons::get("compare"), tr("Differences"));
    m_viewCombo->setToolTip(tr("Compare the original document with your changes before saving"));
    connect(m_viewCombo, &QComboBox::currentIndexChanged, this, [this](int i) { setViewMode(ViewMode(i)); });
    auto* compareLabel = new QLabel(tr("View"), bar);
    compareLabel->setProperty("muted", true);

    m_ocrBtn = makeToolButton("ocr", tr("Recognize text (OCR)"), bar);
    m_ocrBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_ocrBtn->setText(tr("OCR"));
    connect(m_ocrBtn, &QToolButton::clicked, this, &PdfWorkspace::startOcr);

    m_reflowBtn = makeToolButton("reflow", tr("Edit as a reflowable document (Word-like). Layout may change."), bar);
    m_reflowBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_reflowBtn->setText(tr("Reflow edit"));
    connect(m_reflowBtn, &QToolButton::clicked, this, [this] {
        if (!m_session)
            return;
        if (isModified()) {
            QMessageBox::information(this, tr("Reflow editing"),
                                     tr("Save or discard your current changes before switching to reflow editing."));
            return;
        }
        Q_EMIT reflowRequested(filePath());
    });

    m_saveBtn = makeToolButton("save", tr("Save to the original file (Ctrl+S)"), bar);
    m_saveBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_saveBtn->setText(tr("Save"));
    m_saveAsBtn = makeToolButton("save-as", tr("Save a copy as… (Ctrl+Shift+S)"), bar);
    m_saveAsBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_saveAsBtn->setText(tr("Save As"));
    m_exportBtn = makeToolButton("export", tr("Export to another format"), bar);
    m_exportBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_exportBtn->setText(tr("Export"));
    m_exportBtn->setPopupMode(QToolButton::InstantPopup);
    auto* exportMenu = new QMenu(m_exportBtn);
    exportMenu->addAction(tr("Word document (.docx)…"), this, [this] { exportAs("docx"); });
    exportMenu->addAction(tr("Plain text (.txt)…"), this, [this] { exportAs("txt"); });
    exportMenu->addAction(tr("HTML (.html)…"), this, [this] { exportAs("html"); });
    exportMenu->addAction(tr("Page images (.png)…"), this, [this] { exportAs("png"); });
    exportMenu->addSeparator();
    exportMenu->addAction(tr("PDF copy…"), this, [this] { saveAs(); });
    m_exportBtn->setMenu(exportMenu);
    connect(m_saveBtn, &QToolButton::clicked, this, &PdfWorkspace::save);
    connect(m_saveAsBtn, &QToolButton::clicked, this, &PdfWorkspace::saveAs);
    auto* closeBtn = makeToolButton("close", tr("Close document"), bar);
    connect(closeBtn, &QToolButton::clicked, this, [this] {
        if (maybeClose())
            closeDocument();
    });

    l->addWidget(fileIcon);
    l->addWidget(m_title);
    l->addSpacing(8);
    l->addWidget(makeSeparator(bar));
    l->addWidget(m_undoBtn);
    l->addWidget(m_redoBtn);
    l->addWidget(makeSeparator(bar));
    l->addWidget(findBtn);
    l->addWidget(compareLabel);
    l->addWidget(m_viewCombo);
    l->addWidget(m_ocrBtn);
    l->addWidget(m_reflowBtn);
    l->addStretch();
    l->addWidget(m_saveBtn);
    l->addWidget(m_saveAsBtn);
    l->addWidget(m_exportBtn);
    l->addWidget(makeSeparator(bar));
    l->addWidget(closeBtn);

    // Formatting / tools row
    auto* f = new QHBoxLayout(formatBar);
    f->setContentsMargins(10, 3, 10, 3);
    f->setSpacing(4);
    m_toolEdit = makeToolButton("text-cursor", tr("Edit existing text: click a paragraph"), formatBar, true);
    m_toolText = makeToolButton("text-add", tr("Add text: click where the text should start"), formatBar, true);
    m_toolImage = makeToolButton("image", tr("Insert image: choose a file, then click on the page"), formatBar, true);
    for (auto* b : {m_toolEdit, m_toolText, m_toolImage}) {
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setAutoExclusive(true);
    }
    m_toolEdit->setText(tr("Edit text"));
    m_toolText->setText(tr("Add text"));
    m_toolImage->setText(tr("Image"));
    m_toolEdit->setChecked(true);
    connect(m_toolEdit, &QToolButton::clicked, this, [this] { setTool(Tool::EditText); });
    connect(m_toolText, &QToolButton::clicked, this, [this] { setTool(Tool::AddText); });
    connect(m_toolImage, &QToolButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Insert image"), QString(),
                                                          tr("Images (*.png *.jpg *.jpeg *.bmp *.webp *.gif *.tif *.tiff)"));
        if (file.isEmpty()) {
            m_toolEdit->setChecked(true);
            setTool(Tool::EditText);
            return;
        }
        m_pendingImage = file;
        setTool(Tool::AddImage);
        m_banner->show(Banner::Kind::Info, tr("Click on the page where the image should be placed."));
    });

    m_fontBox = new QFontComboBox(formatBar);
    m_fontBox->setEditable(false);
    // Only families that Folvio can embed as complete TrueType fonts.
    {
        auto* model = new QStringListModel(FontMatcher::availableFamilies(), m_fontBox);
        m_fontBox->setModel(model);
    }
    m_fontBox->setMinimumWidth(150);
    m_sizeBox = new QDoubleSpinBox(formatBar);
    m_sizeBox->setRange(3.5, 144);
    m_sizeBox->setDecimals(1);
    m_sizeBox->setSingleStep(0.5);
    m_sizeBox->setSuffix(" pt");
    m_sizeBox->setSpecialValueText(QStringLiteral("—")); // shown when no text is being edited
    m_boldBtn = makeToolButton("bold", tr("Bold (Ctrl+B)"), formatBar, true);
    m_italicBtn = makeToolButton("italic", tr("Italic (Ctrl+I)"), formatBar, true);
    m_colorBtn = makeToolButton("text-color", tr("Text colour"), formatBar);
    m_alignLeft = makeToolButton("align-left", tr("Align left"), formatBar, true);
    m_alignCenter = makeToolButton("align-center", tr("Center"), formatBar, true);
    m_alignRight = makeToolButton("align-right", tr("Align right"), formatBar, true);
    for (auto* b : {m_alignLeft, m_alignCenter, m_alignRight})
        b->setAutoExclusive(true);
    m_boldBtn->setShortcut(QKeySequence::Bold);
    m_italicBtn->setShortcut(QKeySequence::Italic);

    auto styleChanged = [this] {
        if (m_syncingControls || m_editKind == EditKind::None)
            return;
        m_editStyle.family = m_fontBox->currentText();
        m_editStyle.size = m_sizeBox->value();
        m_editStyle.bold = m_boldBtn->isChecked();
        m_editStyle.italic = m_italicBtn->isChecked();
        m_editStyle.align = m_alignCenter->isChecked() ? TextStyle::Center
                            : m_alignRight->isChecked() ? TextStyle::Right : TextStyle::Left;
        applyStyleToEditor();
        m_editor->setFocus();
    };
    connect(m_fontBox, &QComboBox::currentTextChanged, this, styleChanged);
    connect(m_sizeBox, &QDoubleSpinBox::valueChanged, this, styleChanged);
    for (auto* b : {m_boldBtn, m_italicBtn, m_alignLeft, m_alignCenter, m_alignRight})
        connect(b, &QToolButton::toggled, this, styleChanged);
    connect(m_colorBtn, &QToolButton::clicked, this, [this] {
        if (m_editKind == EditKind::None)
            return;
        const QColor c = QColorDialog::getColor(m_editStyle.color, this, tr("Text colour"));
        if (c.isValid()) {
            m_editStyle.color = c;
            applyStyleToEditor();
        }
        m_editor->setFocus();
    });

    auto* zoomOut = makeToolButton("zoom-out", tr("Zoom out (Ctrl+-)"), formatBar);
    auto* zoomIn = makeToolButton("zoom-in", tr("Zoom in (Ctrl++)"), formatBar);
    auto* fit = makeToolButton("fit-width", tr("Fit width (Ctrl+0)"), formatBar);
    connect(zoomOut, &QToolButton::clicked, this, [this] { setZoom(zoom() / 1.15); });
    connect(zoomIn, &QToolButton::clicked, this, [this] { setZoom(zoom() * 1.15); });
    connect(fit, &QToolButton::clicked, this, &PdfWorkspace::fitWidth);

    f->addWidget(m_toolEdit);
    f->addWidget(m_toolText);
    f->addWidget(m_toolImage);
    f->addWidget(makeSeparator(formatBar));
    f->addWidget(m_fontBox);
    f->addWidget(m_sizeBox);
    f->addWidget(m_boldBtn);
    f->addWidget(m_italicBtn);
    f->addWidget(m_colorBtn);
    f->addWidget(makeSeparator(formatBar));
    f->addWidget(m_alignLeft);
    f->addWidget(m_alignCenter);
    f->addWidget(m_alignRight);
    f->addStretch();
    f->addWidget(zoomOut);
    f->addWidget(zoomIn);
    f->addWidget(fit);
    updateFormatControls({}, false);
}

// ---------------------------------------------------------------------------

bool PdfWorkspace::openFile(const QString& path, QString* error)
{
    if (!maybeClose())
        return false;
    closeDocument();
    auto session = std::make_unique<PdfEditSession>();
    QString err;
    QByteArray password;
    PdfEditSession::OpenStatus st = session->open(path, {}, &err);
    while (st == PdfEditSession::OpenStatus::NeedsPassword) {
        bool ok = false;
        const QString pw = QInputDialog::getText(this, tr("Password required"),
                                                 tr("\"%1\" is password protected.\nEnter the password to open it:")
                                                     .arg(QFileInfo(path).fileName()),
                                                 QLineEdit::Password, QString(), &ok);
        if (!ok) {
            if (error)
                *error = tr("The document is password protected.");
            return false;
        }
        password = pw.toUtf8();
        st = session->open(path, password, &err);
    }
    if (st != PdfEditSession::OpenStatus::Ok) {
        if (error)
            *error = err;
        return false;
    }
    m_session = std::move(session);
    connect(m_session.get(), &PdfEditSession::documentChanged, this, &PdfWorkspace::onDocumentChanged);
    connect(m_session.get(), &PdfEditSession::modifiedChanged, this, [this] {
        updateActions();
        Q_EMIT statusChanged();
    });
    m_canvas->setSession(m_session.get());
    m_canvas->setDocumentProvider([this] { return m_session ? m_session->current() : nullptr; });
    m_canvasOriginal->setDocumentProvider([this] { return m_session ? m_session->original() : nullptr; });
    m_canvas->setReference([this] { return m_session ? m_session->original() : nullptr; });
    m_confirmedOverwrite = false;
    m_ocr.clear();
    m_ocrBox->hide();
    m_title->setText(QFileInfo(path).fileName());
    m_title->setToolTip(QDir::toNativeSeparators(path));
    m_viewCombo->setCurrentIndex(0);

    const PdfAnalysis& a = m_session->analysis();
    m_analysis->setText(a.summary() + "\n" + tr("PDF version %1.%2").arg(a.fileVersion / 10).arg(a.fileVersion % 10));
    m_warningsLabel->clear();
    m_thumbs->clear();
    for (int i = 0; i < a.pageCount; ++i) {
        auto* item = new QListWidgetItem(m_thumbs);
        item->setToolTip(tr("Page %1").arg(i + 1));

    }
    refreshThumbnails({});
    QTimer::singleShot(0, this, &PdfWorkspace::fitWidth);

    if (a.isScanned) {
        QString why;
        if (TesseractEngine::isAvailable(&why))
            m_banner->show(Banner::Kind::Info,
                           tr("This PDF looks like a scan: it has no text layer, so its text cannot be edited directly. "
                              "Run text recognition (OCR) to make it searchable and to review the recognized text."),
                           {tr("Recognize text")});
        else
            m_banner->show(Banner::Kind::Warning, tr("This PDF looks like a scan without a text layer. %1").arg(why));
    } else if (a.signatureCount > 0) {
        m_banner->show(Banner::Kind::Warning,
                       tr("This document is digitally signed. Any change will invalidate the signature."));
    } else if (!a.hasTextLayer) {
        m_banner->show(Banner::Kind::Info, tr("No editable text was found in this PDF."));
    } else {
        m_banner->show(Banner::Kind::Info,
                       tr("Click any paragraph to edit it in place. Unchanged content keeps its original appearance."));
    }
    disconnect(m_banner, &Banner::actionTriggered, nullptr, nullptr);
    connect(m_banner, &Banner::actionTriggered, this, [this](int) { startOcr(); });

    Database::instance().touchRecent(path, "pdf", "edit");
    updateActions();
    offerRecovery();
    Q_EMIT statusChanged();
    return true;
}

void PdfWorkspace::offerRecovery()
{
    for (const RecoveryInfo& info : PdfEditSession::pendingRecoveries()) {
        if (!PathUtils::samePath(info.documentPath, filePath()))
            continue;
        const auto answer = QMessageBox::question(
            this, tr("Restore unsaved changes?"),
            tr("Folvio found %n unsaved edit(s) of this document from %1 (the application did not close normally).\n\n"
               "Restore them?", nullptr, info.editCount)
                .arg(QLocale().toString(info.savedAt, QLocale::ShortFormat)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) {
            QString err;
            if (!m_session->restoreFromRecovery(info.recoveryFile, &err))
                QMessageBox::warning(this, tr("Restore failed"), err);
        } else {
            PdfEditSession::deleteRecoveryFile(info.recoveryFile);
        }
        break;
    }
}

QString PdfWorkspace::filePath() const { return m_session ? m_session->filePath() : QString(); }
bool PdfWorkspace::isModified() const { return m_session && m_session->isModified(); }
bool PdfWorkspace::hasDocument() const { return m_session != nullptr; }
int PdfWorkspace::pageCount() const { return m_session ? m_session->pageCount() : 0; }
double PdfWorkspace::zoom() const { return m_canvas->zoom(); }

int PdfWorkspace::currentPage() const
{
    const QRect visible(m_scroll->horizontalScrollBar()->value(), m_scroll->verticalScrollBar()->value(),
                        m_scroll->viewport()->width(), m_scroll->viewport()->height());
    return m_canvas->currentPage(visible);
}

QString PdfWorkspace::saveStatus() const
{
    if (!m_session)
        return {};
    return m_session->isModified() ? tr("Unsaved changes") : tr("All changes saved");
}

bool PdfWorkspace::maybeClose()
{
    if (m_editKind != EditKind::None)
        commitEdit();
    if (!isModified())
        return true;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Unsaved changes"));
    box.setText(tr("\"%1\" has unsaved changes.").arg(QFileInfo(filePath()).fileName()));
    box.setInformativeText(tr("Do you want to save them to the original file?"));
    box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Save);
    switch (box.exec()) {
    case QMessageBox::Save: return save();
    case QMessageBox::Discard:
        if (m_session)
            m_session->discardRecovery();
        return true;
    default: return false;
    }
}

void PdfWorkspace::closeDocument()
{
    if (m_ocrCancel)
        m_ocrCancel->store(true);
    cancelEdit();
    m_canvas->setSession(nullptr);
    m_canvas->setDocumentProvider({});
    m_canvasOriginal->setDocumentProvider({});
    m_canvas->setReference({});
    if (m_session)
        m_session->discardRecovery();
    m_session.reset();
    m_thumbs->clear();
    m_banner->hide();
    m_findBar->hide();
    m_ocr.clear();
    m_matches.clear();
    updateActions();
    Q_EMIT documentClosed();
    Q_EMIT statusChanged();
}

void PdfWorkspace::updateActions()
{
    const bool doc = m_session != nullptr;
    m_undoBtn->setEnabled(doc && m_session->canUndo());
    m_redoBtn->setEnabled(doc && m_session->canRedo());
    m_saveBtn->setEnabled(doc && m_session->isModified());
    m_saveAsBtn->setEnabled(doc);
    m_exportBtn->setEnabled(doc);
    m_ocrBtn->setVisible(doc && (m_session->analysis().scannedPages > 0 || !m_session->analysis().hasTextLayer));
    m_reflowBtn->setEnabled(doc && m_session->analysis().hasTextLayer);
    m_viewCombo->setEnabled(doc);
    for (auto* b : {m_toolEdit, m_toolText, m_toolImage})
        b->setEnabled(doc && m_viewMode == ViewMode::Edit);
}

void PdfWorkspace::updateFormatControls(const TextStyle& s, bool enabled)
{
    m_syncingControls = true;
    for (QWidget* w : std::initializer_list<QWidget*>{m_fontBox, m_sizeBox, m_boldBtn, m_italicBtn, m_colorBtn,
                                                      m_alignLeft, m_alignCenter, m_alignRight})
        w->setEnabled(enabled);
    if (enabled) {
        const int idx = m_fontBox->findText(s.family);
        m_fontBox->setCurrentIndex(idx >= 0 ? idx : 0);
        m_sizeBox->setValue(s.size);
        m_boldBtn->setChecked(s.bold);
        m_italicBtn->setChecked(s.italic);
        (s.align == TextStyle::Center ? m_alignCenter : s.align == TextStyle::Right ? m_alignRight : m_alignLeft)->setChecked(true);
        QPixmap sw(18, 18);
        sw.fill(Qt::transparent);
        QPainter p(&sw);
        p.setRenderHint(QPainter::Antialiasing);
        Icons::get("text-color").paint(&p, QRect(0, -2, 18, 18));
        p.fillRect(QRect(3, 14, 12, 3), s.color);
        p.end();
        m_colorBtn->setIcon(QIcon(sw));
    } else {
        m_colorBtn->setIcon(Icons::get("text-color"));
        m_sizeBox->setValue(m_sizeBox->minimum()); // displays the special value text
    }
    m_syncingControls = false;
}

void PdfWorkspace::setTool(Tool t)
{
    if (m_editKind != EditKind::None)
        commitEdit();
    m_tool = t;
    m_canvas->setHoverEnabled(t == Tool::EditText);
    m_canvas->setCrosshair(t != Tool::EditText);
    if (t == Tool::AddText)
        m_banner->show(Banner::Kind::Info, tr("Click on the page where the new text should start."));
}

void PdfWorkspace::setViewMode(ViewMode m)
{
    if (m_editKind != EditKind::None)
        commitEdit();
    m_viewMode = m;
    m_scroll->setVisible(m != ViewMode::Original);
    m_scrollOriginal->setVisible(m == ViewMode::Original || m == ViewMode::SideBySide);
    m_canvas->setInteractive(m == ViewMode::Edit);
    m_canvas->setShowDifferences(m == ViewMode::Differences);
    m_formatBar->setEnabled(m == ViewMode::Edit);
    if (m == ViewMode::SideBySide)
        m_viewSplitter->setSizes({1, 1});
    m_canvasOriginal->setZoom(m_canvas->zoom());
    if (m == ViewMode::Original || m == ViewMode::SideBySide)
        m_scrollOriginal->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->value());
    if (m == ViewMode::Differences && m_session && m_session->changedPages().isEmpty())
        m_banner->show(Banner::Kind::Info, tr("There are no changes to compare yet."));
    else if (m == ViewMode::Differences)
        m_banner->show(Banner::Kind::Info, tr("Areas that differ from the original are tinted red."));
    updateActions();
}

void PdfWorkspace::setZoom(double z, bool keepCenter)
{
    z = std::clamp(z, 0.25, 5.0);
    const double old = m_canvas->zoom();
    auto* vb = m_scroll->verticalScrollBar();
    const double rel = keepCenter && vb->maximum() > 0 ? double(vb->value() + m_scroll->viewport()->height() / 2) / (m_canvas->height()) : 0;
    if (m_editKind != EditKind::None)
        commitEdit();
    m_canvas->setZoom(z);
    m_canvasOriginal->setZoom(z);
    if (keepCenter && std::abs(old - z) > 1e-4)
        vb->setValue(int(rel * m_canvas->height() - m_scroll->viewport()->height() / 2));
    Q_EMIT statusChanged();
}

void PdfWorkspace::fitWidth()
{
    if (!m_session || pageCount() == 0)
        return;
    const double pageW = m_session->current()->pageSize(0).width() * 96.0 / 72.0;
    const int avail = m_scroll->viewport()->width() - 60;
    if (pageW > 0 && avail > 100)
        setZoom(std::min(2.0, avail / pageW), false);
}

void PdfWorkspace::goToPage(int page)
{
    if (!m_session)
        return;
    page = std::clamp(page, 0, pageCount() - 1);
    const QRect r = m_canvas->pageRect(page);
    m_scroll->verticalScrollBar()->setValue(r.top() - 12);
}

void PdfWorkspace::refreshThumbnails(const QSet<int>& pages)
{
    if (!m_session)
        return;
    // Render thumbnails incrementally to keep the UI responsive.
    QList<int> todo;
    for (int i = 0; i < m_thumbs->count(); ++i)
        if (pages.isEmpty() || pages.contains(i))
            todo << i;
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, todo, step, idx = 0]() mutable {
        if (!m_session)
            return;
        for (int n = 0; n < 4 && idx < todo.size(); ++n, ++idx) {
            const int page = todo[idx];
            if (page >= m_thumbs->count())
                continue;
            const QSizeF pt = m_session->current()->pageSize(page);
            const double s = std::min(112.0 / std::max(1.0, pt.width()), 150.0 / std::max(1.0, pt.height()));
            const QImage img = m_session->current()->render(page, s * devicePixelRatioF());
            QPixmap pm = QPixmap::fromImage(img);
            pm.setDevicePixelRatio(devicePixelRatioF());
            m_thumbs->item(page)->setIcon(QIcon(pm));
        }
        if (idx < todo.size())
            QTimer::singleShot(0, this, *step);
    };
    QTimer::singleShot(0, this, *step);
}

void PdfWorkspace::onDocumentChanged(const QSet<int>& pages)
{
    m_canvas->invalidatePages(pages);
    refreshThumbnails(pages);
    updateActions();
    if (!m_findBar->isHidden() && !m_findEdit->text().isEmpty())
        runFind();
    scheduleRecovery();
    Q_EMIT statusChanged();
}

void PdfWorkspace::scheduleRecovery()
{
    m_recoveryTimer->start();
}

void PdfWorkspace::undo()
{
    if (m_editKind != EditKind::None)
        cancelEdit();
    if (m_session && m_session->undo())
        m_banner->hide();
}

void PdfWorkspace::redo()
{
    if (m_editKind != EditKind::None)
        cancelEdit();
    if (m_session)
        m_session->redo();
}

// ---- editing -----------------------------------------------------------------

void PdfWorkspace::onCanvasClicked(int page, const QPointF& pdf, PdfEditSession::Hit hit, Qt::MouseButton button,
                                   const QPoint& global)
{
    if (!m_session || m_viewMode != ViewMode::Edit)
        return;
    if (m_editKind != EditKind::None)
        commitEdit();
    if (button == Qt::RightButton) {
        QMenu menu(this);
        if (hit.type == PdfEditSession::Hit::Block) {
            const BlockState st = m_session->blockState(page, hit.id);
            menu.addAction(Icons::get("text-cursor"), tr("Edit text"), this, [=, this] { startEditBlock(page, hit.id); })
                ->setEnabled(st.editable);
            menu.addAction(tr("Copy text"), this, [st] { QApplication::clipboard()->setText(st.text); });
            menu.addAction(Icons::get("trash"), tr("Delete paragraph"), this, [=, this] {
                EditOp op;
                op.page = page;
                op.blockId = hit.id;
                op.text.clear();
                QString err;
                if (!m_session->apply(op, &err))
                    m_banner->show(Banner::Kind::Error, err);
            })->setEnabled(st.editable);
            menu.addSeparator();
        } else if (hit.type == PdfEditSession::Hit::Added) {
            menu.addAction(Icons::get("text-cursor"), tr("Edit text"), this, [=, this] { startEditAdded(page, hit.id); });
            menu.addAction(Icons::get("trash"), tr("Delete text box"), this, [=, this] {
                for (const AddedItem& a : m_session->addedItems(page))
                    if (a.addedId == hit.id) {
                        EditOp op;
                        op.kind = EditOp::Kind::AddText;
                        op.page = page;
                        op.addedId = a.addedId;
                        op.style = a.style;
                        op.origin = a.origin;
                        op.text.clear();
                        m_session->apply(op);
                    }
            });
            menu.addSeparator();
        }
        menu.addAction(Icons::get("text-add"), tr("Add text here"), this, [=, this] { startNewText(page, pdf); });
        menu.addAction(Icons::get("image"), tr("Insert image here…"), this, [=, this] {
            const QString file = QFileDialog::getOpenFileName(this, tr("Insert image"), QString(),
                                                              tr("Images (*.png *.jpg *.jpeg *.bmp *.webp *.gif *.tif *.tiff)"));
            if (!file.isEmpty())
                insertImageAt(page, pdf, file);
        });
        menu.exec(global);
        return;
    }
    if (button != Qt::LeftButton)
        return;
    switch (m_tool) {
    case Tool::EditText:
        if (hit.type == PdfEditSession::Hit::Block) {
            if (m_session->blockState(page, hit.id).editable)
                startEditBlock(page, hit.id);
            else
                m_banner->show(Banner::Kind::Info, tr("This text is rotated or part of an OCR layer and cannot be edited in place."));
        } else if (hit.type == PdfEditSession::Hit::Added) {
            startEditAdded(page, hit.id);
        }
        break;
    case Tool::AddText:
        startNewText(page, pdf);
        m_toolEdit->setChecked(true);
        m_tool = Tool::EditText;
        m_canvas->setCrosshair(false);
        m_canvas->setHoverEnabled(true);
        break;
    case Tool::AddImage:
        insertImageAt(page, pdf, m_pendingImage);
        m_toolEdit->setChecked(true);
        setTool(Tool::EditText);
        m_banner->hide();
        break;
    }
}

void PdfWorkspace::demoEditFirstParagraph()
{
    if (m_session && !m_session->blocks(0).isEmpty())
        startEditBlock(0, 0);
}

void PdfWorkspace::startEditBlock(int page, int blockId)
{
    const BlockState st = m_session->blockState(page, blockId);
    m_editKind = EditKind::Block;
    m_editPage = page;
    m_editId = blockId;
    m_editStyle = st.style;
    m_editOriginalStyle = st.style;
    m_editor->setPlainText(st.text);
    m_canvas->setSelectedBlock(page, blockId);
    updateFormatControls(m_editStyle, true);
    applyStyleToEditor();
    m_editor->show();
    m_editor->setFocus();
    m_editor->selectAll();
    if (st.mixedStyle)
        m_banner->show(Banner::Kind::Warning,
                       tr("This paragraph mixes several text styles. Lines you change will use a single style."));
}

void PdfWorkspace::startEditAdded(int page, int addedId)
{
    for (const AddedItem& a : m_session->addedItems(page)) {
        if (a.addedId != addedId)
            continue;
        m_editKind = EditKind::Added;
        m_editPage = page;
        m_editId = addedId;
        m_editOrigin = a.origin;
        m_editWidth = a.width;
        m_editStyle = a.style;
        m_editOriginalStyle = a.style;
        m_editor->setPlainText(a.text);
        updateFormatControls(m_editStyle, true);
        applyStyleToEditor();
        m_editor->show();
        m_editor->setFocus();
        return;
    }
}

void PdfWorkspace::startNewText(int page, const QPointF& pdfTopLeft)
{
    m_editKind = EditKind::NewText;
    m_editPage = page;
    m_editId = m_session->nextAddedId();
    m_editOrigin = pdfTopLeft;
    m_editWidth = 0;
    TextStyle s;
    s.family = FontMatcher::availableFamilies().contains("Arial") ? "Arial" : FontMatcher::availableFamilies().value(0);
    s.size = 11;
    s.color = Qt::black;
    m_editStyle = s;
    m_editOriginalStyle = s;
    m_editor->clear();
    updateFormatControls(m_editStyle, true);
    applyStyleToEditor();
    m_editor->show();
    m_editor->setFocus();
}

void PdfWorkspace::applyStyleToEditor()
{
    QFont f(m_editStyle.family);
    f.setPixelSize(std::max(4, int(std::lround(m_editStyle.size * m_canvas->scale()))));
    f.setBold(m_editStyle.bold);
    f.setItalic(m_editStyle.italic);
    m_editor->setFont(f);
    QPalette pal = m_editor->palette();
    pal.setColor(QPalette::Base, QColor(255, 255, 255));
    pal.setColor(QPalette::Text, m_editStyle.color);
    m_editor->setPalette(pal);
    // The font must be part of the editor's own style sheet: the application-wide
    // style sheet sets a UI font on every widget and would override setFont().
    m_editor->setStyleSheet(QStringLiteral("QPlainTextEdit { border: 1.5px solid %1; background: #ffffff; color: %2; "
                                           "font-family: \"%3\"; font-size: %4px; font-weight: %5; font-style: %6; }")
                                .arg(tc().accent.name(), m_editStyle.color.name(), m_editStyle.family)
                                .arg(f.pixelSize())
                                .arg(m_editStyle.bold ? 700 : 400)
                                .arg(m_editStyle.italic ? "italic" : "normal"));
    QTextOption opt = m_editor->document()->defaultTextOption();
    opt.setAlignment(m_editStyle.align == TextStyle::Center ? Qt::AlignHCenter
                     : m_editStyle.align == TextStyle::Right ? Qt::AlignRight : Qt::AlignLeft);
    m_editor->document()->setDefaultTextOption(opt);
    positionEditor();
}

void PdfWorkspace::positionEditor()
{
    if (m_editKind == EditKind::None || !m_session)
        return;
    QRectF r;
    if (m_editKind == EditKind::Block)
        r = m_canvas->pdfRectToWidget(m_editPage, m_session->blockState(m_editPage, m_editId).bounds);
    else {
        const QPointF tl = m_canvas->pdfToWidget(m_editPage, m_editOrigin);
        r = QRectF(tl, QSizeF(m_editWidth > 0 ? m_editWidth * m_canvas->scale() : 200, 20));
    }
    // Grow to fit the text being typed.
    const QFontMetrics fm(m_editor->font());
    int w = int(r.width()) + 16;
    const QStringList lines = m_editor->toPlainText().split('\n');
    for (const QString& line : lines)
        w = std::max(w, fm.horizontalAdvance(line) + 24);
    const int h = std::max(int(r.height()) + 10, int(lines.size()) * fm.lineSpacing() + 12);
    const QRect page = m_canvas->pageRect(m_editPage);
    w = std::min(w, page.right() - int(r.left()) + 40);
    m_editor->setGeometry(int(r.left()) - 6, int(r.top()) - 5, std::max(w, 60), h);
}

TextStyle PdfWorkspace::currentEditStyle() const
{
    return m_editStyle;
}

void PdfWorkspace::commitEdit()
{
    if (m_editKind == EditKind::None || !m_session)
        return;
    const EditKind kind = m_editKind;
    m_editKind = EditKind::None;
    m_editor->hide();
    m_canvas->setSelectedBlock(-1, -1);
    updateFormatControls({}, false);
    QString text = m_editor->toPlainText();
    text.replace(QChar(0x2029), '\n');
    EditOp op;
    op.page = m_editPage;
    op.text = text;
    op.style = m_editStyle;
    op.styleChanged = m_editStyle != m_editOriginalStyle;
    if (kind == EditKind::Block) {
        op.kind = EditOp::Kind::ReplaceBlock;
        op.blockId = m_editId;
        const BlockState st = m_session->blockState(m_editPage, m_editId);
        if (st.text == text && !op.styleChanged)
            return; // nothing changed
        if (st.edited && !op.styleChanged && st.style != m_editOriginalStyle)
            op.styleChanged = true;
    } else {
        op.kind = EditOp::Kind::AddText;
        op.addedId = m_editId;
        op.origin = m_editOrigin;
        op.width = m_editWidth;
        op.styleChanged = true;
        if (kind == EditKind::NewText && text.trimmed().isEmpty())
            return;
    }
    QString err;
    if (!m_session->apply(op, &err)) {
        // Nothing was changed in the document. Keep the user's text in the editor so it can be
        // shortened or copied, and offer the reconstructed (reflow) editing path.
        m_editKind = kind;
        updateFormatControls(m_editStyle, true);
        applyStyleToEditor();
        m_editor->show();
        m_editor->setFocus();
        if (kind == EditKind::Block)
            m_canvas->setSelectedBlock(m_editPage, m_editId);
        m_banner->show(Banner::Kind::Error, tr("This change was not applied: %1").arg(err),
                       {tr("Use Reflow edit")});
        disconnect(m_banner, &Banner::actionTriggered, nullptr, nullptr);
        connect(m_banner, &Banner::actionTriggered, this, [this](int) {
            cancelEdit();
            if (!isModified())
                Q_EMIT reflowRequested(filePath());
            else
                QMessageBox::information(this, tr("Reflow editing"),
                                         tr("Save or undo your other changes first, then switch to Reflow edit."));
        });
        return;
    }
    showWarnings(m_session->lastWarnings());
    m_canvas->setFocus();
}

void PdfWorkspace::cancelEdit()
{
    if (m_editKind == EditKind::None)
        return;
    m_editKind = EditKind::None;
    m_editor->hide();
    m_canvas->setSelectedBlock(-1, -1);
    updateFormatControls({}, false);
    m_canvas->setFocus();
}

void PdfWorkspace::showWarnings(const QStringList& warnings)
{
    if (warnings.isEmpty()) {
        m_banner->hide();
        m_warningsLabel->clear();
        return;
    }
    m_banner->show(Banner::Kind::Warning, warnings.join("\n"));
    m_warningsLabel->setText(tr("Last edit:") + "\n• " + warnings.join("\n• "));
}

void PdfWorkspace::insertImageAt(int page, const QPointF& pdfPoint, const QString& file)
{
    QImageReader reader(file);
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull()) {
        m_banner->show(Banner::Kind::Error, tr("The image could not be read: %1").arg(reader.errorString()));
        return;
    }
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    const QSizeF pageSize = m_session->current()->pageSize(page);
    // Natural size at 96 dpi, limited to half of the page width.
    QSizeF size(img.width() * 72.0 / 96.0, img.height() * 72.0 / 96.0);
    if (size.width() > pageSize.width() * 0.5)
        size.scale(pageSize.width() * 0.5, 1e9, Qt::KeepAspectRatio);
    EditOp op;
    op.kind = EditOp::Kind::AddImage;
    op.page = page;
    op.rect = QRectF(pdfPoint.x(), pdfPoint.y() - size.height(), size.width(), size.height());
    op.image = png;
    QString err;
    if (!m_session->apply(op, &err))
        m_banner->show(Banner::Kind::Error, err);
}

bool PdfWorkspace::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_scroll->viewport() && event->type() == QEvent::Resize && m_canvas)
        m_canvas->relayout();
    return QWidget::eventFilter(watched, event);
}

// ---- find & replace --------------------------------------------------------------

void PdfWorkspace::showFind(bool replace)
{
    if (!m_session)
        return;
    m_findBar->show();
    m_replaceRow->setVisible(replace);
    m_findEdit->setFocus();
    m_findEdit->selectAll();
    runFind();
}

void PdfWorkspace::runFind()
{
    m_matches.clear();
    m_matchIndex = -1;
    const QString needle = m_findEdit->text();
    if (!m_session || needle.isEmpty()) {
        m_findStatus->clear();
        m_canvas->setHighlights({});
        return;
    }
    const Qt::CaseSensitivity cs = m_caseSensitive->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QHash<int, QList<QRectF>> hl;
    for (int p = 0; p < pageCount(); ++p) {
        for (const TextBlock& b : m_session->blocks(p)) {
            const BlockState st = m_session->blockState(p, b.id);
            if (st.editable && st.text.contains(needle, cs)) {
                m_matches.append({p, b.id, false, st.bounds});
                hl[p] << st.bounds;
            }
        }
        for (const AddedItem& a : m_session->addedItems(p))
            if (a.text.contains(needle, cs)) {
                m_matches.append({p, a.addedId, true, a.bounds});
                hl[p] << a.bounds;
            }
    }
    m_canvas->setHighlights(hl);
    m_findStatus->setText(m_matches.isEmpty() ? tr("No matches") : tr("%n paragraph(s)", nullptr, int(m_matches.size())));
}

void PdfWorkspace::findNext(bool backwards)
{
    if (m_matches.isEmpty())
        return;
    m_matchIndex = backwards ? (m_matchIndex <= 0 ? int(m_matches.size()) - 1 : m_matchIndex - 1)
                             : (m_matchIndex + 1) % int(m_matches.size());
    const Match& m = m_matches[m_matchIndex];
    const QRectF w = m_canvas->pdfRectToWidget(m.page, m.rect);
    m_scroll->ensureVisible(int(w.center().x()), int(w.center().y()), 80, int(m_scroll->viewport()->height() / 3));
    m_canvas->setSelectedBlock(m.added ? -1 : m.page, m.added ? -1 : m.blockId);
    m_findStatus->setText(tr("%1 of %2").arg(m_matchIndex + 1).arg(m_matches.size()));
}

void PdfWorkspace::replaceCurrent()
{
    if (m_matches.isEmpty())
        return;
    if (m_matchIndex < 0)
        findNext(false);
    const Match m = m_matches[m_matchIndex];
    const Qt::CaseSensitivity cs = m_caseSensitive->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QString err;
    if (!m.added) {
        BlockState st = m_session->blockState(m.page, m.blockId);
        EditOp op;
        op.page = m.page;
        op.blockId = m.blockId;
        op.text = st.text.replace(m_findEdit->text(), m_replaceEdit->text(), cs);
        if (!m_session->apply(op, &err))
            m_banner->show(Banner::Kind::Error, err);
        else
            showWarnings(m_session->lastWarnings());
    }
    runFind();
}

void PdfWorkspace::replaceAll()
{
    if (m_matches.isEmpty())
        return;
    const Qt::CaseSensitivity cs = m_caseSensitive->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QList<EditOp> ops;
    for (const Match& m : std::as_const(m_matches)) {
        if (m.added)
            continue;
        BlockState st = m_session->blockState(m.page, m.blockId);
        EditOp op;
        op.page = m.page;
        op.blockId = m.blockId;
        op.text = st.text.replace(m_findEdit->text(), m_replaceEdit->text(), cs);
        ops << op;
    }
    QString err;
    const int n = int(ops.size());
    if (!m_session->applyBatch(ops, &err)) {
        m_banner->show(Banner::Kind::Error, err);
        return;
    }
    m_banner->show(Banner::Kind::Success, tr("Replaced text in %n paragraph(s). Undo reverts all of them at once.", nullptr, n));
    runFind();
}

// ---- OCR ---------------------------------------------------------------------------

void PdfWorkspace::startOcr()
{
    if (!m_session)
        return;
    QString why;
    if (!TesseractEngine::isAvailable(&why)) {
        QMessageBox::warning(this, tr("OCR unavailable"), why);
        return;
    }
    if (m_ocrWatcher && m_ocrWatcher->isRunning())
        return;
    m_banner->show(Banner::Kind::Info, tr("Recognizing text… This can take a while for long documents."), {tr("Cancel")});
    disconnect(m_banner, &Banner::actionTriggered, nullptr, nullptr);
    m_ocrCancel = std::make_shared<std::atomic_bool>(false);
    connect(m_banner, &Banner::actionTriggered, this, [this](int) {
        if (m_ocrCancel)
            m_ocrCancel->store(true);
    });
    const QByteArray bytes = m_session->original()->sourceData();
    const QByteArray password = m_session->original()->password();
    const int pages = pageCount();
    const QString langs = TesseractEngine::defaultLanguages();
    auto cancel = m_ocrCancel;
    QPointer<PdfWorkspace> self(this);
    if (!m_ocrWatcher) {
        m_ocrWatcher = new QFutureWatcher<QHash<int, OcrPageResult>>(this);
        connect(m_ocrWatcher, &QFutureWatcherBase::finished, this, &PdfWorkspace::onOcrFinished);
    }
    m_ocrWatcher->setFuture(QtConcurrent::run([bytes, password, pages, langs, cancel, self]() {
        QHash<int, OcrPageResult> results;
        auto doc = PdfDocument::openData(bytes, password);
        if (!doc)
            return results;
        const QString work = PathUtils::tempRoot() + "/ocr-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (int p = 0; p < pages && !cancel->load(); ++p) {
            if (self)
                QMetaObject::invokeMethod(self, [self, p, pages] {
                    if (self)
                        Q_EMIT self->busy(QObject::tr("OCR page %1 of %2").arg(p + 1).arg(pages), int(100.0 * p / pages));
                }, Qt::QueuedConnection);
            const QImage img = doc->render(p, 300.0 / 72.0, false).convertToFormat(QImage::Format_Grayscale8);
            results.insert(p, TesseractEngine::recognize(img, langs, work, cancel.get()));
        }
        QDir(work).removeRecursively();
        return results;
    }));
    Q_EMIT busy(tr("Recognizing text…"), 0);
}

void PdfWorkspace::onOcrFinished()
{
    Q_EMIT busy(QString(), 100);
    if (!m_session)
        return;
    const QHash<int, OcrPageResult> results = m_ocrWatcher->result();
    if (m_ocrCancel && m_ocrCancel->load()) {
        m_banner->show(Banner::Kind::Info, tr("Text recognition was cancelled."));
        return;
    }
    m_ocr.clear();
    QHash<int, QList<QRectF>> low;
    int total = 0, doubtful = 0;
    double sum = 0;
    QString firstError;
    for (auto it = results.cbegin(); it != results.cend(); ++it) {
        const int page = it.key();
        const OcrPageResult& r = it.value();
        if (!r.ok) {
            if (firstError.isEmpty())
                firstError = r.error;
            continue;
        }
        // Image pixels -> PDF space.
        const QTransform toPdf = m_session->original()->pageToDevice(page, r.imageSize).inverted();
        QList<OcrWordState> words;
        for (const OcrWord& w : r.words) {
            const QRectF pdfRect = toPdf.mapRect(QRectF(w.box));
            words.append({w.text, pdfRect, w.confidence});
            ++total;
            sum += w.confidence;
            if (w.confidence < 70) {
                ++doubtful;
                low[page] << pdfRect;
            }
        }
        m_ocr.insert(page, words);
    }
    if (m_ocr.isEmpty()) {
        m_banner->show(Banner::Kind::Error, tr("Text recognition failed: %1").arg(firstError));
        return;
    }
    m_canvas->setLowConfidence(low);
    m_ocrBox->show();
    m_ocrSummary->setText(tr("%1 words recognized, average confidence %2%. %3 word(s) are uncertain and highlighted "
                             "in orange. OCR can make mistakes: review and correct the words below before applying.")
                              .arg(total)
                              .arg(total ? int(sum / total) : 0)
                              .arg(doubtful));
    populateOcrTable();
    m_banner->show(Banner::Kind::Success, tr("Text recognized. Review uncertain words, then choose \"Apply text layer\"."));
}

void PdfWorkspace::populateOcrTable()
{
    QSignalBlocker block(m_ocrTable);
    m_ocrTable->setRowCount(0);
    const bool onlyDoubtful = m_ocrOnlyDoubtful->isChecked();
    QList<int> pages = m_ocr.keys();
    std::sort(pages.begin(), pages.end());
    for (int page : pages) {
        const auto& words = m_ocr[page];
        for (int i = 0; i < words.size(); ++i) {
            if (onlyDoubtful && words[i].confidence >= 70)
                continue;
            const int row = m_ocrTable->rowCount();
            m_ocrTable->insertRow(row);
            auto* p = new QTableWidgetItem(QString::number(page + 1));
            p->setFlags(p->flags() & ~Qt::ItemIsEditable);
            auto* t = new QTableWidgetItem(words[i].text);
            t->setData(Qt::UserRole, page);
            t->setData(Qt::UserRole + 1, i);
            auto* c = new QTableWidgetItem(QStringLiteral("%1%").arg(int(words[i].confidence)));
            c->setFlags(c->flags() & ~Qt::ItemIsEditable);
            if (words[i].confidence < 70)
                c->setForeground(tc().warning);
            m_ocrTable->setItem(row, 0, p);
            m_ocrTable->setItem(row, 1, t);
            m_ocrTable->setItem(row, 2, c);
        }
    }
}

void PdfWorkspace::applyOcrLayer()
{
    if (!m_session || m_ocr.isEmpty())
        return;
    QList<EditOp> ops;
    for (auto it = m_ocr.cbegin(); it != m_ocr.cend(); ++it) {
        EditOp op;
        op.kind = EditOp::Kind::OcrLayer;
        op.page = it.key();
        for (const OcrWordState& w : it.value())
            if (!w.text.isEmpty())
                op.words.append({w.text, w.rect});
        ops << op;
    }
    QString err;
    if (!m_session->applyBatch(ops, &err)) {
        m_banner->show(Banner::Kind::Error, err);
        return;
    }
    m_canvas->setLowConfidence({});
    m_banner->show(Banner::Kind::Success,
                   tr("An invisible, searchable text layer was added. The scanned page images are unchanged. "
                      "Save to keep it."));
}

// ---- save / export ---------------------------------------------------------------

bool PdfWorkspace::save()
{
    if (!m_session)
        return false;
    if (m_editKind != EditKind::None)
        commitEdit();
    if (!m_session->isModified())
        return true;
    PdfEditSession::SaveOptions opts;
    opts.createBackup = Database::instance().setting("backup.enabled", true).toBool();
    if (Database::instance().setting("backup.location", "appdata").toString() == "beside")
        opts.backupDir = QFileInfo(filePath()).absolutePath();

    if (m_session->hasExternalChange()) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("File changed on disk"));
        box.setText(tr("\"%1\" was modified by another program after you opened it.").arg(QFileInfo(filePath()).fileName()));
        box.setInformativeText(tr("Overwriting it would discard those changes. Save your version as a separate file instead?"));
        auto* saveAsBtn = box.addButton(tr("Save As…"), QMessageBox::AcceptRole);
        auto* overwrite = box.addButton(tr("Overwrite"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == saveAsBtn)
            return saveAs();
        if (box.clickedButton() != overwrite)
            return false;
        opts.ignoreExternalChange = true;
    }

    const QStringList warnings = m_session->preSaveWarnings();
    const bool confirmSetting = Database::instance().setting("save.confirmFirst", true).toBool();
    if ((!m_confirmedOverwrite && confirmSetting) || !warnings.isEmpty()) {
        QMessageBox box(this);
        box.setIcon(warnings.isEmpty() ? QMessageBox::Question : QMessageBox::Warning);
        box.setWindowTitle(tr("Save to original file"));
        box.setText(tr("Save your changes into the original file?"));
        QString info = QDir::toNativeSeparators(filePath()) + "\n\n";
        info += opts.createBackup ? tr("A backup copy of the current file is kept before it is replaced.")
                                  : tr("Backups are turned off in Settings.");
        for (const QString& w : warnings)
            info += "\n\n⚠ " + w;
        box.setInformativeText(info);
        box.setStandardButtons(QMessageBox::Save | QMessageBox::Cancel);
        box.setDefaultButton(warnings.isEmpty() ? QMessageBox::Save : QMessageBox::Cancel);
        if (box.exec() != QMessageBox::Save)
            return false;
        m_confirmedOverwrite = true;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    Q_EMIT busy(tr("Saving…"), -1);
    const auto r = m_session->save(opts);
    QApplication::restoreOverrideCursor();
    Q_EMIT busy(QString(), 100);
    if (!r.ok) {
        QString msg = r.error;
        if (r.status == SafeFileWriter::Status::RestoredAfterFailure)
            msg += "\n\n" + tr("The original file was restored from the backup.");
        else
            msg += "\n\n" + tr("The original file was not changed.");
        QMessageBox::critical(this, tr("Save failed"), msg);
        return false;
    }
    m_banner->show(Banner::Kind::Success, r.backupPath.isEmpty()
                                              ? tr("Saved to %1.").arg(QFileInfo(filePath()).fileName())
                                              : tr("Saved to %1. Backup: %2").arg(QFileInfo(filePath()).fileName(),
                                                                                  QDir::toNativeSeparators(r.backupPath)));
    updateActions();
    Q_EMIT savedTo(filePath());
    Q_EMIT statusChanged();
    return true;
}

bool PdfWorkspace::saveAs()
{
    if (!m_session)
        return false;
    if (m_editKind != EditKind::None)
        commitEdit();
    const QFileInfo fi(filePath());
    const QString suggestion = PathUtils::uniquePath(fi.absolutePath() + "/" + fi.completeBaseName() + " (edited).pdf");
    QString path = QFileDialog::getSaveFileName(this, tr("Save PDF as"), suggestion, tr("PDF documents (*.pdf)"));
    if (path.isEmpty())
        return false;
    if (!path.endsWith(".pdf", Qt::CaseInsensitive))
        path += ".pdf";
    if (PathUtils::samePath(path, filePath()))
        return save();
    const bool exists = QFileInfo::exists(path); // the dialog already confirmed overwriting
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto r = m_session->saveAs(path, exists);
    QApplication::restoreOverrideCursor();
    if (!r.ok) {
        QMessageBox::critical(this, tr("Save failed"), r.error);
        return false;
    }
    m_banner->show(Banner::Kind::Success, tr("Saved a copy to %1. The original file is unchanged.").arg(QDir::toNativeSeparators(path)),
                   {tr("Show in folder")});
    disconnect(m_banner, &Banner::actionTriggered, nullptr, nullptr);
    connect(m_banner, &Banner::actionTriggered, this, [path](int) { revealInExplorer(path); });
    Database::instance().touchRecent(path, "pdf", "edit");
    Q_EMIT savedTo(path);
    return true;
}

void PdfWorkspace::exportAs(const QString& format)
{
    if (!m_session)
        return;
    if (m_editKind != EditKind::None)
        commitEdit();
    const QFileInfo fi(filePath());
    QString target;
    if (format == "png") {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Export page images to folder"), fi.absolutePath());
        if (dir.isEmpty())
            return;
        target = dir + "/" + fi.completeBaseName() + ".png";
    } else {
        const QString filter = format == "docx" ? tr("Word documents (*.docx)")
                               : format == "txt" ? tr("Text files (*.txt)") : tr("HTML files (*.html)");
        target = QFileDialog::getSaveFileName(this, tr("Export"), PathUtils::uniquePath(fi.absolutePath() + "/" + fi.completeBaseName() + "." + format), filter);
        if (target.isEmpty())
            return;
    }
    QByteArray bytes;
    QString err;
    if (!m_session->serialize(&bytes, &err)) {
        QMessageBox::critical(this, tr("Export failed"), err);
        return;
    }
    if (format == "txt" || format == "html") {
        auto doc = PdfDocument::openData(bytes, m_session->original()->password());
        QString out;
        if (format == "txt") {
            out = doc->allText().replace(QChar(0x0C), "\n\n");
        } else {
            out = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>" + fi.completeBaseName().toHtmlEscaped()
                  + "</title></head><body>\n";
            for (int p = 0; p < doc->pageCount(); ++p) {
                out += QStringLiteral("<section data-page=\"%1\">\n").arg(p + 1);
                for (const TextBlock& b : PdfTextLayout::build(doc->textRuns(p), p, doc->horizontalRules(p)))
                    out += "<p>" + b.text().toHtmlEscaped().replace("\n", "<br>") + "</p>\n";
                out += "</section>\n";
            }
            out += "</body></html>\n";
        }
        const auto res = SafeFileWriter::writeNew(
            target,
            [&out](const QString& tmp, QString* e) {
                QFile f(tmp);
                if (!f.open(QIODevice::WriteOnly) || f.write(out.toUtf8()) < 0) {
                    *e = f.errorString();
                    return false;
                }
                return true;
            },
            {}, true);
        if (!res.ok())
            QMessageBox::critical(this, tr("Export failed"), res.error);
        else
            m_banner->show(Banner::Kind::Success, tr("Exported to %1").arg(QDir::toNativeSeparators(target)));
        return;
    }
    // DOCX / PNG through the conversion engine, in the background.
    const QString tmpPdf = PathUtils::tempRoot() + "/export-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".pdf";
    QFile f(tmpPdf);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) {
        QMessageBox::critical(this, tr("Export failed"), f.errorString());
        return;
    }
    f.close();
    ConversionRequest req;
    req.inputPath = tmpPdf;
    req.inputFormat = "pdf";
    req.outputFormat = format;
    req.outputPath = format == "png" ? target : PathUtils::uniquePath(target);
    if (format != "png" && QFileInfo::exists(target))
        req.outputPath = target + ".tmp-export";
    const ConversionPlan plan = ConversionPlanner().plan("pdf", format);
    if (!plan.isValid()) {
        QFile::remove(tmpPdf);
        QMessageBox::warning(this, tr("Export"), tr("No installed engine can export PDF to %1.").arg(format.toUpper()));
        return;
    }
    if (format == "docx")
        m_banner->show(Banner::Kind::Info, tr("Exporting to Word reconstructs the layout (%1). The result is editable "
                                              "but may not look identical.").arg(plan.steps.first().converter->displayName()));
    auto* watcher = new QFutureWatcher<ConversionResult>(this);
    Q_EMIT busy(tr("Exporting to %1…").arg(format.toUpper()), -1);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, tmpPdf, target, req, format] {
        watcher->deleteLater();
        QFile::remove(tmpPdf);
        Q_EMIT busy(QString(), 100);
        ConversionResult r = watcher->result();
        if (r.ok && format != "png" && req.outputPath != target && !r.outputs.isEmpty()) {
            // The user confirmed overwriting in the dialog: swap in safely.
            const QString produced = r.outputs.first();
            const auto res = SafeFileWriter::replace(target, [produced](const QString& tmp, QString*) {
                return QFile::copy(produced, tmp);
            }, {}, {});
            QFile::remove(produced);
            r.outputs = {target};
            r.ok = res.ok();
            r.error = res.error;
        }
        if (!r.ok) {
            QMessageBox::critical(this, tr("Export failed"), r.error);
            return;
        }
        m_banner->show(Banner::Kind::Success, tr("Exported %n file(s).", nullptr, int(r.outputs.size())), {tr("Show in folder")});
        disconnect(m_banner, &Banner::actionTriggered, nullptr, nullptr);
        const QString first = r.outputs.value(0);
        connect(m_banner, &Banner::actionTriggered, this, [first](int) { revealInExplorer(first); });
    });
    watcher->setFuture(QtConcurrent::run([req, plan] {
        std::atomic_bool cancel{false};
        return ConversionExecutor::execute(req, plan, &cancel, {});
    }));
}

} // namespace folvio
