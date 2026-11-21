// Combine: real GUI interaction tests for Arrange (drag & drop reordering) and Perspective.
//
// Version 0.10.0 passed its model-level tests while dragging thumbnails in the real application moved
// the *pictures* (QListView icon mode) instead of the pages. These tests therefore drive the widgets:
// mouse press on a card, mouse move with the button held (Qt starts the drag itself), drag-enter /
// drag-move / drop events delivered to the view at real card coordinates, and then they check the
// document order, the on-screen geometry of every card, the saved project and the exported PDF.
// Only the operating system's drag loop (OLE / X11 / Wayland), which QTest cannot drive, is replaced
// by delivering those events directly (PageGridView::setDragExecutorForTesting).
#include "TestSupport.h"

#include "combine/CombineModel.h"
#include "combine/CombinePdfExporter.h"
#include "combine/CombineSession.h"
#include "combine/CombineWorkspace.h"
#include "combine/ImageGeometry.h"
#include "combine/PageCanvas.h"
#include "combine/PageGridView.h"
#include "engines/pdf/PdfDocument.h"
#include "ui/themes/Theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDropEvent>
#include <QListView>
#include <QMimeData>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

using namespace folvio;
using namespace folvio::combine;
using namespace folvio::test;

namespace {

const QColor kColors[] = {QColor("#d32f2f"), QColor("#1976d2"), QColor("#388e3c"), QColor("#f57c00"), QColor("#7b1fa2")};

// A card image with a big letter on a colour that identifies it in thumbnails and in the PDF.
QString makeLetter(const QTemporaryDir& dir, const QString& letter, const QColor& color, QSize size = QSize(400, 560))
{
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    QPainter p(&img);
    QFont f("Arial");
    f.setPixelSize(size.height() / 2);
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(img.rect(), Qt::AlignCenter, letter);
    p.end();
    const QString path = dir.filePath(letter + ".png");
    img.save(path, "PNG");
    return path;
}

QStringList names(const CombineDocument* doc)
{
    QStringList out;
    for (const CombinePage& p : doc->pages())
        out << QFileInfo(p.displayName).completeBaseName();
    return out;
}

// Saves a screenshot of the widget when FOLVIO_TEST_SCREENSHOTS points to a folder.
void shot(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("FOLVIO_TEST_SCREENSHOTS");
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    QApplication::processEvents();
    w->grab().save(QDir(dir).filePath(name + ".png"));
}

struct Workspace {
    CombineWorkspace ws;
    explicit Workspace(QSize size = QSize(1300, 860))
    {
        if (!qEnvironmentVariableIsEmpty("FOLVIO_TEST_SCREENSHOTS"))
            Theme::instance().apply(Theme::Mode::Light); // screenshots should look like the application
        ws.resize(size);
        ws.show();
        ws.activateWindow(); // shortcuts need an active window (Xvfb has no window manager)
        EXPECT_TRUE(QTest::qWaitForWindowActive(&ws, 3000));
    }
    ~Workspace() { CombineSession::discardSession(nullptr); }
    bool import(const QStringList& files)
    {
        QSignalSpy done(&ws, &CombineWorkspace::importFinished);
        ws.importFiles(files);
        return done.wait(20000);
    }
    PageGridView* grid() const { return qobject_cast<PageGridView*>(ws.arrangeView()); }
    PageGridView* list() const { return qobject_cast<PageGridView*>(ws.editPageList()); }
};

QRect cardRect(QListView* v, int row) { return v->visualRect(v->model()->index(row, 0)); }
QPoint leftHalf(QListView* v, int row) { const QRect r = cardRect(v, row); return r.center() - QPoint(r.width() / 4, 0); }
QPoint rightHalf(QListView* v, int row) { const QRect r = cardRect(v, row); return r.center() + QPoint(r.width() / 4, 0); }
QPoint upperHalf(QListView* v, int row) { const QRect r = cardRect(v, row); return r.center() - QPoint(0, r.height() / 4); }
QPoint lowerHalf(QListView* v, int row) { const QRect r = cardRect(v, row); return r.center() + QPoint(0, r.height() / 4); }

enum class End { Drop, Cancel };

// Presses on card 'fromRow', moves with the button held until Qt starts the drag, then delivers
// drag-enter / drag-move events along 'path' and finishes with a drop (or a cancel = leaving the view,
// which is what Escape does in the platform drag loop).
Qt::DropAction dragCard(PageGridView* view, int fromRow, const QList<QPoint>& path, End end = End::Drop,
                        Qt::KeyboardModifiers pressModifiers = Qt::NoModifier, const QString& shotBeforeDrop = {})
{
    QWidget* vp = view->viewport();
    const QPoint start = cardRect(view, fromRow).center();
    bool started = false;
    Qt::DropAction result = Qt::IgnoreAction;
    PageGridView::setDragExecutorForTesting([&](QDrag* drag) {
        started = true;
        QMimeData* mime = drag->mimeData();
        QDragEnterEvent enter(start, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &enter);
        if (!enter.isAccepted())
            return Qt::IgnoreAction;
        for (const QPoint& p : path) {
            QDragMoveEvent move(p, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(vp, &move);
            QApplication::processEvents();
        }
        if (!shotBeforeDrop.isEmpty())
            shot(view->window(), shotBeforeDrop); // shows the insertion line
        if (end == End::Cancel) {
            QDragLeaveEvent leave;
            QApplication::sendEvent(vp, &leave);
            return Qt::IgnoreAction;
        }
        QDropEvent drop(path.last(), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &drop);
        result = drop.isAccepted() ? drop.dropAction() : Qt::IgnoreAction;
        return result;
    });
    QTest::mousePress(vp, Qt::LeftButton, pressModifiers, start);
    QTest::mouseMove(vp, start + QPoint(12, 6));
    QTest::mouseMove(vp, start + QPoint(40, 12));
    // Depending on the platform the release may reach the view after the drag loop; the view must
    // ignore it (it would otherwise act as a click on the card under the cursor).
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, start + QPoint(40, 12));
    PageGridView::setDragExecutorForTesting({});
    QApplication::processEvents();
    EXPECT_TRUE(started) << "the mouse gesture did not start a drag";
    return result;
}

// Every card occupies its own slot: equal size, no overlap, laid out row by row in page order,
// and the number drawn on the card is its position.
void expectRegularLayout(QListView* view, bool grid)
{
    const int n = view->model()->rowCount();
    QList<QRect> rects;
    for (int i = 0; i < n; ++i)
        rects << cardRect(view, i);
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(rects[i].size(), rects[0].size()) << "card " << i;
        for (int j = i + 1; j < n; ++j)
            EXPECT_FALSE(rects[i].intersects(rects[j])) << "cards " << i << " and " << j << " overlap";
        if (i > 0) {
            if (grid) {
                const bool sameRow = rects[i].top() == rects[i - 1].top();
                if (sameRow)
                    EXPECT_GT(rects[i].left(), rects[i - 1].right()) << "card " << i << " is not right of " << i - 1;
                else
                    EXPECT_GT(rects[i].top(), rects[i - 1].bottom()) << "card " << i << " starts a new row too early";
            } else {
                EXPECT_GT(rects[i].top(), rects[i - 1].bottom()) << "card " << i << " is not below " << i - 1;
            }
        }
        EXPECT_TRUE(view->model()->index(i, 0).data().toString().startsWith(QString::number(i + 1) + ". ")) << "number of card " << i;
    }
}

