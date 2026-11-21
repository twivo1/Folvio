// Combine editing in the real GUI: Crop (the visible result, fitting, annotations staying on their content)
// and the text editor (transparent box, live formatting while typing, moving, resizing without scaling).
// Checks look at rendered pixels of the canvas and of the exported PDF, not only at stored values.
#include "TestSupport.h"

#include "combine/AnnotationRender.h"
#include "combine/CombineModel.h"
#include "combine/CombinePdfExporter.h"
#include "combine/CombineSession.h"
#include "combine/CombineWorkspace.h"
#include "combine/ImageGeometry.h"
#include "combine/PageCanvas.h"
#include "engines/pdf/PdfDocument.h"
#include "ui/themes/Theme.h"

#include <QAbstractButton>
#include <QClipboard>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QListView>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <gtest/gtest.h>

using namespace folvio;
using namespace folvio::combine;
using namespace folvio::test;

namespace {

const QColor kBackground(120, 60, 160); // desktop around the notes (a colour text anti-aliasing cannot produce)
const QColor kNotes(250, 250, 250);
const QColor kMarker(220, 30, 30);
const QColor kOutside(30, 170, 60);

// 1920x1080 "screenshot": grey desktop, a white notes window (200,150)-(900,600) with a red marker and
// text, and a green marker elsewhere on the desktop.
QImage screenshot(QSize size = QSize(1920, 1080))
{
    const double k = size.width() / 1920.0;
    QImage img(size, QImage::Format_RGB32);
    img.fill(kBackground);
    QPainter p(&img);
    p.scale(k, k);
    p.fillRect(QRect(200, 150, 700, 450), kNotes);
    p.fillRect(QRect(300, 250, 40, 40), kMarker);
    p.fillRect(QRect(1500, 800, 60, 60), kOutside);
    QFont f("Arial");
    f.setPixelSize(22);
    p.setFont(f);
    p.setPen(Qt::black);
    for (int i = 0; i < 6; ++i)
        p.drawText(380, 280 + i * 40, QStringLiteral("Note line %1").arg(i + 1));
    return img;
}

bool near(const QColor& a, const QColor& b, int tol = 40)
{
    return std::abs(a.red() - b.red()) < tol && std::abs(a.green() - b.green()) < tol && std::abs(a.blue() - b.blue()) < tol;
}

void typeText(QWidget* w, const QString& text)
{
    for (const QChar ch : text) {
        if (ch == '\n') {
            QTest::keyClick(w, Qt::Key_Return);
            continue;
        }
        QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, QString(ch));
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_unknown, Qt::NoModifier, QString(ch));
        QApplication::sendEvent(w, &press);
        QApplication::sendEvent(w, &release);
    }
}

void drag(QWidget* w, QPointF from, QPointF to)
{
    QTest::mousePress(w, Qt::LeftButton, {}, from.toPoint());
    for (int i = 1; i <= 6; ++i)
        QTest::mouseMove(w, (from + (to - from) * (i / 6.0)).toPoint());
    QTest::mouseRelease(w, Qt::LeftButton, {}, to.toPoint());
}

void shot(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("FOLVIO_TEST_SCREENSHOTS");
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    QApplication::processEvents();
    w->grab().save(QDir(dir).filePath(name + ".png"));
}

