#include "combine/CombineWorkspace.h"

#include "combine/AnnotationRender.h"
#include "combine/ImageGeometry.h"
#include "combine/PageGridView.h"
#include "core/persistence/Database.h"
#include "editor/pdf_native/FontMatcher.h"
#include "ui/themes/Theme.h"
#include "ui/widgets/CommonWidgets.h"
#include "ui/widgets/Icons.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace folvio::combine {

namespace {

constexpr double kA4WidthPt = 595.0; // width-related UI units: "pt at A4 width"

QIcon colorIcon(const QColor& c)
{
    QPixmap pm(18, 18);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(tc().borderStrong, 1));
    p.setBrush(c);
    p.drawRoundedRect(QRectF(1.5, 1.5, 15, 15), 3, 3);
    return QIcon(pm);
}

QImage makeThumbnail(const CombinePage& page)
{
    QString err;
    QImage img = ImageProcessor::render(page, ImageProcessor::Stage::Final, 420, &err);
    if (img.isNull()) {
        QImage broken(150, 200, QImage::Format_RGB32);
        broken.fill(QColor("#f4d6d3"));
        QPainter p(&broken);
        p.drawText(broken.rect(), Qt::AlignCenter | Qt::TextWordWrap, QObject::tr("Image\nunavailable"));
        return broken;
    }
    img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (!page.annotations.isEmpty()) {
        QPainter p(&img);
        p.scale(img.width(), img.width());
        AnnotationRender::paintAll(p, page.annotations);
    }
    return img.scaled(220, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// Thumbnail tile: image, page number, name, "edited" marker.
class PageDelegate : public QStyledItemDelegate {
public:
    explicit PageDelegate(bool compact, QObject* parent) : QStyledItemDelegate(parent), m_compact(compact) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        return m_compact ? QSize(150, 168) : QSize(176, 214);
    }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        const ThemeColors& c = tc();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF cell = QRectF(opt.rect).adjusted(4, 4, -4, -4);
        const bool selected = opt.state & QStyle::State_Selected;
        p->setPen(QPen(selected ? c.accent : c.border, selected ? 2 : 1));
        p->setBrush(selected ? c.accentSoft : c.surface);
        p->drawRoundedRect(cell, 5, 5);
        const QRectF imgArea = cell.adjusted(8, 8, -8, -(m_compact ? 26 : 40));
        const QPixmap pm = idx.data(Qt::DecorationRole).value<QPixmap>();
        if (!pm.isNull()) {
            QSizeF s = pm.size();
            s.scale(imgArea.size(), Qt::KeepAspectRatio);
            const QRectF target(imgArea.center() - QPointF(s.width() / 2, s.height() / 2), s);
            p->fillRect(target.translated(0, 1.5), QColor(0, 0, 0, 30));
            p->drawPixmap(target, pm, pm.rect());
        } else {
            p->setPen(c.textMuted);
            p->drawText(imgArea, Qt::AlignCenter, QObject::tr("Loading…"));
        }
        // Page number badge
        const QRectF badge(cell.left() + 6, cell.top() + 6, 26, 18);
        p->setPen(Qt::NoPen);
        p->setBrush(c.accent);
        p->drawRoundedRect(badge, 3, 3);
        p->setPen(c.accentText);
        QFont f = opt.font;
        f.setBold(true);
        f.setPointSizeF(8);
        p->setFont(f);
        p->drawText(badge, Qt::AlignCenter, QString::number(idx.row() + 1));
        if (idx.data(PageListModel::EditedRole).toBool()) {
            const QRectF dot(cell.right() - 20, cell.top() + 7, 14, 14);
            p->setBrush(c.warning);
            p->setPen(Qt::NoPen);
            p->drawEllipse(dot);
            p->setPen(Qt::white);
            p->drawText(dot, Qt::AlignCenter, QStringLiteral("✎"));
        }
        // Name and summary
        f.setBold(false);
        f.setPointSizeF(8.5);
        p->setFont(f);
        p->setPen(c.text);
        const QString name = idx.data(Qt::DisplayRole).toString().section(QStringLiteral(". "), 1);
        const QRectF nameRect(cell.left() + 8, cell.bottom() - (m_compact ? 22 : 36), cell.width() - 16, 16);
        p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, opt.fontMetrics.elidedText(name, Qt::ElideMiddle, int(nameRect.width())));
        if (!m_compact) {
            p->setPen(c.textMuted);
            const QString sum = idx.data(PageListModel::SummaryRole).toString();
            p->drawText(nameRect.translated(0, 16), Qt::AlignLeft | Qt::AlignVCenter,
                        opt.fontMetrics.elidedText(sum.isEmpty() ? QObject::tr("unchanged") : sum, Qt::ElideRight, int(nameRect.width())));
        }
        p->restore();
    }

private:
    bool m_compact;
};

QToolButton* toolButton(const QString& icon, const QString& text, const QString& tip, QWidget* parent)
{
    auto* b = makeToolButton(icon, tip, parent, true);
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    return b;
}

QLabel* sectionTitle(const QString& t, QWidget* parent)
{
    auto* l = new QLabel(t, parent);
    l->setObjectName("SectionTitle");
    return l;
}

QLabel* muted(const QString& t, QWidget* parent)
{
    auto* l = new QLabel(t, parent);
    l->setWordWrap(true);
    l->setProperty("muted", true);
    return l;
}

} // namespace

// ===========================================================================

