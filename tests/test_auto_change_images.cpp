// Auto Change and images: clipboard priorities, screenshots pasted into a PDF as image objects
// (select, move, resize, crop, delete, undo, save with verification), and the image editor
// (screenshot without a PDF: annotate, export image, convert to PDF).
#include "TestSupport.h"

#include "combine/CombineModel.h"
#include "combine/CombineWorkspace.h"
#include "combine/PageCanvas.h"
#include "core/files/ClipboardContent.h"
#include "core/persistence/Database.h"
#include "editor/pdf_native/PdfCanvas.h"
#include "editor/pdf_native/PdfEditSession.h"
#include "editor/pdf_native/PdfWorkspace.h"
#include "engines/pdf/PdfDocument.h"
#include "ui/MainWindow.h"

#include <QAbstractButton>
#include <QBuffer>
#include <QFileInfo>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <fpdf_edit.h>
#include <fpdfview.h>

#include <gtest/gtest.h>

using namespace folvio;
using namespace folvio::test;

namespace {

const QString kFixture = QStringLiteral(FOLVIO_FIXTURES "/two_column_cv.pdf");

// A recognisable "screenshot": orange frame, blue left half, green right half.
QImage screenshot(int w = 640, int h = 360)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(QColor(255, 140, 0));
    QPainter p(&img);
    p.fillRect(QRect(8, 8, w / 2 - 8, h - 16), QColor(30, 90, 220));
    p.fillRect(QRect(w / 2, 8, w / 2 - 8, h - 16), QColor(20, 170, 70));
    return img;
}

bool similar(const QColor& a, const QColor& b, int tol = 45)
{
    return std::abs(a.red() - b.red()) < tol && std::abs(a.green() - b.green()) < tol && std::abs(a.blue() - b.blue()) < tol;
}

int imageObjects(const QString& pdfPath, int page)
{
    auto doc = PdfDocument::openFile(pdfPath);
    if (!doc)
        return -1;
    std::lock_guard lock(pdfiumMutex());
    FPDF_PAGE pg = FPDF_LoadPage(doc->handle(), page);
    int n = 0;
    for (int i = 0, c = FPDFPage_CountObjects(pg); i < c; ++i)
        n += FPDFPageObj_GetType(FPDFPage_GetObject(pg, i)) == FPDF_PAGEOBJ_IMAGE ? 1 : 0;
    FPDF_ClosePage(pg);
    return n;
}

QColor pdfPixel(const QString& path, int page, const QPointF& pdfPoint, double scale = 2.0)
{
    auto doc = PdfDocument::openFile(path);
    const QImage img = doc->render(page, scale);
    const QPointF px = doc->pageToDevice(page, img.size()).map(pdfPoint);
    return QColor(img.pixel(int(px.x()), int(px.y())));
}

void drag(QWidget* w, QPointF from, QPointF to, Qt::KeyboardModifiers mods = {})
{
    QTest::mousePress(w, Qt::LeftButton, mods, from.toPoint());
    for (int i = 1; i <= 6; ++i) {
        QMouseEvent move(QEvent::MouseMove, from + (to - from) * (i / 6.0), w->mapToGlobal(from + (to - from) * (i / 6.0)),
                         Qt::NoButton, Qt::LeftButton, mods);
        QApplication::sendEvent(w, &move);
    }
    QTest::mouseRelease(w, Qt::LeftButton, mods, to.toPoint());
}

struct PdfFixture {
    QTemporaryDir dir;
    QString pdf;
    PdfWorkspace ws;
    PdfCanvas* canvas = nullptr;
    explicit PdfFixture(const QString& source = kFixture)
    {
        Database::instance().open();
        Database::instance().setSetting("save.confirmFirst", false);
        pdf = dir.filePath(QString::fromUtf8("dokument ąę.pdf"));
        QFile::copy(source, pdf);
        QFile::setPermissions(pdf, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
        ws.resize(1300, 900);
        ws.show();
        ws.activateWindow();
        EXPECT_TRUE(QTest::qWaitForWindowActive(&ws, 3000));
        QString err;
        EXPECT_TRUE(ws.openFile(pdf, &err)) << err.toStdString();
        QTest::qWait(200);
        canvas = ws.canvas();
    }
};

} // namespace