struct Editor {
    CombineWorkspace ws;
    PageCanvas* canvas = nullptr;
    QScrollArea* scroll = nullptr;
    explicit Editor(QSize size = QSize(1400, 950))
    {
        if (!qEnvironmentVariableIsEmpty("FOLVIO_TEST_SCREENSHOTS"))
            Theme::instance().apply(Theme::Mode::Light);
        ws.resize(size);
        ws.show();
        ws.activateWindow();
        EXPECT_TRUE(QTest::qWaitForWindowActive(&ws, 3000));
        canvas = ws.canvas();
        scroll = qobject_cast<QScrollArea*>(canvas->parentWidget()->parentWidget());
    }
    ~Editor() { CombineSession::discardSession(nullptr); }
    bool open(const QString& file)
    {
        QSignalSpy done(&ws, &CombineWorkspace::importFinished);
        ws.importFiles({file});
        if (!done.wait(20000))
            return false;
        ws.setStep(CombineWorkspace::Step::Edit);
        QTest::qWait(120);
        return true;
    }
    QAbstractButton* button(const QString& objectNameOrText)
    {
        if (auto* b = ws.findChild<QAbstractButton*>(objectNameOrText))
            return b;
        for (auto* b : ws.findChildren<QAbstractButton*>())
            if (b->text() == objectNameOrText)
                return b;
        return nullptr;
    }
    void tool(const char* name) { QTest::mouseClick(button(QStringLiteral("tool-") + QLatin1String(name)), Qt::LeftButton); }
    // Pixel of the canvas as the user sees it (child widgets such as the text box included).
    QColor pixel(const QPointF& widgetPos)
    {
        const QImage img = canvas->grab().toImage();
        const double dpr = img.devicePixelRatio();
        return QColor(img.pixel(int(widgetPos.x() * dpr), int(widgetPos.y() * dpr)));
    }
    // Drags the crop frame handles so that the frame covers 'fullPx' (pixels of the uncropped page).
    void cropTo(const QRectF& fullPx)
    {
        const QRectF r = canvas->imageRect();
        const QSizeF full = Geometry::rectifiedPixelSize(ws.document()->page(0));
        auto toWidget = [&](const QPointF& px) { return QPointF(r.left() + px.x() / full.width() * r.width(), r.top() + px.y() / full.height() * r.height()); };
        const QRectF frame = canvas->pendingCrop();
        const QPointF tl(r.left() + frame.left() * r.width(), r.top() + frame.top() * r.height());
        const QPointF br(r.left() + frame.right() * r.width(), r.top() + frame.bottom() * r.height());
        if (scroll)
            scroll->ensureVisible(int(tl.x()), int(tl.y()), 60, 60);
        drag(canvas, tl, toWidget(fullPx.topLeft()));
        if (scroll)
            scroll->ensureVisible(int(br.x()), int(br.y()), 60, 60);
        drag(canvas, br, toWidget(fullPx.bottomRight()));
    }
};

} // namespace

// ===========================================================================
// Crop