CombineWorkspace::CombineWorkspace(QWidget* parent) : QWidget(parent)
{
    setAcceptDrops(true);
    m_doc = new CombineDocument(this);
    m_model = new PageListModel(m_doc, this);
    // Styles per tool (page units; widths shown as "pt at A4 width").
    m_penStyle.width = 2.0 / kA4WidthPt;
    m_penStyle.stroke = QColor("#1f4fbf");
    m_highlighterStyle.width = 14.0 / kA4WidthPt;
    m_highlighterStyle.stroke = QColor("#ffd400");
    m_highlighterStyle.opacity = 0.4;
    m_highlighterStyle.smooth = true;
    m_shapeStyle.width = 2.0 / kA4WidthPt;
    m_shapeStyle.stroke = QColor("#d32f2f");
    m_shapeStyle.fill = QColor("#ffeb3b");
    m_textStyle.stroke = QColor("#111111");
    m_textStyle.fontSize = 14.0 / kA4WidthPt;
    const QStringList families = FontMatcher::availableFamilies();
    m_textStyle.fontFamily = families.contains("Arial") ? QStringLiteral("Arial") : families.value(0, QStringLiteral("Arial"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildHeader());
    auto* bannerWrap = new QWidget(this);
    auto* bl = new QVBoxLayout(bannerWrap);
    bl->setContentsMargins(16, 6, 16, 0);
    m_banner = new Banner(bannerWrap);
    bl->addWidget(m_banner);
    root->addWidget(bannerWrap);
    m_steps = new QStackedWidget(this);
    m_steps->addWidget(buildArrange());
    m_steps->addWidget(buildEdit());
    m_steps->addWidget(buildExport());
    root->addWidget(m_steps, 1);

    connect(m_model, &PageListModel::thumbnailNeeded, this, &CombineWorkspace::requestThumbnail);
    connect(m_model, &PageListModel::externalFilesDropped, this, [this](const QStringList& files, int row) { importFiles(files, row); });
    connect(m_doc, &CombineDocument::pageChanged, this, [this](const QUuid& id) {
        m_model->invalidateThumbnail(id);
        const int row = m_doc->indexOf(id);
        if (row >= 0)
            Q_EMIT m_model->dataChanged(m_model->index(row), m_model->index(row));
    });
    connect(m_doc, &CombineDocument::pagesChanged, this, [this] {
        updateActions();
        if (m_step == Step::Export)
            refreshExportSummary();
    });
    connect(m_doc, &CombineDocument::historyChanged, this, [this] {
        updateActions();
        scheduleAutosave();
    });

    m_autosave = new QTimer(this);
    m_autosave->setSingleShot(true);
    m_autosave->setInterval(800);
    connect(m_autosave, &QTimer::timeout, this, [this] {
        if (m_doc->isEmpty())
            CombineSession::discardSession(nullptr);
        else
            CombineSession::autosave(*m_doc);
    });

    // Shortcuts scoped to this workspace. Text fields consume Ctrl+V / Ctrl+Z themselves
    // (ShortcutOverride), so these never steal input from an active text field.
    auto shortcut = [this](const QKeySequence& k, auto fn) {
        auto* s = new QShortcut(k, this);
        s->setContext(Qt::WidgetWithChildrenShortcut);
        connect(s, &QShortcut::activated, this, fn);
        return s;
    };
    shortcut(QKeySequence::Paste, [this] { pasteFromClipboard(); });
    shortcut(QKeySequence::Open, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add images"), QString(),
                                                                tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff *.gif *.heic *.heif *.avif)"));
        importFiles(files);
    });
    shortcut(QKeySequence::Undo, [this] {
        if (!m_canvas->isEditingText())
            m_doc->undo();
    });
    // Ctrl+Y and Ctrl+Shift+Z on every platform, as one shortcut (duplicates would be ambiguous).
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Y), [this] { m_doc->redo(); })
        ->setKeys({QKeySequence(Qt::CTRL | Qt::Key_Y), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    shortcut(QKeySequence::ZoomIn, [this] { zoomBy(1.15); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal), [this] { zoomBy(1.15); });
    shortcut(QKeySequence::ZoomOut, [this] { zoomBy(1 / 1.15); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_0), [this] { fitPage(); });
    shortcut(QKeySequence(Qt::Key_PageDown), [this] {
        if (m_step == Step::Edit)
            selectPageInEdit(m_doc->indexOf(m_canvas->pageId()) + 1);
    });
    shortcut(QKeySequence(Qt::Key_PageUp), [this] {
        if (m_step == Step::Edit)
            selectPageInEdit(m_doc->indexOf(m_canvas->pageId()) - 1);
    });

    setStep(Step::Arrange);
    updateActions();
}

CombineWorkspace::~CombineWorkspace()
{
    if (m_exportCancel)
        m_exportCancel->store(true);
    if (m_exportWatcher)
        m_exportWatcher->waitForFinished();
    QThreadPool::globalInstance()->waitForDone(5000);
}

// ---- header ---------------------------------------------------------------------

QWidget* CombineWorkspace::buildHeader()
{
    auto* bar = new QWidget(this);
    bar->setObjectName("Toolbar");
    auto* l = new QHBoxLayout(bar);
    l->setContentsMargins(16, 6, 12, 6);
    l->setSpacing(6);
    auto* title = new QLabel(tr("Combine"), bar);
    title->setObjectName("DocTitle");
    l->addWidget(title);
    l->addSpacing(14);
    m_stepButtons = new QButtonGroup(this);
    const QStringList names = {tr("1  Arrange"), tr("2  Edit"), tr("3  Export")};
    const QStringList icons = {"batch", "edit", "export"};
    for (int i = 0; i < 3; ++i) {
        auto* b = new QToolButton(bar);
        b->setText(names[i]);
        b->setIcon(Icons::get(icons[i]));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setMinimumWidth(110);
        m_stepButtons->addButton(b, i);
        l->addWidget(b);
        if (i < 2) {
            auto* arrow = new QLabel(bar);
            arrow->setPixmap(Icons::pixmap("chevron-right", 14, Icons::Tone::Muted, devicePixelRatioF()));
            l->addWidget(arrow);
        }
    }
    connect(m_stepButtons, &QButtonGroup::idClicked, this, [this](int id) { setStep(Step(id)); });
    l->addStretch();
    m_undo = makeToolButton("undo", tr("Undo (Ctrl+Z)"), bar);
    m_redo = makeToolButton("redo", tr("Redo (Ctrl+Y)"), bar);
    connect(m_undo, &QToolButton::clicked, m_doc, &CombineDocument::undo);
    connect(m_redo, &QToolButton::clicked, m_doc, &CombineDocument::redo);
    auto* project = makeToolButton("folder", tr("Project"), bar);
    project->setText(tr("Project"));
    project->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    project->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(project);
    menu->addAction(tr("New combine…"), this, &CombineWorkspace::newProject);
    menu->addAction(tr("Open project (.folvio)…"), this, &CombineWorkspace::openProject);
    m_saveProjectAction = menu->addAction(tr("Save project as…"), this, &CombineWorkspace::saveProject);
    project->setMenu(menu);
    l->addWidget(m_undo);
    l->addWidget(m_redo);
    l->addWidget(makeSeparator(bar));
    l->addWidget(project);
    return bar;
}

void CombineWorkspace::setStep(Step s)
{
    if (m_canvas && m_canvas->isEditingText())
        m_canvas->commitText();
    if (s != Step::Arrange && m_doc->isEmpty()) {
        m_banner->show(Banner::Kind::Info, tr("Add images first: drop them here, choose files, or paste with Ctrl+V."));
        s = Step::Arrange;
    }
    const Step previous = m_step;
    if (previous != s && m_banner->isVisible() && m_banner->kind() == Banner::Kind::Info && !m_banner->hasActions())
        m_banner->hide(); // hints belong to the step they were shown in
    m_step = s;
    m_steps->setCurrentIndex(int(s));
    if (auto* b = m_stepButtons->button(int(s)))
        b->setChecked(true);
    if (s == Step::Edit) {
        // Coming from Arrange: open the page the user selected there.
        const QList<int> chosen = previous == Step::Arrange ? selectedRows(m_grid) : QList<int>{};
        if (!chosen.isEmpty())
            selectPageInEdit(chosen.first());
        else if (m_canvas->pageId().isNull() || m_doc->indexOf(m_canvas->pageId()) < 0)
            selectPageInEdit(0);
        QTimer::singleShot(0, this, &CombineWorkspace::fitPage);
    }
    if (s == Step::Export)
        refreshExportSummary();
    updateActions();
}

void CombineWorkspace::setTool(PageCanvas::Tool tool)
{
    if (auto* b = m_tools->button(int(tool))) {
        b->setChecked(true);
        Q_EMIT m_tools->idClicked(int(tool));
    }
}

void CombineWorkspace::runDiagnosticDemo()
{
    if (m_doc->isEmpty())
        return;
    selectPageInEdit(0);
    setTool(PageCanvas::Tool::Perspective);
    m_canvas->detectPage();
    QString err;
    m_canvas->applyPerspective(&err);
    CombinePage p = m_doc->page(0);
    Annotation hl;
    hl.type = Annotation::Type::Highlighter;
    hl.points = {QPointF(0.08, 0.30), QPointF(0.62, 0.30)};
    hl.stroke = QColor(255, 212, 0);
    hl.width = 0.03;
    hl.opacity = 0.4;
    Annotation arrow;
    arrow.type = Annotation::Type::Arrow;
    arrow.points = {QPointF(0.80, 0.62), QPointF(0.62, 0.48)};
    arrow.stroke = QColor(211, 47, 47);
    arrow.width = 0.006;
    Annotation box;
    box.type = Annotation::Type::Rect;
    box.rect = QRectF(0.06, 1.18, 0.88, 0.09);
    box.stroke = QColor(31, 79, 191);
    box.width = 0.004;
    Annotation text;
    text.type = Annotation::Type::Text;
    text.text = QString::fromUtf8("Sprawdzone – zażółć gęślą jaźń");
    text.rect = QRectF(0.55, 0.66, 0.42, 0.05);
    text.stroke = QColor(211, 47, 47);
    text.bold = true;
    p.annotations << hl << arrow << box << text;
    m_doc->updatePage(p, tr("Demo annotations"));
}

// ---- Arrange ----------------------------------------------------------------------

QWidget* CombineWorkspace::buildArrange()
{
    auto* w = new QWidget(this);
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(16, 10, 16, 12);
    l->setSpacing(8);
    auto* tools = new QHBoxLayout;
    auto* add = makePrimaryButton(tr("Add images…"), w);
    add->setIcon(Icons::get("plus", Icons::Tone::OnAccent));
    add->setToolTip(tr("Add images (Ctrl+O)"));
    auto* paste = new QPushButton(Icons::get("file"), tr("Paste"), w);
    paste->setToolTip(tr("Add images from the clipboard (Ctrl+V)"));
    m_btnSort = new QPushButton(tr("Sort by name"), w);
    m_btnSort->setToolTip(tr("Order pages by file name (numbers in names are compared as numbers)"));
    m_btnUp = new QPushButton(Icons::get("chevron-up"), tr("Move up"), w);
    m_btnUp->setToolTip(tr("Move up (Ctrl+Up)"));
    m_btnDown = new QPushButton(Icons::get("chevron-down"), tr("Move down"), w);
    m_btnDown->setToolTip(tr("Move down (Ctrl+Down)"));
    m_btnDuplicate = new QPushButton(tr("Duplicate"), w);
    m_btnRemove = new QPushButton(Icons::get("trash"), tr("Remove"), w);
    m_btnRemove->setToolTip(tr("Remove selected pages (Delete)"));
    m_btnEdit = new QPushButton(Icons::get("edit"), tr("Edit page"), w);
    tools->addWidget(add);
    tools->addWidget(paste);
    tools->addWidget(makeSeparator(w));
    tools->addWidget(m_btnUp);
    tools->addWidget(m_btnDown);
    tools->addWidget(m_btnSort);
    tools->addWidget(m_btnDuplicate);
    tools->addWidget(m_btnRemove);
    tools->addStretch();
    tools->addWidget(m_btnEdit);
    m_btnNextEdit = makePrimaryButton(tr("Next: Edit"), w);
    m_btnNextEdit->setIcon(Icons::get("chevron-right", Icons::Tone::OnAccent));
    tools->addWidget(m_btnNextEdit);
    l->addLayout(tools);

    m_arrangeStack = new QStackedWidget(w);
    m_drop = new DropZone(tr("Drop images to combine into one PDF"),
                          tr("PNG · JPG · WEBP · BMP · TIFF · HEIC\nYou can also paste screenshots and copied files with Ctrl+V."), w);
    m_drop->setFileFilter(tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff *.gif *.heic *.heif *.avif)"));
    connect(m_drop, &DropZone::filesDropped, this, qOverload<const QStringList&>(&CombineWorkspace::importFiles));
    m_grid = new PageGridView(PageGridView::Layout::Grid, w);
    m_grid->setModel(m_model);
    m_grid->setItemDelegate(new PageDelegate(false, m_grid));
    m_grid->setSpacing(4);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    connectPageView(m_grid);
    m_grid->installEventFilter(this);
    m_arrangeStack->addWidget(m_drop);
    m_arrangeStack->addWidget(m_grid);
    l->addWidget(m_arrangeStack, 1);
    m_arrangeInfo = muted(QString(), w);
    l->addWidget(m_arrangeInfo);

    connect(add, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add images"), QString(),
                                                                tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff *.gif *.heic *.heif *.avif)"));
        importFiles(files);
    });
    connect(paste, &QPushButton::clicked, this, &CombineWorkspace::pasteFromClipboard);
    connect(m_btnSort, &QPushButton::clicked, m_doc, &CombineDocument::sortByName);
    connect(m_btnUp, &QPushButton::clicked, this, [this] {
        QList<int> rows = selectedRows(m_grid);
        if (rows.isEmpty() || rows.first() == 0)
            return;
        reorderPages(rows, rows.first() - 1);
    });
    connect(m_btnDown, &QPushButton::clicked, this, [this] {
        QList<int> rows = selectedRows(m_grid);
        if (rows.isEmpty() || rows.last() >= m_doc->count() - 1)
            return;
        reorderPages(rows, rows.last() + 2);
    });
    connect(m_btnDuplicate, &QPushButton::clicked, this, [this] { m_doc->duplicatePages(selectedRows(m_grid)); });
    connect(m_btnRemove, &QPushButton::clicked, this, [this] { m_doc->removePages(selectedRows(m_grid)); });
    connect(m_btnEdit, &QPushButton::clicked, this, [this] {
        const QList<int> rows = selectedRows(m_grid);
        openEditor(rows.isEmpty() ? 0 : rows.first());
    });
    connect(m_btnNextEdit, &QPushButton::clicked, this, [this] { openEditor(selectedRows(m_grid).value(0, 0)); });
    connect(m_grid, &QListView::doubleClicked, this, [this](const QModelIndex& i) { openEditor(i.row()); });
    connect(m_grid, &QListView::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex idx = m_grid->indexAt(pos);
        QMenu menu(this);
        if (idx.isValid()) {
            menu.addAction(Icons::get("edit"), tr("Edit"), this, [this, idx] { openEditor(idx.row()); });
            menu.addAction(tr("Duplicate"), this, [this] { m_doc->duplicatePages(selectedRows(m_grid)); });
            menu.addAction(Icons::get("trash"), tr("Remove"), this, [this] { m_doc->removePages(selectedRows(m_grid)); });
            menu.addSeparator();
        }
        menu.addAction(tr("Paste images"), this, &CombineWorkspace::pasteFromClipboard);
        menu.exec(m_grid->viewport()->mapToGlobal(pos));
    });
    connect(m_grid->selectionModel(), &QItemSelectionModel::selectionChanged, this, &CombineWorkspace::updateActions);
    return w;
}