// ===========================================================================
// Clipboard priorities

TEST(ClipboardContent, PrefersImageFilesThenBitmapsThenTextAndIgnoresPicturesOfText)
{
    QTemporaryDir dir;
    const QString png = dir.filePath("shot.png");
    screenshot().save(png);

    // Explorer "Copy" of a PNG: the file (full quality), even with a thumbnail bitmap.
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(png)});
    files.setImageData(QImage(16, 16, QImage::Format_RGB32));
    ClipboardContent c = ClipboardContent::read(&files);
    EXPECT_EQ(c.kind, ClipboardContent::Kind::ImageFiles);
    EXPECT_EQ(c.loadImage().size(), QSize(640, 360));

    // Win+Shift+S: only a bitmap.
    QMimeData shot;
    shot.setImageData(screenshot());
    c = ClipboardContent::read(&shot);
    EXPECT_EQ(c.kind, ClipboardContent::Kind::Image);
    EXPECT_EQ(c.image.size(), QSize(640, 360));

    // Browser "Copy image": bitmap + HTML with <img>: the image.
    QMimeData browser;
    browser.setImageData(screenshot());
    browser.setHtml("<img src=\"https://example.invalid/a.png\">");
    browser.setText("https://example.invalid/a.png");
    EXPECT_EQ(ClipboardContent::read(&browser).kind, ClipboardContent::Kind::Image);

    // Word/LibreOffice text: text + HTML without <img> + a rendered bitmap: the text.
    QMimeData office;
    office.setImageData(screenshot());
    office.setHtml("<p>Zażółć gęślą jaźń</p>");
    office.setText(QString::fromUtf8("Zażółć gęślą jaźń"));
    c = ClipboardContent::read(&office);
    EXPECT_EQ(c.kind, ClipboardContent::Kind::Text);
    EXPECT_EQ(c.text, QString::fromUtf8("Zażółć gęślą jaźń"));

    // X11/Wayland: encoded image/png only.
    QMimeData raw;
    QByteArray bytes;
    {
        QBuffer b(&bytes);
        b.open(QIODevice::WriteOnly);
        screenshot(100, 50).save(&b, "PNG");
    }
    raw.setData("image/png", bytes);
    c = ClipboardContent::read(&raw);
    EXPECT_EQ(c.kind, ClipboardContent::Kind::Image);
    EXPECT_EQ(c.image.size(), QSize(100, 50));

    // A copied non-image file is not text to paste.
    const QString doc = dir.filePath("a.pdf");
    QFile(doc).open(QIODevice::WriteOnly);
    QMimeData other;
    other.setUrls({QUrl::fromLocalFile(doc)});
    EXPECT_EQ(ClipboardContent::read(&other).kind, ClipboardContent::Kind::None);
}

// ===========================================================================
// Screenshot pasted into an existing PDF