QColor pdfPageColor(PdfDocument* pdf, int page)
{
    const QImage img = pdf->render(page, 0.5);
    return QColor(img.pixel(img.width() / 2, img.height() / 6)); // above the letter
}

bool near(const QColor& a, const QColor& b, int tol = 45)
{
    return std::abs(a.red() - b.red()) < tol && std::abs(a.green() - b.green()) < tol && std::abs(a.blue() - b.blue()) < tol;
}

} // namespace

// ===========================================================================
// Arrange

TEST(CombineArrangeGui, DragAndDropReordersPagesUndoProjectAndPdf)
{
    QTemporaryDir dir;
    Workspace w;
    ASSERT_TRUE(w.import({makeLetter(dir, "A", kColors[0]), makeLetter(dir, "B", kColors[1]), makeLetter(dir, "C", kColors[2])}));
    PageGridView* grid = w.grid();
    ASSERT_TRUE(grid);
    ASSERT_EQ(names(w.ws.document()), QStringList({"A", "B", "C"}));
    expectRegularLayout(grid, true);
    // Thumbnails are rendered in the background.
    for (int i = 0; i < 3; ++i)
        QTRY_VERIFY_WITH_TIMEOUT(!grid->model()->index(i, 0).data(Qt::DecorationRole).isNull(), 10000);
    QTest::qWait(50);
    shot(&w.ws, "arrange-before-drag");

    // Drag A after C (right half of C): B, C, A
    EXPECT_EQ(dragCard(grid, 0, {cardRect(grid, 1).center(), rightHalf(grid, 2)}, End::Drop, Qt::NoModifier,
                       QStringLiteral("arrange-during-drag")),
              Qt::MoveAction);
    EXPECT_EQ(names(w.ws.document()), QStringList({"B", "C", "A"}));
    expectRegularLayout(grid, true);
    for (int i = 0; i < 3; ++i)
        QTRY_VERIFY_WITH_TIMEOUT(!grid->model()->index(i, 0).data(Qt::DecorationRole).isNull(), 10000);
    QTest::qWait(50);
    shot(&w.ws, "arrange-after-drag");
    // The moved page stays selected at its new place.
    ASSERT_EQ(grid->selectionModel()->selectedIndexes().size(), 1);
    EXPECT_EQ(grid->selectionModel()->selectedIndexes().first().row(), 2);

    // A to the beginning (left half of the first card): A, B, C
    dragCard(grid, 2, {cardRect(grid, 1).center(), leftHalf(grid, 0)});
    EXPECT_EQ(names(w.ws.document()), QStringList({"A", "B", "C"}));
    expectRegularLayout(grid, true);

    // B after C: A, C, B
    dragCard(grid, 1, {rightHalf(grid, 2)});
    EXPECT_EQ(names(w.ws.document()), QStringList({"A", "C", "B"}));
    expectRegularLayout(grid, true);

    // Undo / redo with the keyboard.
    grid->setFocus();
    QTest::keyClick(grid, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(names(w.ws.document()), QStringList({"A", "B", "C"}));
    QTest::keyClick(grid, Qt::Key_Y, Qt::ControlModifier);
    EXPECT_EQ(names(w.ws.document()), QStringList({"A", "C", "B"}));
    expectRegularLayout(grid, true);

    // Project round trip keeps order and unique identities.
    QSet<QUuid> ids;
    for (const CombinePage& p : w.ws.document()->pages())
        ids.insert(p.id);
    EXPECT_EQ(ids.size(), 3);
    const QString project = dir.filePath("order.folvio");
    QString err;
    ASSERT_TRUE(CombineSession::saveProject(*w.ws.document(), project, false, &err)) << err.toStdString();
    CombineDocument reopened;
    QStringList warnings;
    ASSERT_TRUE(CombineSession::openProject(reopened, project, &warnings, &err)) << err.toStdString();
    EXPECT_EQ(names(&reopened), QStringList({"A", "C", "B"}));
    QSet<QUuid> reopenedIds;
    for (const CombinePage& p : reopened.pages())
        reopenedIds.insert(p.id);
    EXPECT_EQ(reopenedIds.size(), 3);

    // The exported PDF has the pages in the new order.
    const QString pdfPath = dir.filePath("ordered.pdf");
    QSignalSpy exported(&w.ws, &CombineWorkspace::exportFinished);
    w.ws.exportToPath(pdfPath, false);
    ASSERT_TRUE(exported.wait(60000));
    ASSERT_TRUE(exported.first().first().toBool()) << w.ws.lastExportError().toStdString();
    auto pdf = PdfDocument::openFile(pdfPath);
    ASSERT_TRUE(pdf);
    ASSERT_EQ(pdf->pageCount(), 3);
    EXPECT_TRUE(near(pdfPageColor(pdf.get(), 0), kColors[0])) << "page 1 is not A";
    EXPECT_TRUE(near(pdfPageColor(pdf.get(), 1), kColors[2])) << "page 2 is not C";
    EXPECT_TRUE(near(pdfPageColor(pdf.get(), 2), kColors[1])) << "page 3 is not B";
}

TEST(CombineArrangeGui, ExactlyTwoPagesSwapBothWays)
{
    QTemporaryDir dir;
    Workspace w;
    ASSERT_TRUE(w.import({makeLetter(dir, "A", kColors[0]), makeLetter(dir, "B", kColors[1])}));
    PageGridView* grid = w.grid();
    dragCard(grid, 0, {rightHalf(grid, 1)});
    EXPECT_EQ(names(w.ws.document()), QStringList({"B", "A"}));
    expectRegularLayout(grid, true);
    dragCard(grid, 1, {leftHalf(grid, 0)});
    EXPECT_EQ(names(w.ws.document()), QStringList({"A", "B"}));
    expectRegularLayout(grid, true);
}

TEST(CombineArrangeGui, EmptyAreaCancelAndDropInPlace)
{
    QTemporaryDir dir;
    Workspace w;
    ASSERT_TRUE(w.import({makeLetter(dir, "A", kColors[0]), makeLetter(dir, "B", kColors[1]), makeLetter(dir, "C", kColors[2])}));
    PageGridView* grid = w.grid();
    // Empty space right of the last card / below everything = end of the list.
    const QRect last = cardRect(grid, 2);
    dragCard(grid, 0, {last.topRight() + QPoint(60, last.height() / 2)});
    EXPECT_EQ(names(w.ws.document()), QStringList({"B", "C", "A"}));
    dragCard(grid, 0, {QPoint(20, grid->viewport()->height() - 10)});
    EXPECT_EQ(names(w.ws.document()), QStringList({"C", "A", "B"}));
    expectRegularLayout(grid, true);

    // Cancel (Escape / leaving the view) restores nothing because nothing changed.
    const QString undoBefore = w.ws.document()->undoText();
    EXPECT_EQ(dragCard(grid, 0, {rightHalf(grid, 2)}, End::Cancel), Qt::IgnoreAction);
    EXPECT_EQ(names(w.ws.document()), QStringList({"C", "A", "B"}));
    EXPECT_EQ(grid->dropIndicatorIndex(), -1) << "the insertion line must disappear";

    // Dropping a card onto its own place changes nothing and adds no undo step.
    const int steps = w.ws.document()->canUndo() ? 1 : 0;
    dragCard(grid, 1, {leftHalf(grid, 1)});
    dragCard(grid, 1, {rightHalf(grid, 1)});
    EXPECT_EQ(names(w.ws.document()), QStringList({"C", "A", "B"}));
    EXPECT_EQ(w.ws.document()->undoText(), undoBefore);
    Q_UNUSED(steps)
    expectRegularLayout(grid, true);
}

TEST(CombineArrangeGui, SeveralSelectedPagesMoveTogether)
{
    QTemporaryDir dir;
    Workspace w;
    ASSERT_TRUE(w.import({makeLetter(dir, "A", kColors[0]), makeLetter(dir, "B", kColors[1]), makeLetter(dir, "C", kColors[2]),
                          makeLetter(dir, "D", kColors[3])}));
    PageGridView* grid = w.grid();
    QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::NoModifier, cardRect(grid, 1).center());
    QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::ControlModifier, cardRect(grid, 3).center());
    ASSERT_EQ(grid->selectionModel()->selectedIndexes().size(), 2);
    dragCard(grid, 3, {leftHalf(grid, 0)});
    EXPECT_EQ(names(w.ws.document()), QStringList({"B", "D", "A", "C"}));
    EXPECT_EQ(grid->selectionModel()->selectedIndexes().size(), 2) << "both moved pages stay selected";
    expectRegularLayout(grid, true);
    w.ws.document()->undo();
    EXPECT_EQ(names(w.ws.document()), QStringList({"A", "B", "C", "D"}));
}