bool CombineWorkspace::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(event);
        QListView* view = qobject_cast<QListView*>(watched);
        if (view && (view == m_grid || view == m_pageList)) {
            if (k->key() == Qt::Key_Delete) {
                m_doc->removePages(selectedRows(view));
                return true;
            }
            if ((k->modifiers() & Qt::ControlModifier) && (k->key() == Qt::Key_Up || k->key() == Qt::Key_Left)) {
                QList<int> rows = selectedRows(view);
                if (!rows.isEmpty() && rows.first() > 0)
                    reorderPages(rows, rows.first() - 1);
                return true;
            }
            if ((k->modifiers() & Qt::ControlModifier) && (k->key() == Qt::Key_Down || k->key() == Qt::Key_Right)) {
                QList<int> rows = selectedRows(view);
                if (!rows.isEmpty() && rows.last() < m_doc->count() - 1)
                    reorderPages(rows, rows.last() + 2);
                return true;
            }
            if (view == m_grid && (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter)) {
                openEditor(view->currentIndex().row());
                return true;
            }
        }
    }
    if (m_scroll && m_canvas && watched == m_scroll->viewport() && event->type() == QEvent::Resize)
        m_canvas->resize(m_canvas->minimumSize().expandedTo(m_scroll->viewport()->size()));
    return QWidget::eventFilter(watched, event);
}

QList<int> CombineWorkspace::selectedRows(QListView* view) const
{
    QList<int> rows;
    for (const QModelIndex& i : view->selectionModel()->selectedIndexes())
        rows << i.row();
    std::sort(rows.begin(), rows.end());
    return rows;
}

void CombineWorkspace::openEditor(int row)
{
    if (m_doc->isEmpty())
        return;
    setStep(Step::Edit);
    selectPageInEdit(std::clamp(row, 0, m_doc->count() - 1));
}

// ---- import -------------------------------------------------------------------------

void CombineWorkspace::connectPageView(PageGridView* view)
{
    connect(view, &PageGridView::reorderRequested, this, &CombineWorkspace::reorderPages);
    connect(view, &PageGridView::filesDropped, this, [this](const QStringList& files, int at) { importFiles(files, at); });
    connect(view, &PageGridView::imageDropped, this, [this](const QImage& image, int at) {
        QString err;
        CombinePage page = CombineSession::pageFromImage(image, tr("Dropped image"), &err);
        ImportResult r;
        if (page.id.isNull())
            r.errors << err;
        else
            r.pages << page;
        onImported(r, at);
    });
}

void CombineWorkspace::reorderPages(const QList<int>& rows, int insertIndex)
{
    QList<QUuid> moved;
    for (int r : rows)
        if (r >= 0 && r < m_doc->count())
            moved << m_doc->page(r).id;
    m_doc->movePages(rows, insertIndex); // the only place the order changes; recorded for Undo/Redo
    // Keep the moved pages selected at their new positions in both views.
    for (QListView* view : {static_cast<QListView*>(m_grid), static_cast<QListView*>(m_pageList)}) {
        if (!view || !view->selectionModel())
            continue;
        QItemSelection sel;
        for (const QUuid& id : moved) {
            const int row = m_doc->indexOf(id);
            if (row >= 0)
                sel.select(m_model->index(row), m_model->index(row));
        }
        view->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
        if (!moved.isEmpty())
            view->selectionModel()->setCurrentIndex(m_model->index(m_doc->indexOf(moved.first())), QItemSelectionModel::NoUpdate);
    }
    updateActions();
}

void CombineWorkspace::importFiles(const QStringList& paths)
{
    importFiles(paths, -1);
}

void CombineWorkspace::importFiles(const QStringList& paths, int position)
{
    if (paths.isEmpty())
        return;
    ++m_importsRunning;
    Q_EMIT statusMessage(tr("Importing %n file(s)…", nullptr, int(paths.size())));
    const int insertAt = position < 0 ? m_doc->count() : position;
    auto* watcher = new QFutureWatcher<ImportResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, insertAt] {
        watcher->deleteLater();
        --m_importsRunning;
        onImported(watcher->result(), insertAt);
    });
    // Decoding and fingerprinting run off the GUI thread.
    watcher->setFuture(QtConcurrent::run([paths] { return CombineSession::importFiles(paths); }));
}

bool CombineWorkspace::importInProgress() const { return m_importsRunning > 0; }

bool CombineWorkspace::pasteFromClipboard()
{
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (!CombineSession::mimeHasImportable(mime)) {
        m_banner->show(Banner::Kind::Info, tr("The clipboard does not contain an image or image files."));
        return false;
    }
    QStringList files;
    for (const QUrl& u : mime->urls())
        if (u.isLocalFile())
            files << u.toLocalFile();
    if (!files.isEmpty()) {
        importFiles(files);
        return true;
    }
    const ImportResult r = CombineSession::importMime(mime);
    onImported(r, m_doc->count());
    return !r.pages.isEmpty();
}

void CombineWorkspace::onImported(const ImportResult& r, int insertAt)
{
    if (!r.pages.isEmpty()) {
        m_doc->addPages(r.pages, std::min(insertAt, m_doc->count()));
        m_arrangeStack->setCurrentWidget(m_grid);
    }
    if (!r.errors.isEmpty())
        m_banner->show(Banner::Kind::Warning, tr("Some files were not added:") + "\n" + r.errors.mid(0, 6).join("\n")
                                                  + (r.errors.size() > 6 ? QStringLiteral("\n…") : QString()));
    else if (!r.pages.isEmpty())
        m_banner->hide();
    Q_EMIT statusMessage(tr("%n page(s) added.", nullptr, int(r.pages.size())));
    updateActions();
    Q_EMIT importFinished(int(r.pages.size()));
}

void CombineWorkspace::dragEnterEvent(QDragEnterEvent* e)
{
    if (CombineSession::mimeHasImportable(e->mimeData()))
        e->acceptProposedAction();
}

void CombineWorkspace::dropEvent(QDropEvent* e)
{
    QStringList files;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile())
            files << u.toLocalFile();
    if (!files.isEmpty())
        importFiles(files);
    else if (e->mimeData()->hasImage())
        onImported(CombineSession::importMime(e->mimeData()), m_doc->count());
    e->acceptProposedAction();
}

void CombineWorkspace::requestThumbnail(const QUuid& id)
{
    const int row = m_doc->indexOf(id);
    if (row < 0)
        return;
    const CombinePage page = m_doc->page(row);
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, id] {
        watcher->deleteLater();
        if (m_doc->indexOf(id) >= 0)
            m_model->setThumbnail(id, watcher->result());
    });
    watcher->setFuture(QtConcurrent::run([page] { return makeThumbnail(page); }));
}

// ---- Edit -----------------------------------------------------------------------------