TEST(AutoChangePdfImage, PasteMoveResizeCropDeleteUndoAndSaveKeepEverythingElse)
{
    PdfFixture f;
    PdfEditSession* s = f.ws.session();
    const QString textBefore = s->original()->pageText(0);
    const int imagesBefore = imageObjects(f.pdf, 0);

    // Ctrl+V of a screenshot.
    QApplication::clipboard()->setImage(screenshot());
    // The Windows clipboard changes owner asynchronously.
    QTRY_VERIFY_WITH_TIMEOUT(ClipboardContent::fromClipboard().kind == ClipboardContent::Kind::Image, 5000);
    f.canvas->setFocus();
    QTest::keyClick(f.canvas, Qt::Key_V, Qt::ControlModifier);
    ASSERT_EQ(s->addedImages(0).size(), 1);
    AddedImage img = s->addedImages(0).first();
    EXPECT_EQ(f.ws.selectedImageId(), img.addedId) << "the pasted image must be selected";
    EXPECT_EQ(img.pixelSize, QSize(640, 360));
    EXPECT_NEAR(img.rect.width() / img.rect.height(), 640.0 / 360, 0.01);
    EXPECT_TRUE(f.canvas->hasImageOverlay());
    test::saveScreenshot(&f.ws, "pdf-image-pasted");
    // Visible on the page.
    EXPECT_TRUE(similar(QColor(s->current()->render(0, 1.0).pixel(
                         s->current()->pageToDevice(0, s->current()->render(0, 1.0).size())
                             .map(QPointF(img.rect.left() + img.rect.width() * 0.25, img.rect.center().y()))
                             .toPoint())),
                     QColor(30, 90, 220)));

    // Move by dragging.
    const QRectF w0 = f.canvas->imageOverlayRect();
    drag(f.canvas, w0.center(), w0.center() + QPointF(-60, 80));
    img = s->addedImages(0).first();
    const QRectF moved = f.canvas->pdfRectToWidget(0, img.rect).normalized();
    EXPECT_NEAR(moved.left(), w0.left() - 60, 2.0);
    EXPECT_NEAR(moved.top(), w0.top() + 80, 2.0);
    EXPECT_NEAR(moved.width(), w0.width(), 1.0);

    // Resize from the bottom-right corner: proportions are kept.
    const QRectF w1 = f.canvas->imageOverlayRect();
    drag(f.canvas, w1.bottomRight(), w1.bottomRight() - QPointF(w1.width() / 2, w1.height() / 2));
    img = s->addedImages(0).first();
    const QRectF small = f.canvas->pdfRectToWidget(0, img.rect).normalized();
    EXPECT_NEAR(small.width(), w1.width() / 2, 3.0);
    EXPECT_NEAR(small.width() / small.height(), 640.0 / 360, 0.03);
    EXPECT_NEAR(small.left(), w1.left(), 1.5) << "the opposite corner stays in place";

    // Crop away the right (green) half with the right edge handle.
    f.ws.setImageCropMode(true);
    const QRectF w2 = f.canvas->imageOverlayRect();
    drag(f.canvas, QPointF(w2.right(), w2.center().y()), QPointF(w2.center().x(), w2.center().y()));
    img = s->addedImages(0).first();
    EXPECT_NEAR(img.crop.width(), 0.5, 0.02);
    EXPECT_NEAR(img.crop.left(), 0.0, 1e-9);
    const QRectF cropped = f.canvas->pdfRectToWidget(0, img.rect).normalized();
    EXPECT_NEAR(cropped.left(), w2.left(), 1.5) << "cropping must not move the visible part";
    EXPECT_NEAR(cropped.width(), w2.width() / 2, 2.0);
    // Only blue is visible now; the green half is gone from the page but kept in the edit.
    {
        const QImage r = s->current()->render(0, 2.0);
        const QTransform t = s->current()->pageToDevice(0, r.size());
        EXPECT_TRUE(similar(QColor(r.pixel(t.map(QPointF(img.rect.center().x(), img.rect.center().y())).toPoint())), QColor(30, 90, 220)));
        EXPECT_EQ(QImage::fromData(img.image).size(), QSize(640, 360)) << "crop must be non-destructive";
    }
    test::saveScreenshot(&f.ws, "pdf-image-crop-mode");
    QTest::keyClick(f.canvas, Qt::Key_Return); // finish cropping
    EXPECT_FALSE(f.canvas->imageCropMode());

    // Undo the crop, redo it.
    f.ws.undo();
    EXPECT_NEAR(s->addedImages(0).first().crop.width(), 1.0, 1e-9);
    f.ws.redo();
    EXPECT_NEAR(s->addedImages(0).first().crop.width(), 0.5, 0.02);

    // Save: original text unchanged, exactly one more image object, verification passed.
    ASSERT_TRUE(f.ws.save());
    EXPECT_EQ(imageObjects(f.pdf, 0), imagesBefore + 1);
    auto saved = PdfDocument::openFile(f.pdf);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->pageText(0).simplified(), textBefore.simplified());
    img = s->addedImages(0).first();
    EXPECT_TRUE(similar(pdfPixel(f.pdf, 0, img.rect.center()), QColor(30, 90, 220)));

    // Delete with the Delete key; undo brings it back.
    f.ws.selectImage(0, img.addedId);
    f.canvas->setFocus();
    QTest::keyClick(f.canvas, Qt::Key_Delete);
    EXPECT_TRUE(s->addedImages(0).isEmpty());
    EXPECT_EQ(f.ws.selectedImageId(), -1);
    f.ws.undo();
    EXPECT_EQ(s->addedImages(0).size(), 1);
    f.ws.redo();
    ASSERT_TRUE(f.ws.save());
    EXPECT_EQ(imageObjects(f.pdf, 0), imagesBefore);
    EXPECT_EQ(PdfDocument::openFile(f.pdf)->pageText(0).simplified(), textBefore.simplified());
}