TEST(CombineArrangeGui, ManyPagesAutoScrollAndResize)
{
    QTemporaryDir dir;
    Workspace w(QSize(900, 600));
    QStringList files;
    for (int i = 0; i < 40; ++i)
        files << makeLetter(dir, QStringLiteral("P%1").arg(i + 1, 2, 10, QLatin1Char('0')), kColors[i % 5], QSize(120, 168));
    ASSERT_TRUE(w.import(files));
    PageGridView* grid = w.grid();
    expectRegularLayout(grid, true);
    QScrollBar* bar = grid->verticalScrollBar();
    ASSERT_GT(bar->maximum(), 0) << "the test needs more cards than fit";
    // Hold the dragged card near the bottom edge until the view has scrolled to the end, then drop
    // it after the last card.
    QList<QPoint> path;
    for (int i = 0; i < 120; ++i)
        path << QPoint(grid->viewport()->width() / 2, grid->viewport()->height() - 8);
    QWidget* vp = grid->viewport();
    const QPoint start = cardRect(grid, 0).center();
    bool started = false;
    PageGridView::setDragExecutorForTesting([&](QDrag* drag) {
        started = true;
        QDragEnterEvent enter(start, Qt::MoveAction, drag->mimeData(), Qt::LeftButton, {});
        QApplication::sendEvent(vp, &enter);
        for (const QPoint& p : path) {
            QDragMoveEvent move(p, Qt::MoveAction, drag->mimeData(), Qt::LeftButton, {});
            QApplication::sendEvent(vp, &move);
        }
        EXPECT_EQ(bar->value(), bar->maximum()) << "autoscroll did not reach the end";
        const QPoint target = rightHalf(grid, 39);
        QDragMoveEvent move(target, Qt::MoveAction, drag->mimeData(), Qt::LeftButton, {});
        QApplication::sendEvent(vp, &move);
        QDropEvent drop(target, Qt::MoveAction, drag->mimeData(), Qt::LeftButton, {});
        QApplication::sendEvent(vp, &drop);
        return drop.dropAction();
    });
    QTest::mousePress(vp, Qt::LeftButton, {}, start);
    QTest::mouseMove(vp, start + QPoint(12, 6));
    QTest::mouseMove(vp, start + QPoint(40, 12));
    QTest::mouseRelease(vp, Qt::LeftButton, {}, start + QPoint(40, 12));
    PageGridView::setDragExecutorForTesting({});
    ASSERT_TRUE(started);
    QStringList n = names(w.ws.document());
    EXPECT_EQ(n.last(), "P01");
    EXPECT_EQ(n.first(), "P02");
    expectRegularLayout(grid, true);
    // Narrower and wider windows re-flow the grid without gaps or overlaps, order unchanged.
    for (int width : {640, 1500}) {
        w.ws.resize(width, 700);
        QTest::qWait(80);
        expectRegularLayout(grid, true);
        EXPECT_EQ(names(w.ws.document()), n);
    }
}