QWidget* CombineWorkspace::buildEdit()
{
    auto* w = new QWidget(this);
    auto* root = new QVBoxLayout(w);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* bar = new QWidget(w);
    bar->setObjectName("Toolbar");
    auto* tl = new QHBoxLayout(bar);
    tl->setContentsMargins(12, 4, 12, 4);
    tl->setSpacing(2);
    m_tools = new QButtonGroup(this);
    m_tools->setExclusive(true);
    struct T {
        PageCanvas::Tool tool;
        const char* icon;
        const char* text;
        const char* tip;
    };
    const T tools[] = {
        {PageCanvas::Tool::Select, "text-cursor", "Select", "Select, move and resize annotations"},
        {PageCanvas::Tool::Perspective, "perspective", "Perspective",
         "Perspective: move four independent corners onto a photographed page, then Straighten"},
        {PageCanvas::Tool::Crop, "crop", "Crop", "Crop: cut a straight rectangle out of the page (no straightening)"},
        {PageCanvas::Tool::Pen, "edit", "Pen", "Freehand drawing"},
        {PageCanvas::Tool::Highlighter, "text-color", "Highlighter", "Transparent highlighter"},
        {PageCanvas::Tool::Eraser, "trash", "Eraser", "Erase annotations (the photo is never changed)"},
        {PageCanvas::Tool::Shape, "compare", "Shapes", "Line, arrow, rectangle, ellipse, polyline"},
        {PageCanvas::Tool::Text, "text-add", "Text", "Add a text box"},
    };
    for (const T& t : tools) {
        auto* b = toolButton(QString::fromLatin1(t.icon), tr(t.text), tr(t.tip), bar);
        b->setObjectName(QStringLiteral("tool-") + QString::fromLatin1(t.text).toLower());
        m_tools->addButton(b, int(t.tool));
        tl->addWidget(b);
        if (t.tool == PageCanvas::Tool::Shape) {
            m_shapeButton = b;
            b->setPopupMode(QToolButton::MenuButtonPopup);
            auto* menu = new QMenu(b);
            const std::pair<Annotation::Type, QString> shapes[] = {
                {Annotation::Type::Arrow, tr("Arrow")}, {Annotation::Type::Line, tr("Line")},
                {Annotation::Type::Rect, tr("Rectangle")}, {Annotation::Type::Ellipse, tr("Ellipse")},
                {Annotation::Type::Polyline, tr("Polyline (click points, double-click to finish)")}};
            for (const auto& [type, label] : shapes)
                menu->addAction(label, this, [this, type = type, label = label] {
                    m_canvas->setShapeType(type);
                    m_shapeButton->setText(label.section(' ', 0, 0));
                    m_tools->button(int(PageCanvas::Tool::Shape))->setChecked(true);
                    m_canvas->setTool(PageCanvas::Tool::Shape);
                    syncOptionsPanel();
                });
            b->setMenu(menu);
            b->setText(tr("Arrow"));
        }
        if (t.tool == PageCanvas::Tool::Crop)
            tl->addWidget(makeSeparator(bar));
    }
    tl->addWidget(makeSeparator(bar));
    auto* rotL = makeToolButton("rotate-left", tr("Rotate page left"), bar);
    auto* rotR = makeToolButton("rotate-right", tr("Rotate page right"), bar);
    tl->addWidget(rotL);
    tl->addWidget(rotR);
    tl->addStretch();
    auto* zoomOut = makeToolButton("zoom-out", tr("Zoom out (Ctrl+-)"), bar);
    auto* zoomIn = makeToolButton("zoom-in", tr("Zoom in (Ctrl++)"), bar);
    auto* fit = makeToolButton("fit-width", tr("Fit page / reset view (Ctrl+0)"), bar);
    tl->addWidget(zoomOut);
    tl->addWidget(zoomIn);
    tl->addWidget(fit);
    tl->addWidget(makeSeparator(bar));
    auto* toExport = makePrimaryButton(tr("Next: Export"), bar);
    toExport->setIcon(Icons::get("chevron-right", Icons::Tone::OnAccent));
    tl->addWidget(toExport);
    root->addWidget(bar);

    auto* split = new QSplitter(Qt::Horizontal, w);
    split->setChildrenCollapsible(false);
    m_pageList = new PageGridView(PageGridView::Layout::VerticalList, split);
    m_pageList->setModel(m_model);
    m_pageList->setItemDelegate(new PageDelegate(true, m_pageList));
    connectPageView(m_pageList);
    m_pageList->setMinimumWidth(170);
    m_pageList->setMaximumWidth(190);
    m_pageList->setFrameShape(QFrame::NoFrame);
    m_pageList->installEventFilter(this);

    auto* center = new QWidget(split);
    auto* cl = new QVBoxLayout(center);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    m_scroll = new QScrollArea(center);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignCenter);
    m_canvas = new PageCanvas;
    m_canvas->setDocument(m_doc);
    m_scroll->setWidget(m_canvas);
    m_scroll->viewport()->installEventFilter(this);
    m_editInfo = muted(QString(), center);
    m_editInfo->setContentsMargins(10, 4, 10, 4);
    cl->addWidget(m_scroll, 1);
    cl->addWidget(m_editInfo);

    auto* options = buildOptionsPanel();
    options->setParent(split);
    split->addWidget(m_pageList);
    split->addWidget(center);
    split->addWidget(options);
    split->setStretchFactor(1, 1);
    split->setSizes({180, 900, 280});
    root->addWidget(split, 1);

    connect(m_tools, &QButtonGroup::idClicked, this, [this](int id) {
        m_canvas->setTool(PageCanvas::Tool(id));
        switch (PageCanvas::Tool(id)) {
        case PageCanvas::Tool::Pen: m_canvas->setStyle(m_penStyle); break;
        case PageCanvas::Tool::Highlighter: m_canvas->setStyle(m_highlighterStyle); break;
        case PageCanvas::Tool::Shape: m_canvas->setStyle(m_shapeStyle); break;
        case PageCanvas::Tool::Text: m_canvas->setStyle(m_textStyle); break;
        default: break;
        }
        syncOptionsPanel();
        fitPage();
        m_canvas->setFocus();
    });
    m_tools->button(int(PageCanvas::Tool::Select))->setChecked(true);
    connect(rotL, &QToolButton::clicked, this, [this] { m_canvas->rotatePage(-90); fitPage(); });
    connect(rotR, &QToolButton::clicked, this, [this] { m_canvas->rotatePage(90); fitPage(); });
    connect(zoomIn, &QToolButton::clicked, this, [this] { zoomBy(1.15); });
    connect(zoomOut, &QToolButton::clicked, this, [this] { zoomBy(1 / 1.15); });
    connect(fit, &QToolButton::clicked, this, &CombineWorkspace::fitPage);
    connect(toExport, &QPushButton::clicked, this, [this] { setStep(Step::Export); });
    connect(m_pageList->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& i) {
        if (m_step == Step::Edit && i.isValid() && m_doc->page(i.row()).id != m_canvas->pageId()) {
            m_canvas->setPage(m_doc->page(i.row()).id);
            fitPage();
            syncOptionsPanel();
        }
    });
    connect(m_canvas, &PageCanvas::selectionChanged, this, &CombineWorkspace::syncOptionsPanel);
    connect(m_canvas, &PageCanvas::perspectiveChanged, this, &CombineWorkspace::syncOptionsPanel);
    connect(m_canvas, &PageCanvas::zoomChanged, this, [this](double z) {
        m_editInfo->setText(tr("Page %1 of %2 · Zoom %3%")
                                .arg(m_doc->indexOf(m_canvas->pageId()) + 1)
                                .arg(m_doc->count())
                                .arg(int(std::round(z * 100))));
    });
    connect(m_canvas, &PageCanvas::message, this, [this](const QString& text, bool error) {
        m_banner->show(error ? Banner::Kind::Warning : Banner::Kind::Info, text);
    });
    return w;
}