TEST(AutoChangePdfImage, ExistingImagesStayAndTextPasteMakesATextBox)
{
    QTemporaryDir dir;
    const QString scanned = dir.filePath("scan.pdf");
    ASSERT_TRUE(makeScannedPdf(scanned, QStringLiteral("Original scan")));
    PdfFixture f(scanned);
    const int before = imageObjects(f.pdf, 0);
    ASSERT_GE(before, 1);

    // Image from a file copied in the file manager.
    const QString png = dir.filePath("photo.png");
    screenshot(300, 200).save(png);
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(png)});
    QApplication::clipboard()->setMimeData(mime);
    QTRY_VERIFY_WITH_TIMEOUT(ClipboardContent::fromClipboard().kind == ClipboardContent::Kind::ImageFiles, 5000);
    ASSERT_TRUE(f.ws.pasteFromClipboard());
    ASSERT_EQ(f.ws.session()->addedImages(0).size(), 1);
    EXPECT_EQ(f.ws.session()->addedImages(0).first().pixelSize, QSize(300, 200));

    // Text: a new text box with the pasted text, nothing else changes.
    QApplication::clipboard()->setText(QString::fromUtf8("Notatka: źdźbło"));
    // The Windows clipboard changes owner asynchronously.
    QTRY_VERIFY_WITH_TIMEOUT(ClipboardContent::fromClipboard().kind == ClipboardContent::Kind::Text, 3000);
    ASSERT_TRUE(f.ws.pasteFromClipboard());
    auto* editor = f.ws.findChild<QPlainTextEdit*>();
    ASSERT_TRUE(editor && editor->isVisible());
    EXPECT_EQ(editor->toPlainText(), QString::fromUtf8("Notatka: źdźbło"));
    QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
    ASSERT_TRUE(f.ws.save());
    EXPECT_EQ(imageObjects(f.pdf, 0), before + 1) << "the scan image must not be removed";
    EXPECT_TRUE(PdfDocument::openFile(f.pdf)->pageText(0).contains(QString::fromUtf8("źdźbło")));
}

// ===========================================================================
// Screenshot without a PDF: the image editor