TEST(CombineArrangeGui, EditPageListReordersVertically)
{
    QTemporaryDir dir;
    Workspace w;
    ASSERT_TRUE(w.import({makeLetter(dir, "A", kColors[0]), makeLetter(dir, "B", kColors[1]), makeLetter(dir, "C", kColors[2])}));
    w.ws.setStep(CombineWorkspace::Step::Edit);
    QTest::qWait(100);
    PageGridView* list = w.list();
    ASSERT_TRUE(list && list->isVisible());
    expectRegularLayout(list, false);
    dragCard(list, 0, {lowerHalf(list, 2)});
    EXPECT_EQ(names(w.ws.document()), QStringList({"B", "C", "A"}));
    dragCard(list, 2, {upperHalf(list, 0)});
    EXPECT_EQ(names(w.ws.document()), QStringList({"A", "B", "C"}));
    expectRegularLayout(list, false);
}

// ===========================================================================
// Perspective

namespace {

QImage makeDocumentPage(QSize size = QSize(840, 1188))
{
    QImage img(size, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    const int m = size.width() / 14;
    p.fillRect(QRect(m, m, m * 3, m * 2), kColors[1]);
    QFont f("Arial");
    f.setPixelSize(size.width() / 28);
    p.setFont(f);
    p.setPen(Qt::black);
    for (int i = 0; i < 14; ++i)
        p.drawText(m, m * 4 + i * size.width() / 18, QStringLiteral("Line %1 of the photographed page").arg(i + 1));
    const int s = size.width() / 21;
    for (const QPoint& c : {QPoint(8, 8), QPoint(size.width() - 8 - s, 8), QPoint(size.width() - 8 - s, size.height() - 8 - s),
                            QPoint(8, size.height() - 8 - s)})
        p.fillRect(QRect(c, QSize(s, s)), Qt::black);
    return img;
}

QImage photographed(const QImage& doc, const Quad& quad, QSize canvas = QSize(1600, 1200))
{
    QImage photo(canvas, QImage::Format_RGB32);
    photo.fill(QColor(70, 55, 45));
    QTransform t;
    QTransform::quadToQuad(QPolygonF({QPointF(0, 0), QPointF(doc.width(), 0), QPointF(doc.width(), doc.height()), QPointF(0, doc.height())}),
                           QPolygonF({quad.pts[0], quad.pts[1], quad.pts[2], quad.pts[3]}), t);
    QPainter p(&photo);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    p.setTransform(t);
    p.drawImage(0, 0, doc);
    return photo;
}

void dragPoint(QWidget* canvas, const QPointF& from, const QPointF& to)
{
    QTest::mousePress(canvas, Qt::LeftButton, {}, from.toPoint());
    for (int i = 1; i <= 6; ++i)
        QTest::mouseMove(canvas, (from + (to - from) * (i / 6.0)).toPoint());
    QTest::mouseRelease(canvas, Qt::LeftButton, {}, to.toPoint());
}

} // namespace

TEST(CombinePerspectiveGui, FourIndependentCornersStraightenAnyConvexQuad)
{
    QTemporaryDir dir;
    const QImage doc = makeDocumentPage();
    // A clearly irregular quadrilateral: neither rectangle nor trapezoid.
    Quad truth;
    truth.pts = {QPointF(430, 140), QPointF(1170, 230), QPointF(1310, 1090), QPointF(280, 1010)};
    const QString photo = dir.filePath("angled.png");
    photographed(doc, truth).save(photo, "PNG");
    Workspace w(QSize(1400, 950));
    ASSERT_TRUE(w.import({photo}));
    w.ws.setStep(CombineWorkspace::Step::Edit);
    QTest::qWait(100);
    auto* perspectiveTool = w.ws.findChild<QAbstractButton*>("tool-perspective");
    auto* cropTool = w.ws.findChild<QAbstractButton*>("tool-crop");
    ASSERT_TRUE(perspectiveTool && cropTool);
    QTest::mouseClick(perspectiveTool, Qt::LeftButton);
    PageCanvas* canvas = w.ws.canvas();
    ASSERT_EQ(canvas->tool(), PageCanvas::Tool::Perspective);
    // Start from the default rectangle so every corner really has to be placed by the user.
    canvas->resetQuad();
    const Quad initial = *canvas->pendingQuad();
    auto scroll = qobject_cast<QScrollArea*>(canvas->parentWidget()->parentWidget());
    auto dragCorner = [&](int c, const QPointF& target) {
        const QPointF from = canvas->fullToWidget(canvas->pendingQuad()->pts[size_t(c)]);
        if (scroll)
            scroll->ensureVisible(int(from.x()), int(from.y()), 80, 80);
        QTest::qWait(20);
        dragPoint(canvas, canvas->fullToWidget(canvas->pendingQuad()->pts[size_t(c)]), canvas->fullToWidget(target));
    };
    const double tolerance = 2.5 * double(canvas->rotatedSourceSize().width()) / canvas->imageRect().width();

    // 1. Only TL moves.
    dragCorner(0, truth.pts[0]);
    Quad q = *canvas->pendingQuad();
    EXPECT_LT(QLineF(q.pts[0], truth.pts[0]).length(), tolerance);
    for (int c = 1; c < 4; ++c)
        EXPECT_EQ(q.pts[size_t(c)], initial.pts[size_t(c)]) << "corner " << c << " moved together with TL";

    // 2. Only BR moves.
    dragCorner(2, truth.pts[2]);
    q = *canvas->pendingQuad();
    EXPECT_LT(QLineF(q.pts[2], truth.pts[2]).length(), tolerance);
    EXPECT_LT(QLineF(q.pts[0], truth.pts[0]).length(), tolerance);
    EXPECT_EQ(q.pts[1], initial.pts[1]);
    EXPECT_EQ(q.pts[3], initial.pts[3]);

    // 3. TR through its own X/Y fields.
    QTest::mouseClick(w.ws.findChild<QAbstractButton*>("corner1"), Qt::LeftButton);
    auto* x = w.ws.findChild<QDoubleSpinBox*>("cornerX");
    auto* y = w.ws.findChild<QDoubleSpinBox*>("cornerY");
    ASSERT_TRUE(x && y);
    x->setValue(truth.pts[1].x());
    y->setValue(truth.pts[1].y());
    q = *canvas->pendingQuad();
    EXPECT_LT(QLineF(q.pts[1], truth.pts[1]).length(), 1.0);
    EXPECT_EQ(q.pts[3], initial.pts[3]);

    // 4. BL with the mouse while zoomed in to 200 % (coordinate mapping with zoom and scrolling).
    canvas->setZoom(2.0);
    QTest::qWait(50);
    dragCorner(3, truth.pts[3]);
    q = *canvas->pendingQuad();
    const double zoomedTolerance = 2.5 * double(canvas->rotatedSourceSize().width()) / canvas->imageRect().width();
    EXPECT_LT(QLineF(q.pts[3], truth.pts[3]).length(), zoomedTolerance);

    // 5. Keyboard: Shift+Right moves only the selected corner by ten screen pixels, Shift+Left back.
    canvas->selectCorner(1);
    canvas->setFocus();
    const QPointF before = canvas->pendingQuad()->pts[1];
    QTest::keyClick(canvas, Qt::Key_Right, Qt::ShiftModifier);
    EXPECT_GT(canvas->pendingQuad()->pts[1].x(), before.x());
    EXPECT_EQ(canvas->pendingQuad()->pts[0], q.pts[0]);
    QTest::keyClick(canvas, Qt::Key_Left, Qt::ShiftModifier);
    EXPECT_LT(QLineF(canvas->pendingQuad()->pts[1], before).length(), 0.01);
    canvas->setZoom(1.0);
    canvas->fitToView(scroll ? scroll->viewport()->size() : canvas->size());
    QTest::qWait(30);
    shot(&w.ws, "perspective-independent-corners");

    ASSERT_TRUE(canvas->pendingQuadCheck().ok) << canvas->pendingQuadCheck().error.toStdString();
    const Quad placed = *canvas->pendingQuad();

    // 6. Straighten.
    auto* straighten = w.ws.findChild<QAbstractButton*>("straighten");
    ASSERT_TRUE(straighten && straighten->isEnabled());
    QTest::mouseClick(straighten, Qt::LeftButton);
    const CombinePage page = w.ws.document()->page(0);
    ASSERT_TRUE(page.quad.has_value());
    for (int c = 0; c < 4; ++c)
        EXPECT_EQ(page.quad->pts[size_t(c)], placed.pts[size_t(c)]);
    EXPECT_EQ(canvas->compareMode(), PageCanvas::CompareMode::Corrected);
    shot(&w.ws, "perspective-straightened");

    // A click on the corrected preview shows the corners again instead of moving an invisible one.
    QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->imageRect().center().toPoint());
    EXPECT_EQ(canvas->compareMode(), PageCanvas::CompareMode::Original);
    for (int c = 0; c < 4; ++c)
        EXPECT_EQ(canvas->pendingQuad()->pts[size_t(c)], placed.pts[size_t(c)]) << "corner " << c << " changed by the click";