QWidget* CombineWorkspace::buildOptionsPanel()
{
    auto* panel = new QWidget;
    panel->setMinimumWidth(250);
    panel->setMaximumWidth(330);
    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(12, 12, 12, 12);
    m_options = new QStackedWidget(panel);
    outer->addWidget(m_options);

    // 0: perspective --------------------------------------------------------
    auto* persp = new QWidget;
    auto* pl = new QVBoxLayout(persp);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(8);
    pl->addWidget(sectionTitle(tr("Perspective"), persp));
    auto* howTo = new QLabel(tr("Drag each corner to match the corners of your document, then click Straighten."), persp);
    howTo->setWordWrap(true);
    pl->addWidget(howTo);
    pl->addWidget(muted(tr("Every corner moves on its own - any convex four-sided shape works. Arrow keys move the "
                           "selected corner (Shift = 10x), Tab selects the next one, Detect page finds them for you."), persp));
    auto* detect = new QPushButton(Icons::get("search"), tr("Detect page"), persp);
    auto* resetQ = new QPushButton(tr("Reset corners"), persp);
    auto* order = new QPushButton(tr("Fix corner order"), persp);
    auto* row1 = new QHBoxLayout;
    row1->addWidget(detect);
    row1->addWidget(resetQ);
    pl->addLayout(row1);
    pl->addWidget(order);
    auto* corners = new QHBoxLayout;
    m_cornerButtons = new QButtonGroup(this);
    const QStringList cn = {tr("TL"), tr("TR"), tr("BR"), tr("BL")};
    for (int i = 0; i < 4; ++i) {
        auto* b = new QToolButton(persp);
        b->setText(cn[i]);
        b->setObjectName(QStringLiteral("corner%1").arg(i));
        b->setCheckable(true);
        b->setAutoRaise(false);
        b->setToolTip(QStringList{tr("Top left"), tr("Top right"), tr("Bottom right"), tr("Bottom left")}[i]);
        m_cornerButtons->addButton(b, i);
        corners->addWidget(b);
    }
    pl->addLayout(corners);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    m_cornerX = new QDoubleSpinBox(persp);
    m_cornerY = new QDoubleSpinBox(persp);
    m_cornerX->setObjectName(QStringLiteral("cornerX"));
    m_cornerY->setObjectName(QStringLiteral("cornerY"));
    for (auto* s : {m_cornerX, m_cornerY}) {
        s->setDecimals(0);
        s->setRange(-100000, 100000);
        s->setSuffix(" px");
        s->setKeyboardTracking(false);
    }
    form->addRow(tr("X"), m_cornerX);
    form->addRow(tr("Y"), m_cornerY);
    m_aspect = new QComboBox(persp);
    m_aspect->addItems({tr("Auto (from corners)"), tr("A4"), tr("Letter"), tr("Custom")});
    m_customRatio = new QDoubleSpinBox(persp);
    m_customRatio->setRange(0.1, 10);
    m_customRatio->setDecimals(3);
    m_customRatio->setValue(1.414);
    m_customRatio->setToolTip(tr("Height divided by width"));
    m_marginPct = new QDoubleSpinBox(persp);
    m_marginPct->setRange(0, 25);
    m_marginPct->setDecimals(1);
    m_marginPct->setSuffix(" %");
    m_marginPct->setToolTip(tr("Keeps a band of the photo around the page so text near the edge is never cut."));
    form->addRow(tr("Proportions"), m_aspect);
    form->addRow(tr("Height / width"), m_customRatio);
    form->addRow(tr("Safety margin"), m_marginPct);
    pl->addLayout(form);
    auto* cmp = new QHBoxLayout;
    m_compare = new QButtonGroup(this);
    const QStringList modes = {tr("Original"), tr("Corrected"), tr("Split")};
    for (int i = 0; i < 3; ++i) {
        auto* b = new QToolButton(persp);
        b->setText(modes[i]);
        b->setCheckable(true);
        m_compare->addButton(b, i);
        cmp->addWidget(b);
    }
    pl->addWidget(muted(tr("Preview"), persp));
    pl->addLayout(cmp);
    m_quadStatus = muted(QString(), persp);
    pl->addWidget(m_quadStatus);
    m_straighten = makePrimaryButton(tr("Straighten"), persp);
    m_straighten->setObjectName(QStringLiteral("straighten"));
    m_removeCorrection = new QPushButton(tr("Remove correction"), persp);
    auto* applyAll = new QPushButton(tr("Apply proportions && margin to…"), persp);
    applyAll->setToolTip(tr("Copies the proportions and safety margin settings to other pages. "
                            "Corners are never copied: every photo needs its own."));
    auto* applyMenu = new QMenu(applyAll);
    auto* applySelected = applyMenu->addAction(tr("Selected pages (in the page list)"));
    auto* applyEvery = applyMenu->addAction(tr("All pages"));
    applyAll->setMenu(applyMenu);
    pl->addWidget(m_straighten);
    pl->addWidget(m_removeCorrection);
    pl->addWidget(applyAll);
    pl->addStretch();
    m_options->addWidget(persp);

    connect(detect, &QPushButton::clicked, this, [this] { m_canvas->detectPage(); });
    connect(resetQ, &QPushButton::clicked, m_canvas, &PageCanvas::resetQuad);
    connect(order, &QPushButton::clicked, m_canvas, &PageCanvas::autoOrderCorners);
    connect(m_cornerButtons, &QButtonGroup::idClicked, this, [this](int c) {
        m_canvas->selectCorner(c);
        m_canvas->setFocus();
    });
    auto cornerEdited = [this] {
        if (m_syncing)
            return;
        m_canvas->setCornerPosition(m_canvas->selectedCorner(), QPointF(m_cornerX->value(), m_cornerY->value()));
    };
    connect(m_cornerX, &QDoubleSpinBox::valueChanged, this, cornerEdited);
    connect(m_cornerY, &QDoubleSpinBox::valueChanged, this, cornerEdited);
    auto perspEdited = [this] {
        if (m_syncing)
            return;
        PerspectiveSettings& s = m_canvas->pendingPerspective();
        s.aspect = AspectMode(m_aspect->currentIndex());
        s.customRatio = m_customRatio->value();
        s.marginPercent = m_marginPct->value();
        m_customRatio->setEnabled(s.aspect == AspectMode::Custom);
        m_canvas->refreshPerspectivePreview();
        syncOptionsPanel();
    };
    connect(m_aspect, &QComboBox::currentIndexChanged, this, perspEdited);
    connect(m_customRatio, &QDoubleSpinBox::valueChanged, this, perspEdited);
    connect(m_marginPct, &QDoubleSpinBox::valueChanged, this, perspEdited);
    connect(m_compare, &QButtonGroup::idClicked, this, [this](int id) {
        m_canvas->setCompareMode(PageCanvas::CompareMode(id));
        fitPage();
    });
    connect(m_straighten, &QPushButton::clicked, this, [this] {
        QString err;
        if (!m_canvas->applyPerspective(&err)) {
            m_banner->show(Banner::Kind::Warning, err);
            return;
        }
        m_banner->show(Banner::Kind::Success, tr("Page straightened. You can still move the corners and straighten again; "
                                                 "the original photo is kept."));
        fitPage();
        syncOptionsPanel();
    });
    connect(m_removeCorrection, &QPushButton::clicked, this, [this] {
        m_canvas->removePerspective();
        fitPage();
        syncOptionsPanel();
    });
    auto applyPerspectiveSettings = [this](bool selectedOnly) {
        const PerspectiveSettings ps = m_canvas->pendingPerspective();
        QList<int> rows;
        if (selectedOnly)
            rows = selectedRows(m_pageList);
        else
            for (int i = 0; i < m_doc->count(); ++i)
                rows << i;
        if (rows.isEmpty())
            return;
        m_doc->beginMacro(tr("Apply proportions to pages"));
        for (int r : rows) {
            CombinePage c = m_doc->page(r);
            c.perspective = ps;
            m_doc->updatePage(c, tr("Apply proportions"));
        }
        m_doc->endMacro();
        m_banner->show(Banner::Kind::Info, tr("Proportions and margin set on %n page(s). Corners are not copied.", nullptr, int(rows.size())));
    };
    connect(applySelected, &QAction::triggered, this, [applyPerspectiveSettings] { applyPerspectiveSettings(true); });
    connect(applyEvery, &QAction::triggered, this, [applyPerspectiveSettings] { applyPerspectiveSettings(false); });

    // 1: crop ---------------------------------------------------------------
    auto* crop = new QWidget;
    auto* cropL = new QVBoxLayout(crop);
    cropL->setContentsMargins(0, 0, 0, 0);
    cropL->setSpacing(8);
    cropL->addWidget(sectionTitle(tr("Crop"), crop));
    cropL->addWidget(muted(tr("Drag the frame or its handles; drag outside to draw a new frame. Crop works on the "
                              "straightened page — straighten first if the photo is taken at an angle."), crop));
    auto* applyCrop = makePrimaryButton(tr("Apply crop"), crop);
    auto* resetCrop = new QPushButton(tr("Reset crop"), crop);
    cropL->addWidget(applyCrop);
    cropL->addWidget(resetCrop);
    cropL->addStretch();
    m_options->addWidget(crop);
    connect(applyCrop, &QPushButton::clicked, this, [this] {
        m_canvas->applyCrop();
        fitPage();
    });
    connect(resetCrop, &QPushButton::clicked, this, [this] {
        m_canvas->resetCrop();
        fitPage();
    });

    // 2: style (pen / highlighter / shapes / text / selection) -------------
    m_stylePanel = new QWidget;
    auto* sl = new QVBoxLayout(m_stylePanel);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(8);
    m_styleTitle = sectionTitle(tr("Pen"), m_stylePanel);
    sl->addWidget(m_styleTitle);
    m_styleHint = muted(QString(), m_stylePanel);
    sl->addWidget(m_styleHint);
    auto* sf = new QFormLayout;
    sf->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_strokeColor = new QToolButton(m_stylePanel);
    m_strokeColor->setToolTip(tr("Colour"));
    m_width = new QSlider(Qt::Horizontal, m_stylePanel);
    m_width->setRange(1, 200); // tenths of a point at A4 width... mapped below
    m_widthLabel = new QLabel(m_stylePanel);
    m_opacity = new QSlider(Qt::Horizontal, m_stylePanel);
    m_opacity->setRange(5, 100);
    m_opacityLabel = new QLabel(m_stylePanel);
    auto* widthRow = new QHBoxLayout;
    widthRow->addWidget(m_width, 1);
    widthRow->addWidget(m_widthLabel);
    auto* opacityRow = new QHBoxLayout;
    opacityRow->addWidget(m_opacity, 1);
    opacityRow->addWidget(m_opacityLabel);
    m_smooth = new QCheckBox(tr("Smooth lines"), m_stylePanel);
    m_fillEnabled = new QCheckBox(tr("Fill"), m_stylePanel);
    m_fillColor = new QToolButton(m_stylePanel);
    auto* fillRow = new QHBoxLayout;
    fillRow->addWidget(m_fillEnabled);
    fillRow->addWidget(m_fillColor);
    fillRow->addStretch();
    m_styleForm = sf;
    m_widthRow = widthRow;
    m_opacityRow = opacityRow;
    m_fillRow = fillRow;
    sf->addRow(tr("Colour"), m_strokeColor);
    sf->addRow(tr("Thickness"), widthRow);
    sf->addRow(tr("Opacity"), opacityRow);
    sf->addRow(QString(), m_smooth);
    sf->addRow(QString(), fillRow);
    sl->addLayout(sf);
    m_textOptions = new QWidget(m_stylePanel);
    auto* tf = new QFormLayout(m_textOptions);
    tf->setContentsMargins(0, 0, 0, 0);
    m_fontFamily = new QComboBox(m_textOptions);
    m_fontFamily->addItems(FontMatcher::availableFamilies());
    m_fontSize = new QDoubleSpinBox(m_textOptions);
    m_fontSize->setRange(4, 120);
    m_fontSize->setDecimals(1);
    m_fontSize->setSuffix(" pt");
    m_fontSize->setToolTip(tr("Size relative to an A4-wide page"));
    auto* fmt = new QHBoxLayout;
    m_bold = makeToolButton("bold", tr("Bold"), m_textOptions, true);
    m_italic = makeToolButton("italic", tr("Italic"), m_textOptions, true);
    m_underline = makeToolButton("underline", tr("Underline"), m_textOptions, true);
    fmt->addWidget(m_bold);
    fmt->addWidget(m_italic);
    fmt->addWidget(m_underline);
    fmt->addSpacing(8);
    m_align = new QButtonGroup(this);
    const QStringList alignIcons = {"align-left", "align-center", "align-right"};
    for (int i = 0; i < 3; ++i) {
        auto* b = makeToolButton(alignIcons[i], QStringList{tr("Left"), tr("Center"), tr("Right")}[i], m_textOptions, true);
        m_align->addButton(b, i);
        fmt->addWidget(b);
    }
    fmt->addStretch();
    tf->addRow(tr("Font"), m_fontFamily);
    tf->addRow(tr("Size"), m_fontSize);
    tf->addRow(fmt);
    sl->addWidget(m_textOptions);
    m_selectionActions = new QWidget(m_stylePanel);
    auto* sa = new QGridLayout(m_selectionActions);
    sa->setContentsMargins(0, 6, 0, 0);
    auto* del = new QPushButton(Icons::get("trash"), tr("Delete"), m_selectionActions);
    auto* dup = new QPushButton(tr("Duplicate"), m_selectionActions);
    auto* fwd = new QPushButton(Icons::get("chevron-up"), tr("Forward"), m_selectionActions);
    auto* bwd = new QPushButton(Icons::get("chevron-down"), tr("Backward"), m_selectionActions);
    auto* editText = new QPushButton(Icons::get("text-cursor"), tr("Edit text"), m_selectionActions);
    sa->addWidget(del, 0, 0);
    sa->addWidget(dup, 0, 1);
    sa->addWidget(fwd, 1, 0);
    sa->addWidget(bwd, 1, 1);
    sa->addWidget(editText, 2, 0, 1, 2);
    sl->addWidget(m_selectionActions);
    sl->addStretch();
    m_options->addWidget(m_stylePanel);

    connect(del, &QPushButton::clicked, m_canvas, &PageCanvas::deleteSelection);
    connect(dup, &QPushButton::clicked, m_canvas, &PageCanvas::duplicateSelection);
    connect(fwd, &QPushButton::clicked, this, [this] { m_canvas->raiseSelection(1); });
    connect(bwd, &QPushButton::clicked, this, [this] { m_canvas->raiseSelection(-1); });
    connect(editText, &QPushButton::clicked, this, [this] {
        const auto s = m_canvas->selected();
        if (s && s->type == Annotation::Type::Text) {
            QMouseEvent fake(QEvent::MouseButtonDblClick, QPointF(), QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(m_canvas, &fake);
        }
    });
    auto pickColor = [this](QToolButton* button, bool fill) {
        connect(button, &QToolButton::clicked, this, [this, button, fill] {
            const QColor current = fill ? m_canvas->style().fill : m_canvas->style().stroke;
            const QColor c = QColorDialog::getColor(current, this, tr("Choose colour"));
            if (!c.isValid())
                return;
            if (fill)
                m_canvas->style().fill = c;
            else
                m_canvas->style().stroke = c;
            button->setIcon(colorIcon(c));
            applyStyleFromPanel(tr("Change colour"));
        });
    };
    pickColor(m_strokeColor, false);
    pickColor(m_fillColor, true);
    connect(m_width, &QSlider::valueChanged, this, [this](int v) {
        m_widthLabel->setText(QStringLiteral("%1 pt").arg(v / 2.0, 0, 'f', 1));
        if (m_syncing)
            return;
        m_canvas->style().width = (v / 2.0) / kA4WidthPt;
        if (!m_width->isSliderDown())
            applyStyleFromPanel(tr("Change thickness"));
    });
    connect(m_width, &QSlider::sliderReleased, this, [this] { applyStyleFromPanel(tr("Change thickness")); });
    connect(m_opacity, &QSlider::valueChanged, this, [this](int v) {
        m_opacityLabel->setText(QStringLiteral("%1%").arg(v));
        if (m_syncing)
            return;
        m_canvas->style().opacity = v / 100.0;
        if (!m_opacity->isSliderDown())
            applyStyleFromPanel(tr("Change opacity"));
    });
    connect(m_opacity, &QSlider::sliderReleased, this, [this] { applyStyleFromPanel(tr("Change opacity")); });
    connect(m_smooth, &QCheckBox::toggled, this, [this](bool on) {
        if (m_syncing)
            return;
        m_canvas->style().smooth = on;
        applyStyleFromPanel(tr("Change smoothing"));
    });
    connect(m_fillEnabled, &QCheckBox::toggled, this, [this](bool on) {
        if (m_syncing)
            return;
        m_canvas->style().hasFill = on;
        applyStyleFromPanel(tr("Change fill"));
    });
    connect(m_fontFamily, &QComboBox::currentTextChanged, this, [this](const QString& f) {
        if (m_syncing)
            return;
        m_canvas->style().fontFamily = f;
        applyStyleFromPanel(tr("Change font"));
    });
    connect(m_fontSize, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (m_syncing)
            return;
        m_canvas->style().fontSize = v / kA4WidthPt;
        applyStyleFromPanel(tr("Change font size"));
    });
    for (auto* b : {m_bold, m_italic, m_underline})
        connect(b, &QToolButton::toggled, this, [this] {
            if (m_syncing)
                return;
            m_canvas->style().bold = m_bold->isChecked();
            m_canvas->style().italic = m_italic->isChecked();
            m_canvas->style().underline = m_underline->isChecked();
            applyStyleFromPanel(tr("Change text style"));
        });
    connect(m_align, &QButtonGroup::idClicked, this, [this](int id) {
        if (m_syncing)
            return;
        m_canvas->style().align = id;
        applyStyleFromPanel(tr("Change alignment"));
    });
    return panel;
}