TEST(CombineCropGui, ApplyShowsTheCroppedPageFittedAndAnnotationsStayOnTheirContent)
{
    QTemporaryDir dir;
    const QString file = dir.filePath("screenshot.png");
    screenshot().save(file, "PNG");
    Editor e;
    ASSERT_TRUE(e.open(file));

    // A pen stroke right below the red marker (uncropped page units = px / 1920).
    e.tool("pen");
    const QPointF a(310.0 / 1920, 320.0 / 1920), b(330.0 / 1920, 320.0 / 1920);
    drag(e.canvas, e.canvas->fromPage(a), e.canvas->fromPage(b));
    ASSERT_EQ(e.ws.document()->page(0).annotations.size(), 1);
    const Annotation strokeBefore = e.ws.document()->page(0).annotations[0];

    e.tool("crop");
    ASSERT_EQ(e.canvas->tool(), PageCanvas::Tool::Crop);
    e.cropTo(QRectF(200, 150, 700, 450));
    shot(&e.ws, "crop-frame");
    QTest::mouseClick(e.button(QObject::tr("Apply crop")), Qt::LeftButton);
    QTest::qWait(80);

    // 1. Stored crop.
    const CombinePage page = e.ws.document()->page(0);
    ASSERT_TRUE(page.crop.has_value());
    EXPECT_NEAR(page.crop->left(), 200.0 / 1920, 0.004);
    EXPECT_NEAR(page.crop->top(), 150.0 / 1080, 0.004);
    EXPECT_NEAR(page.crop->width(), 700.0 / 1920, 0.006);
    EXPECT_NEAR(page.crop->height(), 450.0 / 1080, 0.006);

    // 2. The canvas shows the result, not the frame on the full screenshot.
    EXPECT_EQ(e.canvas->tool(), PageCanvas::Tool::Select);
    EXPECT_TRUE(e.button("tool-select")->isChecked());
    const QRectF ir = e.canvas->imageRect();
    EXPECT_NEAR(ir.width() / ir.height(), 700.0 / 450.0, 0.03);
    // Fit to view: the whole page is visible and fills one direction of the viewport.
    const QSize vp = e.scroll->viewport()->size();
    EXPECT_LE(ir.width(), vp.width());
    EXPECT_LE(ir.height(), vp.height());
    EXPECT_TRUE(ir.width() > 0.8 * (vp.width() - 60) || ir.height() > 0.8 * (vp.height() - 60));
    shot(&e.ws, "crop-applied");

    // 3. Pixels: no grey desktop inside the page, the red marker where it belongs, no green marker.
    const QImage canvasImg = e.canvas->grab().toImage();
    const double dpr = canvasImg.devicePixelRatio();
    int grey = 0, green = 0;
    for (int yy = 2; yy < 38; ++yy)
        for (int xx = 2; xx < 38; ++xx) {
            const QColor c(canvasImg.pixel(int((ir.left() + ir.width() * xx / 40.0) * dpr), int((ir.top() + ir.height() * yy / 40.0) * dpr)));
            grey += near(c, kBackground, 12) ? 1 : 0;
            green += near(c, kOutside, 30) ? 1 : 0;
        }
    EXPECT_EQ(grey, 0) << "area outside the crop is still visible";
    EXPECT_EQ(green, 0);
    const QPointF markerCenter(ir.left() + ir.width() * (320.0 - 200) / 700, ir.top() + ir.height() * (270.0 - 150) / 450);
    EXPECT_TRUE(near(e.pixel(markerCenter), kMarker)) << "marker not at its place in the cropped page";

    // 4. The annotation is still right below the marker (on the same content).
    const Annotation strokeAfter = e.ws.document()->page(0).annotations[0];
    const QPointF expected((320.0 - 200) / 700, (320.0 - 150) / 700); // page units of the cropped page
    EXPECT_LT(QLineF(strokeAfter.points[strokeAfter.points.size() / 2], expected).length(), 0.02);
    EXPECT_NEAR(strokeAfter.width / (strokeBefore.width * 1920.0 / 700), 1.0, 0.01) << "stroke width must keep its size on the content";

    // 5. Thumbnail shows the cropped page.
    auto* grid = e.ws.arrangeView();
    auto thumbAspect = [grid] {
        const QPixmap t = grid->model()->index(0, 0).data(Qt::DecorationRole).value<QPixmap>();
        return t.isNull() ? 0.0 : double(t.width()) / t.height();
    };
    // Thumbnails are rendered in the background; the stale (uncropped) one is replaced shortly.
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(thumbAspect() - 700.0 / 450.0) < 0.05, 10000);

    // 6. Undo restores the uncropped page and the original annotation; redo crops again.
    e.canvas->setFocus();
    QTest::keyClick(e.canvas, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_FALSE(e.ws.document()->page(0).crop.has_value());
    EXPECT_EQ(e.ws.document()->page(0).annotations[0].points, strokeBefore.points);
    QTest::keyClick(e.canvas, Qt::Key_Y, Qt::ControlModifier);
    ASSERT_TRUE(e.ws.document()->page(0).crop.has_value());

    // 7. Crop again: the frame starts at the current crop, on the full screenshot.
    e.tool("crop");
    EXPECT_NEAR(e.canvas->pendingCrop().width(), page.crop->width(), 1e-6);
    EXPECT_NEAR(e.canvas->imageRect().width() / e.canvas->imageRect().height(), 1920.0 / 1080, 0.02);
    e.tool("select");

    // 8. Project round trip.
    const QString project = dir.filePath("crop.folvio");
    QString err;
    ASSERT_TRUE(CombineSession::saveProject(*e.ws.document(), project, false, &err)) << err.toStdString();
    CombineDocument reopened;
    QStringList warnings;
    ASSERT_TRUE(CombineSession::openProject(reopened, project, &warnings, &err)) << err.toStdString();
    ASSERT_TRUE(reopened.page(0).crop.has_value());
    EXPECT_NEAR(reopened.page(0).crop->width(), page.crop->width(), 1e-9);
    EXPECT_EQ(reopened.page(0).annotations[0].points, strokeAfter.points);

    // 9. PDF: page with the crop's proportions, marker at its place.
    ExportSettings s = e.ws.document()->exportSettings();
    s.pageSize = PageSizeMode::Auto;
    s.margins = MarginMode::None;
    e.ws.document()->setExportSettings(s);
    const QString pdfPath = dir.filePath("cropped.pdf");
    QSignalSpy exported(&e.ws, &CombineWorkspace::exportFinished);
    e.ws.exportToPath(pdfPath, false);
    ASSERT_TRUE(exported.wait(60000));
    ASSERT_TRUE(exported.first().first().toBool()) << e.ws.lastExportError().toStdString();
    auto pdf = PdfDocument::openFile(pdfPath);
    ASSERT_TRUE(pdf);
    const QSizeF ps = pdf->pageSize(0);
    EXPECT_NEAR(ps.width() / ps.height(), 700.0 / 450.0, 0.02);
    const QImage pdfImg = pdf->render(0, 1.0);
    EXPECT_TRUE(near(QColor(pdfImg.pixel(int(pdfImg.width() * 120.0 / 700), int(pdfImg.height() * 120.0 / 450))), kMarker));
}