TEST(AutoChangeImageEditor, ScreenshotPasteAnnotateExportImageAndPdf)
{
    Database::instance().open();
    QTemporaryDir dir;
    MainWindow w;
    w.resize(1400, 900);
    w.show();
    w.activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(&w, 3000));
    w.showPage(MainWindow::AutoChange);
    QApplication::clipboard()->setImage(screenshot());
    QTRY_VERIFY_WITH_TIMEOUT(ClipboardContent::fromClipboard().kind == ClipboardContent::Kind::Image, 5000);
    auto* page = w.findChild<WorkspacePage*>();
    ASSERT_TRUE(page);
    QTest::keyClick(&w, Qt::Key_V, Qt::ControlModifier);
    combine::CombineWorkspace* ed = page->image();
    QTRY_VERIFY_WITH_TIMEOUT(ed->isVisible() && ed->hasImage(), 5000);
    EXPECT_EQ(ed->step(), combine::CombineWorkspace::Step::Edit);
    EXPECT_FALSE(ed->document()->isModified());
    EXPECT_EQ(ed->document()->page(0).sourceSize, QSize(640, 360));
    // The editor's tools are those of Combine; Arrange is hidden.
    EXPECT_FALSE(ed->findChild<QAbstractButton*>("step-0")->isVisible());
    for (const char* name : {"image-recognize", "image-export", "image-close", "tool-text", "tool-crop", "tool-pen"})
        EXPECT_TRUE(ed->findChild<QAbstractButton*>(name) && ed->findChild<QAbstractButton*>(name)->isVisible()) << name;

    test::saveScreenshot(ed, "image-editor-opened");
    // Annotate: a red pen stroke over the blue half.
    ed->setTool(combine::PageCanvas::Tool::Pen);
    combine::PageCanvas* c = ed->canvas();
    combine::ToolStyle st = c->style();
    st.stroke = QColor(230, 0, 0);
    st.width = 0.012;
    c->setStyle(st);
    drag(c, c->fromPage(QPointF(0.1, 0.2)), c->fromPage(QPointF(0.3, 0.2)));
    ASSERT_EQ(ed->document()->page(0).annotations.size(), 1);
    EXPECT_TRUE(ed->document()->isModified());

    // Export Image: the annotation is burnt in, the original screenshot is unchanged.
    const QString out = dir.filePath("edited.png");
    QString err;
    ASSERT_TRUE(ed->exportImageTo(out, &err)) << err.toStdString();
    const QImage exported(out);
    ASSERT_EQ(exported.size(), QSize(640, 360));
    EXPECT_TRUE(similar(QColor(exported.pixel(128, 128)), QColor(230, 0, 0), 60));
    EXPECT_TRUE(similar(QColor(exported.pixel(160, 300)), QColor(30, 90, 220)));
    EXPECT_TRUE(similar(QColor(exported.pixel(500, 180)), QColor(20, 170, 70)));
    EXPECT_TRUE(similar(QColor(QImage(ed->document()->page(0).sourcePath).pixel(128, 128)), QColor(30, 90, 220)))
        << "the source image must stay unchanged";
    const QString jpg = dir.filePath("edited.jpg");
    ASSERT_TRUE(ed->exportImageTo(jpg, &err)) << err.toStdString();
    EXPECT_FALSE(QImage(jpg).isNull());

    // Convert to PDF through the shared exporter.
    const QString pdf = dir.filePath("screenshot.pdf");
    QSignalSpy done(ed, &combine::CombineWorkspace::exportFinished);
    ed->exportToPath(pdf, false);
    ASSERT_TRUE(done.wait(60000));
    ASSERT_TRUE(done.first().first().toBool()) << ed->lastExportError().toStdString();
    EXPECT_EQ(PdfDocument::openFile(pdf)->pageCount(), 1);

    // The Combine workspace and its session are untouched by the image editor.
    EXPECT_TRUE(w.combineWorkspace()->document()->isEmpty());

    // Closing without asking (exported state is not marked here): the start page comes back.
    ed->document()->markSaved();
    QTest::mouseClick(ed->findChild<QAbstractButton*>("image-close"), Qt::LeftButton);
    EXPECT_FALSE(ed->hasImage());
    EXPECT_FALSE(page->hasDocument());

    // Opening an image file from the Auto Change drop zone path.
    const QString png = dir.filePath("photo.png");
    screenshot(320, 200).save(png);
    ASSERT_TRUE(page->openDocument(png));
    QTRY_VERIFY_WITH_TIMEOUT(ed->hasImage(), 10000);
    EXPECT_EQ(ed->document()->page(0).sourcePath, QFileInfo(png).absoluteFilePath());
    EXPECT_FALSE(ed->document()->isModified());
    ed->closeImage();
}