void CombineWorkspace::applyStyleFromPanel(const QString& description)
{
    const ToolStyle s = m_canvas->style();
    // Remember the style per tool.
    switch (m_canvas->tool()) {
    case PageCanvas::Tool::Pen: m_penStyle = s; break;
    case PageCanvas::Tool::Highlighter: m_highlighterStyle = s; break;
    case PageCanvas::Tool::Shape: m_shapeStyle = s; break;
    case PageCanvas::Tool::Text: m_textStyle = s; break;
    default: break;
    }
    // In Select mode the panel edits the selected annotation.
    const auto sel = m_canvas->selected();
    if (m_canvas->tool() == PageCanvas::Tool::Select && sel) {
        Annotation a = *sel;
        a.stroke = s.stroke;
        a.opacity = s.opacity;
        if (a.type != Annotation::Type::Text)
            a.width = s.width;
        a.smooth = s.smooth;
        a.fill = s.fill;
        a.hasFill = s.hasFill && (a.type == Annotation::Type::Rect || a.type == Annotation::Type::Ellipse);
        if (a.type == Annotation::Type::Text) {
            a.fontFamily = s.fontFamily;
            a.fontSize = s.fontSize;
            a.bold = s.bold;
            a.italic = s.italic;
            a.underline = s.underline;
            a.align = s.align;
            const int lines = int(AnnotationRender::layoutText(a).size());
            a.rect.setHeight(lines * a.fontSize * 1.25 + a.fontSize * 0.35);
        }
        if (a.toJson() != sel->toJson())
            m_canvas->updateSelected(a, description);
    }
}

void CombineWorkspace::syncOptionsPanel()
{
    if (!m_canvas)
        return;
    m_syncing = true;
    const PageCanvas::Tool tool = m_canvas->tool();
    if (tool == PageCanvas::Tool::Perspective) {
        m_options->setCurrentIndex(0);
        const PerspectiveSettings& s = m_canvas->pendingPerspective();
        m_aspect->setCurrentIndex(int(s.aspect));
        m_customRatio->setValue(s.customRatio);
        m_customRatio->setEnabled(s.aspect == AspectMode::Custom);
        m_marginPct->setValue(s.marginPercent);
        if (auto* b = m_cornerButtons->button(m_canvas->selectedCorner()))
            b->setChecked(true);
        if (const auto q = m_canvas->pendingQuad()) {
            m_cornerX->setValue(q->pts[size_t(m_canvas->selectedCorner())].x());
            m_cornerY->setValue(q->pts[size_t(m_canvas->selectedCorner())].y());
            const QuadCheck c = m_canvas->pendingQuadCheck();
            if (c.ok) {
                const QSize out = Geometry::rectifiedSize(*q, s, QSizeF(m_canvas->rotatedSourceSize()));
                m_quadStatus->setText(tr("Result: %1 × %2 px").arg(out.width()).arg(out.height()));
            } else {
                m_quadStatus->setText("⚠ " + c.error);
            }
            m_straighten->setEnabled(c.ok);
        }
        if (auto* b = m_compare->button(int(m_canvas->compareMode())))
            b->setChecked(true);
        const int row = m_doc->indexOf(m_canvas->pageId());
        m_removeCorrection->setEnabled(row >= 0 && m_doc->page(row).quad.has_value());
    } else if (tool == PageCanvas::Tool::Crop) {
        m_options->setCurrentIndex(1);
    } else {
        m_options->setCurrentIndex(2);
        const auto sel = m_canvas->selected();
        ToolStyle s = m_canvas->style();
        QString title;
        bool showStroke = true, showFill = false, showText = false, showSmooth = false;
        switch (tool) {
        case PageCanvas::Tool::Pen: title = tr("Pen"); showSmooth = true; break;
        case PageCanvas::Tool::Highlighter: title = tr("Highlighter"); showSmooth = true; break;
        case PageCanvas::Tool::Shape: title = tr("Shapes"); showFill = true; break;
        case PageCanvas::Tool::Text: title = tr("Text"); showText = true; break;
        case PageCanvas::Tool::Eraser:
            title = tr("Eraser");
            showStroke = false;
            break;
        case PageCanvas::Tool::Select:
            title = sel ? tr("Selected: %1").arg(Annotation::typeName(sel->type)) : tr("Select");
            showStroke = sel.has_value();
            if (sel) {
                s.stroke = sel->stroke;
                s.width = sel->width;
                s.opacity = sel->opacity;
                s.smooth = sel->smooth;
                s.fill = sel->fill;
                s.hasFill = sel->hasFill;
                s.fontFamily = sel->fontFamily;
                s.fontSize = sel->fontSize;
                s.bold = sel->bold;
                s.italic = sel->italic;
                s.underline = sel->underline;
                s.align = sel->align;
                m_canvas->setStyle(s);
                showFill = sel->type == Annotation::Type::Rect || sel->type == Annotation::Type::Ellipse;
                showText = sel->type == Annotation::Type::Text;
                showSmooth = sel->type == Annotation::Type::Pen || sel->type == Annotation::Type::Highlighter;
            }
            break;
        default: break;
        }
        m_styleTitle->setText(title);
        m_styleHint->setText(tool == PageCanvas::Tool::Eraser
                                 ? tr("Drag over annotations to erase them. Strokes are cut where the eraser passes; "
                                      "shapes and text are removed whole. The photo itself is never changed.")
                             : tool == PageCanvas::Tool::Select && !sel
                                 ? tr("Click an annotation to select it. Drag to move, use the handles to resize, "
                                      "Delete to remove, Ctrl+D to duplicate, double-click text to edit it.")
                             : tool == PageCanvas::Tool::Text ? tr("Click on the page to add text. Ctrl+Enter finishes, Esc cancels.")
                                                              : QString());
        m_styleHint->setVisible(!m_styleHint->text().isEmpty());
        const bool isText = showText;
        // Whole form rows (label + field) are hidden, never just the field.
        m_styleForm->setRowVisible(m_strokeColor, showStroke);
        m_styleForm->setRowVisible(m_opacityRow, showStroke);
        m_styleForm->setRowVisible(m_widthRow, showStroke && !isText);
        m_styleForm->setRowVisible(m_smooth, showSmooth);
        m_styleForm->setRowVisible(m_fillRow, showFill);
        m_textOptions->setVisible(isText);
        m_selectionActions->setVisible(tool == PageCanvas::Tool::Select && sel.has_value());
        m_strokeColor->setIcon(colorIcon(s.stroke));
        m_fillColor->setIcon(colorIcon(s.fill));
        m_width->setValue(int(std::lround(s.width * kA4WidthPt * 2)));
        m_widthLabel->setText(QStringLiteral("%1 pt").arg(s.width * kA4WidthPt, 0, 'f', 1));
        m_opacity->setValue(int(std::lround(s.opacity * 100)));
        m_opacityLabel->setText(QStringLiteral("%1%").arg(int(std::lround(s.opacity * 100))));
        m_smooth->setChecked(s.smooth);
        m_fillEnabled->setChecked(s.hasFill);
        m_fontFamily->setCurrentText(s.fontFamily);
        m_fontSize->setValue(s.fontSize * kA4WidthPt);
        m_bold->setChecked(s.bold);
        m_italic->setChecked(s.italic);
        m_underline->setChecked(s.underline);
        if (auto* b = m_align->button(s.align))
            b->setChecked(true);
    }
    m_syncing = false;
}