TEST(CombineCropGui, JpgSmallLargeZoomedAndAfterPerspective)
{
    QTemporaryDir dir;
    struct Case {
        QString name;
        QSize size;
        double zoom;
    };
    const Case cases[] = {{"photo.jpg", QSize(1920, 1080), 0}, {"small.png", QSize(192, 108), 0},
                          {"large.png", QSize(5760, 3240), 0}, {"zoomed.png", QSize(1920, 1080), 2.0}};
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name.toStdString());
        const QString file = dir.filePath(c.name);
        screenshot(c.size).save(file, c.name.endsWith(".jpg") ? "JPG" : "PNG", 92);
        Editor e;
        ASSERT_TRUE(e.open(file));
        e.tool("crop");
        if (c.zoom > 0) {
            e.canvas->setZoom(c.zoom);
            QTest::qWait(50);
        }
        const double k = c.size.width() / 1920.0;
        e.cropTo(QRectF(200 * k, 150 * k, 700 * k, 450 * k));
        QTest::mouseClick(e.button(QObject::tr("Apply crop")), Qt::LeftButton);
        QTest::qWait(60);
        const auto crop = e.ws.document()->page(0).crop;
        ASSERT_TRUE(crop.has_value());
        // Tolerance: one screen pixel of the frame in page fractions.
        EXPECT_NEAR(crop->width(), 700.0 / 1920, 0.01);
        EXPECT_NEAR(crop->height(), 450.0 / 1080, 0.012);
        EXPECT_EQ(e.canvas->tool(), PageCanvas::Tool::Select);
        const QRectF ir = e.canvas->imageRect();
        EXPECT_NEAR(ir.width() / ir.height(), 700.0 / 450, 0.04);
        const QPointF marker(ir.left() + ir.width() * 120.0 / 700, ir.top() + ir.height() * 120.0 / 450);
        EXPECT_TRUE(near(e.pixel(marker), kMarker)) << c.name.toStdString();
    }

    // After perspective: the crop works on the straightened page.
    const QString photo = dir.filePath("angled.png");
    screenshot().save(photo, "PNG");
    Editor e;
    ASSERT_TRUE(e.open(photo));
    CombinePage p = e.ws.document()->page(0);
    Quad q;
    q.pts = {QPointF(200, 150), QPointF(900, 150), QPointF(900, 600), QPointF(200, 600)}; // the notes window
    p.quad = q;
    e.ws.document()->updatePage(p, "perspective");
    e.tool("crop");
    const QSizeF rect = Geometry::rectifiedPixelSize(e.ws.document()->page(0));
    e.cropTo(QRectF(0, 0, rect.width() / 2, rect.height() / 2));
    QTest::mouseClick(e.button(QObject::tr("Apply crop")), Qt::LeftButton);
    const QImage finalImg = ImageProcessor::render(e.ws.document()->page(0), ImageProcessor::Stage::Final, 0);
    EXPECT_NEAR(finalImg.width(), rect.width() / 2, 4);
    EXPECT_NEAR(finalImg.height(), rect.height() / 2, 4);
    // The marker (at 100..140 / 100..140 of the notes window) is inside the top-left quarter.
    EXPECT_TRUE(near(QColor(finalImg.pixel(int(120.0 / 700 * rect.width()), int(120.0 / 450 * rect.height()))), kMarker));
}

// ===========================================================================
// Text editor