    // 7. Result geometry: upright page with the document's proportions and content.
    const QImage result = ImageProcessor::render(w.ws.document()->page(0), ImageProcessor::Stage::Final, 0);
    ASSERT_FALSE(result.isNull());
    EXPECT_GT(result.height(), result.width());
    const QImage ref = doc.scaled(result.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
    const QImage got = result.convertToFormat(QImage::Format_Grayscale8);
    double diff = 0;
    for (int yy = 6; yy < got.height() - 6; ++yy)
        for (int xx = 6; xx < got.width() - 6; ++xx)
            diff += std::abs(int(got.constScanLine(yy)[xx]) - int(ref.constScanLine(yy)[xx]));
    diff /= double(got.width() - 12) * (got.height() - 12);
    EXPECT_LT(diff, 18.0) << "straightened page does not match the document";

    // 8. Crop is a different tool: it changes a rectangle, never the corners.
    QTest::mouseClick(cropTool, Qt::LeftButton);
    ASSERT_EQ(canvas->tool(), PageCanvas::Tool::Crop);
    const QRectF ir = canvas->imageRect();
    dragPoint(canvas, ir.topLeft() + QPointF(2, 2), ir.topLeft() + QPointF(ir.width() * 0.2, ir.height() * 0.2));
    EXPECT_LT(canvas->pendingCrop().left(), 0.3);
    EXPECT_EQ(w.ws.document()->page(0).quad->pts[0], placed.pts[0]);

    // 9. Export and check the rendered PDF page (colour block top left, black corner marks).
    const QString pdfPath = dir.filePath("straight.pdf");
    QSignalSpy exported(&w.ws, &CombineWorkspace::exportFinished);
    w.ws.exportToPath(pdfPath, false);
    ASSERT_TRUE(exported.wait(60000));
    ASSERT_TRUE(exported.first().first().toBool()) << w.ws.lastExportError().toStdString();
    auto pdf = PdfDocument::openFile(pdfPath);
    ASSERT_TRUE(pdf);
    const QImage pageImg = pdf->render(0, 1.0);
    const QRectF imgRect = CombinePdfExporter::plan(w.ws.document()->page(0), w.ws.document()->exportSettings()).imageRect;
    const int m = int(imgRect.width() / 14);
    EXPECT_TRUE(near(QColor(pageImg.pixel(int(imgRect.left()) + 2 * m, int(imgRect.top()) + 2 * m)), kColors[1]));
}