void CombineWorkspace::selectPageInEdit(int row)
{
    if (m_doc->isEmpty())
        return;
    row = std::clamp(row, 0, m_doc->count() - 1);
    m_pageList->selectionModel()->setCurrentIndex(m_model->index(row), QItemSelectionModel::ClearAndSelect);
    if (m_canvas->pageId() != m_doc->page(row).id) {
        m_canvas->setPage(m_doc->page(row).id);
        fitPage();
    }
    syncOptionsPanel();
}

void CombineWorkspace::zoomBy(double f)
{
    if (m_step == Step::Edit)
        m_canvas->setZoom(m_canvas->zoom() * f);
}

void CombineWorkspace::fitPage()
{
    if (m_step == Step::Edit && m_scroll)
        m_canvas->fitToView(m_scroll->viewport()->size());
}

// ---- Export ---------------------------------------------------------------------------

QWidget* CombineWorkspace::buildExport()
{
    auto* w = new QWidget(this);
    auto* root = new QHBoxLayout(w);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(16);

    auto* left = new QVBoxLayout;
    left->addWidget(sectionTitle(tr("Summary"), w));
    m_summary = new QTreeWidget(w);
    m_summary->setHeaderLabels({tr("#"), tr("Page"), tr("Orientation"), tr("Page size"), tr("Image"), tr("Edits")});
    m_summary->setRootIsDecorated(false);
    m_summary->setAlternatingRowColors(true);
    m_summary->header()->setStretchLastSection(true);
    left->addWidget(m_summary, 1);
    m_estimate = muted(QString(), w);
    left->addWidget(m_estimate);
    root->addLayout(left, 3);

    auto* card = new QFrame(w);
    card->setProperty("card", true);
    card->setMinimumWidth(320);
    card->setMaximumWidth(400);
    auto* cl = new QVBoxLayout(card);
    cl->setContentsMargins(16, 14, 16, 14);
    cl->setSpacing(10);
    cl->addWidget(sectionTitle(tr("Document settings"), card));
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_pageSize = new QComboBox(card);
    m_pageSize->addItems({tr("Original image size"), tr("A4"), tr("Letter"), tr("Custom")});
    m_customW = new QDoubleSpinBox(card);
    m_customH = new QDoubleSpinBox(card);
    for (auto* s : {m_customW, m_customH}) {
        s->setRange(20, 2000);
        s->setSuffix(" mm");
        s->setDecimals(0);
    }
    auto* customRow = new QHBoxLayout;
    customRow->addWidget(m_customW);
    customRow->addWidget(new QLabel(QStringLiteral("×"), card));
    customRow->addWidget(m_customH);
    m_orientation = new QComboBox(card);
    m_orientation->addItems({tr("Auto (per page)"), tr("Portrait"), tr("Landscape")});
    m_margins = new QComboBox(card);
    m_margins->addItems({tr("None"), tr("Small (6 mm)"), tr("Normal (13 mm)"), tr("Custom")});
    m_customMargin = new QDoubleSpinBox(card);
    m_customMargin->setRange(0, 100);
    m_customMargin->setSuffix(" mm");
    m_quality = new QComboBox(card);
    m_quality->addItems({tr("High (lossless)"), tr("Balanced"), tr("Small file")});
    m_quality->setToolTip(tr("Balanced keeps unedited JPEG photos untouched and stores edited photos as high-quality JPEG; "
                             "screenshots stay lossless."));
    form->addRow(tr("Page size"), m_pageSize);
    form->addRow(tr("Custom size"), customRow);
    form->addRow(tr("Orientation"), m_orientation);
    form->addRow(tr("Margins"), m_margins);
    form->addRow(tr("Custom margin"), m_customMargin);
    form->addRow(tr("Image quality"), m_quality);
    cl->addLayout(form);
    m_exportButton = makePrimaryButton(tr("Export to PDF…"), card);
    m_exportButton->setIcon(Icons::get("export", Icons::Tone::OnAccent));
    m_cancelExport = new QPushButton(Icons::get("cancel"), tr("Cancel"), card);
    m_cancelExport->hide();
    m_progress = new QProgressBar(card);
    m_progress->setTextVisible(false);
    m_progress->hide();
    m_progressLabel = muted(QString(), card);
    m_exportBanner = new Banner(card);
    cl->addWidget(m_exportButton);
    cl->addWidget(m_progress);
    cl->addWidget(m_progressLabel);
    cl->addWidget(m_cancelExport);
    cl->addWidget(m_exportBanner);
    cl->addStretch();
    root->addWidget(card, 2);

    auto changed = [this] {
        readExportSettingsFromUi();
        refreshExportSummary();
    };
    for (QComboBox* c : {m_pageSize, m_orientation, m_margins, m_quality})
        connect(c, &QComboBox::currentIndexChanged, this, changed);
    for (QDoubleSpinBox* s : {m_customW, m_customH, m_customMargin})
        connect(s, &QDoubleSpinBox::valueChanged, this, changed);
    connect(m_exportButton, &QPushButton::clicked, this, [this] {
        const ExportSettings s = m_doc->exportSettings();
        QString folder = s.lastFolder;
        if (folder.isEmpty() || !QFileInfo(folder).isDir())
            folder = Database::instance().setting("combine.folder").toString();
        if (folder.isEmpty() || !QFileInfo(folder).isDir())
            folder = QDir::homePath() + "/Documents";
        // The dialog asks before replacing an existing file.
        QString path = QFileDialog::getSaveFileName(this, tr("Export to PDF"), QDir(folder).filePath(s.fileName), tr("PDF documents (*.pdf)"));
        if (path.isEmpty())
            return;
        if (!path.endsWith(".pdf", Qt::CaseInsensitive))
            path += ".pdf";
        startExport(path, true);
    });
    connect(m_cancelExport, &QPushButton::clicked, this, [this] {
        if (m_exportCancel)
            m_exportCancel->store(true);
        m_progressLabel->setText(tr("Cancelling…"));
    });
    connect(m_doc, &CombineDocument::exportSettingsChanged, this, [this] {
        if (m_step == Step::Export)
            refreshExportSummary();
    });
    return w;
}

void CombineWorkspace::readExportSettingsFromUi()
{
    if (m_syncing)
        return;
    ExportSettings s = m_doc->exportSettings();
    const PageSizeMode sizes[] = {PageSizeMode::Auto, PageSizeMode::A4, PageSizeMode::Letter, PageSizeMode::Custom};
    s.pageSize = sizes[std::clamp(m_pageSize->currentIndex(), 0, 3)];
    s.customSizeMm = QSizeF(m_customW->value(), m_customH->value());
    s.orientation = OrientationMode(m_orientation->currentIndex());
    s.margins = MarginMode(m_margins->currentIndex());
    s.customMarginMm = m_customMargin->value();
    s.quality = QualityMode(m_quality->currentIndex());
    m_doc->setExportSettings(s);
}

void CombineWorkspace::refreshExportSummary()
{
    const ExportSettings s = m_doc->exportSettings();
    m_syncing = true;
    const int sizeIndex = s.pageSize == PageSizeMode::Auto ? 0 : s.pageSize == PageSizeMode::A4 ? 1 : s.pageSize == PageSizeMode::Letter ? 2 : 3;
    m_pageSize->setCurrentIndex(sizeIndex);
    m_customW->setValue(s.customSizeMm.width());
    m_customH->setValue(s.customSizeMm.height());
    m_orientation->setCurrentIndex(int(s.orientation));
    m_margins->setCurrentIndex(int(s.margins));
    m_customMargin->setValue(s.customMarginMm);
    m_quality->setCurrentIndex(int(s.quality));
    m_customW->setEnabled(s.pageSize == PageSizeMode::Custom);
    m_customH->setEnabled(s.pageSize == PageSizeMode::Custom);
    m_customMargin->setEnabled(s.margins == MarginMode::Custom);
    m_orientation->setEnabled(s.pageSize != PageSizeMode::Auto);
    m_syncing = false;

    m_summary->clear();
    for (int i = 0; i < m_doc->count(); ++i) {
        const CombinePage& p = m_doc->page(i);
        const PagePlan pl = CombinePdfExporter::plan(p, s);
        auto* item = new QTreeWidgetItem(m_summary);
        item->setText(0, QString::number(i + 1));
        item->setText(1, p.displayName);
        item->setText(2, pl.landscape ? tr("Landscape") : tr("Portrait"));
        item->setText(3, QStringLiteral("%1 × %2 mm").arg(pl.pageSize.width() / 72 * 25.4, 0, 'f', 0).arg(pl.pageSize.height() / 72 * 25.4, 0, 'f', 0));
        item->setText(4, QStringLiteral("%1 × %2%3").arg(pl.imagePixels.width()).arg(pl.imagePixels.height()).arg(pl.passthroughJpeg ? tr(" · original JPEG") : QString()));
        item->setText(5, p.editSummary());
        if (!QFileInfo::exists(p.sourcePath)) {
            item->setForeground(1, tc().danger);
            item->setToolTip(1, tr("Source image missing"));
        }
    }
    for (int c = 0; c < 5; ++c)
        m_summary->resizeColumnToContents(c);
    const qint64 bytes = CombinePdfExporter::estimateTotalBytes(m_doc->pages(), s);
    m_estimate->setText(tr("%n page(s) · estimated size about %1 (approximate)", nullptr, m_doc->count()).arg(humanSize(bytes)));
    updateActions();
}