TEST(CombineTextGui, TransparentBoxLiveFormattingMoveResizeAndPdf)
{
    QTemporaryDir dir;
    const QColor photo(40, 120, 200);
    QImage img(1200, 800, QImage::Format_RGB32);
    img.fill(photo);
    const QString file = dir.filePath("blue.png");
    img.save(file, "PNG");
    Editor e;
    ASSERT_TRUE(e.open(file));

    // 1. New text box: the page stays visible under it (no white rectangle).
    e.tool("text");
    QTest::mouseClick(e.canvas, Qt::LeftButton, {}, e.canvas->fromPage(QPointF(0.1, 0.2)).toPoint());
    auto* editor = e.canvas->textEditor();
    ASSERT_TRUE(editor->isVisible());
    const QRect box = editor->geometry();
    EXPECT_TRUE(near(e.pixel(QPointF(box.center()) + QPointF(box.width() / 4.0, 0)), photo, 25))
        << "the text box covers the page";
    typeText(editor, "Hello World");
    shot(&e.ws, "text-typing");
    EXPECT_TRUE(near(e.pixel(QPointF(box.right() - 6, box.center().y())), photo, 25)) << "background behind the text is not transparent";

    // 2. Colour through the real colour dialog during the first edit: applied at once, typing continues.
    QTimer::singleShot(300, [] {
        if (auto* dlg = qobject_cast<QColorDialog*>(QApplication::activeModalWidget())) {
            dlg->setCurrentColor(QColor(220, 0, 0));
            dlg->accept();
        }
    });
    QWidget* colour = nullptr;
    for (auto* b : e.ws.findChildren<QAbstractButton*>())
        if (b->toolTip() == QObject::tr("Colour") && b->isVisible())
            colour = b;
    ASSERT_TRUE(colour);
    QTest::mouseClick(colour, Qt::LeftButton);
    QTest::qWait(400);
    ASSERT_TRUE(editor->isVisible()) << "opening the colour dialog ended the edit";
    ASSERT_TRUE(e.canvas->editingText().has_value());
    EXPECT_EQ(e.canvas->editingText()->stroke, QColor(220, 0, 0));
    // Red glyph pixels inside the box right away.
    {
        const QImage g = e.canvas->grab().toImage();
        const double dpr = g.devicePixelRatio();
        int red = 0;
        for (int y = box.top() + 2; y < box.bottom() - 2; ++y)
            for (int x = box.left() + 2; x < box.left() + box.width() / 2; ++x)
                red += near(QColor(g.pixel(int(x * dpr), int(y * dpr))), QColor(220, 0, 0), 70) ? 1 : 0;
        EXPECT_GT(red, 20) << "the colour change is not visible on the text being typed";
    }

    // 3. Size, font family and bold apply immediately too.
    const auto size = e.ws.findChildren<QDoubleSpinBox*>();
    QDoubleSpinBox* fontSize = nullptr;
    for (auto* sb : size)
        if (sb->suffix() == " pt" && sb->isVisible() && sb->maximum() == 120)
            fontSize = sb;
    ASSERT_TRUE(fontSize);
    fontSize->setValue(24);
    EXPECT_NEAR(e.canvas->editingText()->fontSize * 595.0, 24.0, 0.01);
    QComboBox* family = nullptr;
    for (auto* cb : e.ws.findChildren<QComboBox*>())
        if (cb->isVisible() && cb->findText(e.canvas->editingText()->fontFamily) >= 0 && cb->count() > 1)
            family = cb;
    ASSERT_TRUE(family);
    const QString otherFamily = family->itemText(family->currentIndex() == 0 ? 1 : 0);
    family->setCurrentText(otherFamily);
    EXPECT_EQ(e.canvas->editingText()->fontFamily, otherFamily);
    e.canvas->style().bold = true;
    e.canvas->setEditingTextStyle(e.canvas->style());
    EXPECT_TRUE(e.canvas->editingText()->bold);
    ASSERT_TRUE(editor->isVisible());

    // 4. Polish characters and a second line; Ctrl+V in the box pastes text, never a page.
    editor->setFocus();
    typeText(editor, QString::fromUtf8("\nZażółć gęślą jaźń"));
    const int pages = e.ws.document()->count();
    QApplication::clipboard()->setText(QStringLiteral(" OK"));
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    EXPECT_EQ(e.ws.document()->count(), pages);
    // 5. Move and resize while typing: the Move tab and the right handle.
    const QRectF before = e.canvas->editingText()->rect;
    drag(e.canvas, e.canvas->editMoveGrip().center(), e.canvas->editMoveGrip().center() + QPointF(60, 40));
    EXPECT_TRUE(editor->isVisible()) << "moving the box ended typing";
    EXPECT_GT(e.canvas->editingText()->rect.left(), before.left());
    EXPECT_GT(e.canvas->editingText()->rect.top(), before.top());
    const QRectF moved = e.canvas->editingText()->rect;
    const QRectF right = e.canvas->editResizeHandles().value(0);
    drag(e.canvas, right.center(), right.center() + QPointF(120, 0));
    EXPECT_GT(e.canvas->editingText()->rect.width(), moved.width());
    EXPECT_NEAR(e.canvas->editingText()->fontSize * 595.0, 24.0, 0.01) << "resizing the box must not scale the font";

    // Escape finishes typing and keeps the text.
    QTest::keyClick(editor, Qt::Key_Escape);
    ASSERT_FALSE(editor->isVisible());
    ASSERT_EQ(e.ws.document()->page(0).annotations.size(), 1);
    Annotation t = e.ws.document()->page(0).annotations[0];
    EXPECT_EQ(t.text, QString::fromUtf8("Hello World\nZażółć gęślą jaźń OK"));
    EXPECT_EQ(t.stroke, QColor(220, 0, 0));
    EXPECT_EQ(t.fontFamily, otherFamily);
    EXPECT_TRUE(t.bold);

    // 6. Select tool: drag the text to a new place; a second click on it edits it.
    e.tool("select");
    const QPointF centre = e.canvas->fromPage(t.rect.center());
    QTest::mouseClick(e.canvas, Qt::LeftButton, {}, centre.toPoint());
    drag(e.canvas, centre, centre + QPointF(-40, 70));
    const Annotation movedT = e.ws.document()->page(0).annotations[0];
    EXPECT_GT(movedT.rect.top(), t.rect.top());
    EXPECT_EQ(movedT.text, t.text) << "a drag must move, not edit";
    EXPECT_FALSE(editor->isVisible());

    // 7. Narrower with the side handle: the text re-wraps, the font size stays, the height grows (Auto size).
    QRectF sel(e.canvas->fromPage(movedT.rect.topLeft()), e.canvas->fromPage(movedT.rect.bottomRight()));
    sel.adjust(-4, -4, 4, 4);
    // Choose a width between a single word and its complete line, using the
    // embedded font's advances in page units. A fixed percentage can leave both paragraphs intact.
    const double wordWidth = std::max(AnnotationRender::textAdvance(movedT, "Hello"),
                                      AnnotationRender::textAdvance(movedT, "World"));
    const double lineWidth = AnnotationRender::textAdvance(movedT, "Hello World");
    const double targetWidth = (wordWidth + lineWidth) / 2;
    ASSERT_LE(lineWidth, movedT.rect.width());
    const QPointF grip(sel.right(), sel.center().y());
    const double dx = e.canvas->fromPage(QPointF(targetWidth, 0)).x()
                    - e.canvas->fromPage(QPointF(movedT.rect.width(), 0)).x();
    drag(e.canvas, grip, grip + QPointF(dx, 0));
    const Annotation narrow = e.ws.document()->page(0).annotations[0];
    EXPECT_LT(narrow.rect.width(), movedT.rect.width());
    EXPECT_DOUBLE_EQ(narrow.fontSize, movedT.fontSize);
    ASSERT_TRUE(narrow.autoSize);
    EXPECT_GT(narrow.rect.height(), movedT.rect.height());
    const auto wrapped = AnnotationRender::layoutText(narrow);
    ASSERT_GE(wrapped.size(), 3);
    EXPECT_EQ(wrapped[0].text, QStringLiteral("Hello"));
    EXPECT_EQ(wrapped[1].text, QStringLiteral("World"));
    EXPECT_GT(wrapped.size(), AnnotationRender::layoutText(movedT).size());
    EXPECT_GE(narrow.rect.height(), AnnotationRender::textHeight(narrow) - 1e-9);
    // Bottom handle: a taller box switches Auto size off; it can never become shorter than the text.
    sel = QRectF(e.canvas->fromPage(narrow.rect.topLeft()), e.canvas->fromPage(narrow.rect.bottomRight())).adjusted(-4, -4, 4, 4);
    drag(e.canvas, QPointF(sel.center().x(), sel.bottom()), QPointF(sel.center().x(), sel.bottom() + 80));
    const Annotation tall = e.ws.document()->page(0).annotations[0];
    EXPECT_FALSE(tall.autoSize);
    EXPECT_GT(tall.rect.height(), narrow.rect.height());
    sel = QRectF(e.canvas->fromPage(tall.rect.topLeft()), e.canvas->fromPage(tall.rect.bottomRight())).adjusted(-4, -4, 4, 4);
    drag(e.canvas, QPointF(sel.center().x(), sel.bottom()), QPointF(sel.center().x(), sel.top() + 2));
    const Annotation shortest = e.ws.document()->page(0).annotations[0];
    EXPECT_GE(shortest.rect.height(), AnnotationRender::textHeight(shortest) - 1e-9) << "text must not be clipped";
    shot(&e.ws, "text-resized");

    // Ctrl+C / Ctrl+V of the selected text box: a copy next to it, the text also on the clipboard.
    // (X servers without a window manager do not give activation back after the colour dialog.)
    e.ws.activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(&e.ws, 3000));
    e.canvas->setFocus();
    QTest::keyClick(e.canvas, Qt::Key_C, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(QApplication::clipboard()->text(), shortest.text, 3000);
    QTest::keyClick(e.canvas, Qt::Key_V, Qt::ControlModifier);
    ASSERT_EQ(e.ws.document()->page(0).annotations.size(), 2);
    EXPECT_EQ(e.ws.document()->page(0).annotations[1].text, shortest.text);
    EXPECT_NE(e.ws.document()->page(0).annotations[1].id, shortest.id);
    e.canvas->setFocus();
    QTest::keyClick(e.canvas, Qt::Key_Z, Qt::ControlModifier); // remove the pasted copy again
    ASSERT_EQ(e.ws.document()->page(0).annotations.size(), 1);

    // 8. Undo / redo of the resize.
    e.canvas->setFocus();
    QTest::keyClick(e.canvas, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(e.ws.document()->page(0).annotations[0].rect, tall.rect);
    QTest::keyClick(e.canvas, Qt::Key_Y, Qt::ControlModifier);
    EXPECT_EQ(e.ws.document()->page(0).annotations[0].rect, shortest.rect);

    // 9. Project round trip keeps everything editable.
    const QString project = dir.filePath("text.folvio");
    QString err;
    ASSERT_TRUE(CombineSession::saveProject(*e.ws.document(), project, false, &err)) << err.toStdString();
    CombineDocument reopened;
    QStringList warnings;
    ASSERT_TRUE(CombineSession::openProject(reopened, project, &warnings, &err));
    EXPECT_EQ(reopened.page(0).annotations[0].toJson(), shortest.toJson());

    // 10. PDF: real text with Polish characters, red glyphs, and no box or frame around them.
    ExportSettings s = e.ws.document()->exportSettings();
    s.pageSize = PageSizeMode::Auto;
    s.margins = MarginMode::None;
    e.ws.document()->setExportSettings(s);
    const QString pdfPath = dir.filePath("text.pdf");
    QSignalSpy exported(&e.ws, &CombineWorkspace::exportFinished);
    e.ws.exportToPath(pdfPath, false);
    ASSERT_TRUE(exported.wait(60000));
    ASSERT_TRUE(exported.first().first().toBool()) << e.ws.lastExportError().toStdString();
    auto pdf = PdfDocument::openFile(pdfPath);
    ASSERT_TRUE(pdf);
    const QString text = pdf->pageText(0);
    EXPECT_TRUE(text.contains("Hello")) << text.toStdString();
    EXPECT_TRUE(text.contains(QString::fromUtf8("Zażółć"))) << text.toStdString();
    const QImage page = pdf->render(0, 1.0);
    const QRectF tb(shortest.rect.left() * page.width(), shortest.rect.top() * page.width(), shortest.rect.width() * page.width(),
                    shortest.rect.height() * page.width());
    int red = 0, white = 0, total = 0;
    for (int y = int(tb.top()); y < int(tb.bottom()); y += 2)
        for (int x = int(tb.left()); x < int(tb.right()); x += 2) {
            if (x < 0 || y < 0 || x >= page.width() || y >= page.height())
                continue;
            const QColor c(page.pixel(x, y));
            red += near(c, QColor(220, 0, 0), 70) ? 1 : 0;
            white += near(c, Qt::white, 20) ? 1 : 0;
            ++total;
        }
    EXPECT_GT(red, 10) << "no red text in the PDF";
    EXPECT_EQ(white, 0) << "a white box was exported behind the text";
    // The edges of the box are photo colour: no selection frame in the PDF.
    EXPECT_TRUE(near(QColor(page.pixel(int(tb.left()) + 1, int(tb.bottom()) - 1)), photo, 30));
}