void CombineWorkspace::exportToPath(const QString& path, bool overwrite)
{
    startExport(path, overwrite);
}

bool CombineWorkspace::exportInProgress() const
{
    return m_exportWatcher && m_exportWatcher->isRunning();
}

void CombineWorkspace::startExport(const QString& path, bool overwrite)
{
    if (exportInProgress() || m_doc->isEmpty())
        return;
    if (m_canvas->isEditingText())
        m_canvas->commitText();
    for (const CombinePage& p : m_doc->pages())
        if (!QFileInfo::exists(p.sourcePath)) {
            m_exportBanner->show(Banner::Kind::Error, tr("Cannot export: the source image of \"%1\" is missing (%2).")
                                                          .arg(p.displayName, QDir::toNativeSeparators(p.sourcePath)));
            return;
        }
    ExportSettings s = m_doc->exportSettings();
    s.lastFolder = QFileInfo(path).absolutePath();
    s.fileName = QFileInfo(path).fileName();
    m_doc->setExportSettings(s);
    Database::instance().setSetting("combine.folder", s.lastFolder);
    const QList<CombinePage> pages = m_doc->pages();
    m_exportCancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = m_exportCancel;
    m_progress->setRange(0, int(pages.size()));
    m_progress->setValue(0);
    m_progress->show();
    m_cancelExport->show();
    m_exportButton->setEnabled(false);
    m_exportBanner->hide();
    m_progressLabel->setText(tr("Preparing…"));
    m_lastExportError.clear();
    if (!m_exportWatcher) {
        m_exportWatcher = new QFutureWatcher<ExportResult>(this);
        connect(m_exportWatcher, &QFutureWatcherBase::finished, this, &CombineWorkspace::onExportDone);
    }
    QPointer<CombineWorkspace> self(this);
    auto progress = [self](int done, int total, const QString& stage) {
        QMetaObject::invokeMethod(qApp, [self, done, total, stage] {
            if (!self)
                return;
            self->m_progress->setRange(0, total);
            self->m_progress->setValue(done);
            self->m_progressLabel->setText(stage);
        }, Qt::QueuedConnection);
    };
    m_exportWatcher->setFuture(QtConcurrent::run([path, pages, s, overwrite, progress, cancel] {
        return CombinePdfExporter::exportTo(path, pages, s, overwrite, progress, cancel.get());
    }));
}

void CombineWorkspace::onExportDone()
{
    const ExportResult r = m_exportWatcher->result();
    m_progress->hide();
    m_cancelExport->hide();
    m_exportButton->setEnabled(true);
    m_progressLabel->clear();
    disconnect(m_exportBanner, &Banner::actionTriggered, nullptr, nullptr);
    if (r.ok) {
        m_lastExport = r.path;
        m_exportBanner->show(Banner::Kind::Success,
                             tr("PDF exported successfully\n%1 · %n page(s) · %2", nullptr, r.pages)
                                 .arg(QDir::toNativeSeparators(r.path), humanSize(r.bytes)),
                             {tr("Open PDF"), tr("Open Folder"), tr("Done")});
        const QString path = r.path;
        connect(m_exportBanner, &Banner::actionTriggered, this, [this, path](int i) {
            if (i == 0)
                QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            else if (i == 1)
                revealInExplorer(path);
            else
                m_exportBanner->hide();
        });
        Database::instance().touchRecent(r.path, "pdf", "convert");
        Q_EMIT statusMessage(tr("PDF exported successfully."));
    } else {
        m_lastExportError = r.error;
        m_exportBanner->show(r.cancelled ? Banner::Kind::Info : Banner::Kind::Error,
                             r.cancelled ? tr("Export cancelled. No file was written.")
                                         : tr("Export failed: %1\nNo file was written.").arg(r.error));
    }
    Q_EMIT exportFinished(r.ok);
}

// ---- session / project ------------------------------------------------------------

void CombineWorkspace::scheduleAutosave()
{
    m_autosave->start();
}

void CombineWorkspace::offerSessionRecovery()
{
    if (m_sessionOffered || !m_doc->isEmpty())
        return;
    m_sessionOffered = true;
    if (!CombineSession::hasRecoverableSession())
        return;
    m_banner->show(Banner::Kind::Info, tr("An unfinished Combine session was found. Continue where you left off?"),
                   {tr("Restore"), tr("Discard")});
    disconnect(m_banner, &Banner::actionTriggered, nullptr, nullptr);
    connect(m_banner, &Banner::actionTriggered, this, [this](int i) {
        disconnect(m_banner, &Banner::actionTriggered, nullptr, nullptr);
        if (i == 0) {
            QString err;
            if (!CombineSession::restoreSession(*m_doc, &err)) {
                m_banner->show(Banner::Kind::Error, err);
                return;
            }
            const QStringList problems = CombineSession::sourceProblems(*m_doc);
            if (!problems.isEmpty())
                m_banner->show(Banner::Kind::Warning, problems.mid(0, 5).join("\n"));
            else
                m_banner->show(Banner::Kind::Success, tr("Session restored: %n page(s).", nullptr, m_doc->count()));
            m_arrangeStack->setCurrentWidget(m_grid);
        } else {
            CombineSession::discardSession(nullptr);
            m_banner->hide();
        }
    });
}

void CombineWorkspace::newProject()
{
    if (!m_doc->isEmpty()) {
        const auto r = QMessageBox::question(this, tr("New combine"), tr("Remove all pages and start over? Exported PDFs are not affected."));
        if (r != QMessageBox::Yes)
            return;
    }
    m_doc->clear();
    CombineSession::discardSession(nullptr);
    setStep(Step::Arrange);
}

void CombineWorkspace::saveProject()
{
    if (m_doc->isEmpty())
        return;
    QString path = QFileDialog::getSaveFileName(this, tr("Save Combine project"), QDir::homePath() + "/Documents/Combine.folvio",
                                                tr("Folvio Combine project (*.folvio)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(".folvio", Qt::CaseInsensitive))
        path += ".folvio";
    const auto embed = QMessageBox::question(this, tr("Save project"),
                                             tr("Embed all images in the project file?\n\nYes: the project works even if the "
                                                "original images are moved (larger file).\nNo: images are referenced by their "
                                                "location (pasted images are always embedded)."),
                                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    QString err;
    if (!CombineSession::saveProject(*m_doc, path, embed == QMessageBox::Yes, &err))
        QMessageBox::critical(this, tr("Save project"), err);
    else
        m_banner->show(Banner::Kind::Success, tr("Project saved to %1").arg(QDir::toNativeSeparators(path)));
}

void CombineWorkspace::openProject()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Combine project"), QString(), tr("Folvio Combine project (*.folvio)"));
    if (path.isEmpty())
        return;
    if (!m_doc->isEmpty()
        && QMessageBox::question(this, tr("Open project"), tr("Replace the current pages with the project?")) != QMessageBox::Yes)
        return;
    QStringList warnings;
    QString err;
    if (!CombineSession::openProject(*m_doc, path, &warnings, &err)) {
        QMessageBox::critical(this, tr("Open project"), err);
        return;
    }
    m_arrangeStack->setCurrentWidget(m_grid);
    if (!warnings.isEmpty())
        m_banner->show(Banner::Kind::Warning, tr("Some source images are not available:") + "\n" + warnings.mid(0, 6).join("\n"));
    else
        m_banner->show(Banner::Kind::Success, tr("Project opened: %n page(s).", nullptr, m_doc->count()));
    setStep(Step::Arrange);
}

bool CombineWorkspace::prepareClose()
{
    if (exportInProgress()) {
        const auto r = QMessageBox::question(this, tr("Export running"), tr("A PDF export is running. Cancel it and quit?"));
        if (r != QMessageBox::Yes)
            return false;
        m_exportCancel->store(true);
        m_exportWatcher->waitForFinished();
    }
    if (m_canvas->isEditingText())
        m_canvas->commitText();
    m_autosave->stop();
    if (!m_doc->isEmpty())
        CombineSession::autosave(*m_doc); // keeps the work for the next start
    return true;
}

void CombineWorkspace::updateActions()
{
    const bool has = !m_doc->isEmpty();
    m_undo->setEnabled(m_doc->canUndo());
    m_redo->setEnabled(m_doc->canRedo());
    m_undo->setToolTip(m_doc->canUndo() ? tr("Undo %1 (Ctrl+Z)").arg(m_doc->undoText()) : tr("Undo (Ctrl+Z)"));
    m_redo->setToolTip(m_doc->canRedo() ? tr("Redo %1 (Ctrl+Y)").arg(m_doc->redoText()) : tr("Redo (Ctrl+Y)"));
    m_arrangeStack->setCurrentWidget(has ? static_cast<QWidget*>(m_grid) : static_cast<QWidget*>(m_drop));
    const QList<int> sel = selectedRows(m_grid);
    m_btnRemove->setEnabled(!sel.isEmpty());
    m_btnDuplicate->setEnabled(!sel.isEmpty());
    m_btnUp->setEnabled(!sel.isEmpty() && sel.first() > 0);
    m_btnDown->setEnabled(!sel.isEmpty() && sel.last() < m_doc->count() - 1);
    m_btnEdit->setEnabled(has);
    m_btnNextEdit->setEnabled(has);
    m_btnSort->setEnabled(m_doc->count() > 1);
    if (m_saveProjectAction)
        m_saveProjectAction->setEnabled(has);
    for (int i = 1; i < 3; ++i)
        m_stepButtons->button(i)->setEnabled(has);
    if (m_exportButton)
        m_exportButton->setEnabled(has && !exportInProgress());
    int edited = 0;
    for (const CombinePage& p : m_doc->pages())
        edited += p.hasEdits() ? 1 : 0;
    m_arrangeInfo->setText(has ? tr("%n page(s)", nullptr, m_doc->count()) + tr(" · %1 edited · drag thumbnails to reorder, "
                                                                                "double-click to edit").arg(edited)
                               : QString());
}

} // namespace folvio::combine

namespace folvio::combine {
QListView* CombineWorkspace::arrangeView() const { return m_grid; }
QListView* CombineWorkspace::editPageList() const { return m_pageList; }
} // namespace folvio::combine
