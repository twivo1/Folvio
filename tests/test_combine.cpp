// COMBINE module: import, arrange, perspective correction, crop, annotations, PDF export,
// session persistence and a full end-to-end run through the Qt GUI.
#include "TestSupport.h"

#include "combine/AnnotationRender.h"
#include "combine/CombineModel.h"
#include "combine/CombinePdfExporter.h"
#include "combine/CombineSession.h"
#include "combine/CombineWorkspace.h"
#include "combine/ImageGeometry.h"
#include "combine/PageCanvas.h"
#include "engines/pdf/PdfDocument.h"
#include "ui/widgets/CommonWidgets.h"

#include <QApplication>
#include <QBuffer>
#include <QButtonGroup>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QKeyEvent>
#include <QItemSelectionModel>
#include <QListView>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUrl>

#include <fpdf_edit.h>
#include <fpdfview.h>

#include <gtest/gtest.h>

#include <numbers>

using namespace folvio;
using namespace folvio::combine;
using namespace folvio::test;

namespace {

const QColor kPageColors[] = {QColor("#d32f2f"), QColor("#1976d2"), QColor("#388e3c"), QColor("#f57c00"),
                              QColor("#7b1fa2"), QColor("#00838f"), QColor("#5d4037"), QColor("#c2185b"),
                              QColor("#455a64"), QColor("#afb42b")};

// A "document": white page, a coloured identification block, text lines and black corner marks.
QImage makeDocument(int index, QSize size = QSize(840, 1188))
{
    QImage img(size, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const int m = size.width() / 14;
    p.fillRect(QRect(m, m, m * 3, m * 2), kPageColors[index % 10]);
    QFont f("Arial");
    f.setPixelSize(size.width() / 28);
    p.setFont(f);
    p.setPen(Qt::black);
    for (int i = 0; i < 14; ++i)
        p.drawText(m, m * 4 + i * size.width() / 18, QStringLiteral("Line %1 of document %2 - test text").arg(i + 1).arg(index + 1));
    // Corner marks (8 px from each corner, 40 px squares) to verify geometry after rectification.
    const int s = size.width() / 21;
    for (const QPoint& c : {QPoint(8, 8), QPoint(size.width() - 8 - s, 8), QPoint(size.width() - 8 - s, size.height() - 8 - s),
                            QPoint(8, size.height() - 8 - s)})
        p.fillRect(QRect(c, QSize(s, s)), Qt::black);
    return img;
}

// Photograph the document: project it into 'quad' on a dark background.
QImage photograph(const QImage& doc, const Quad& quad, QSize canvas = QSize(1600, 1200))
{
    QImage photo(canvas, QImage::Format_RGB32);
    photo.fill(QColor(70, 55, 45));
    QTransform t;
    const QPolygonF from({QPointF(0, 0), QPointF(doc.width(), 0), QPointF(doc.width(), doc.height()), QPointF(0, doc.height())});
    const QPolygonF to({quad.pts[0], quad.pts[1], quad.pts[2], quad.pts[3]});
    EXPECT_TRUE(QTransform::quadToQuad(from, to, t));
    QPainter p(&photo);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    p.setTransform(t);
    p.drawImage(0, 0, doc);
    return photo;
}

Quad quadOf(std::initializer_list<QPointF> pts)
{
    Quad q;
    int i = 0;
    for (const QPointF& p : pts)
        q.pts[size_t(i++)] = p;
    return q;
}

// Corners of a w x h document photographed by a pinhole camera (focal length f px, principal point at
// the canvas centre) after rotating the page by rz (in plane), rx (tilt) and ry (pan), at distance d.
Quad cameraQuad(double rx, double ry, double rz, double d, double f, QSize canvas = QSize(1600, 1200), QSizeF doc = QSizeF(210, 297))
{
    auto rad = [](double a) { return a * std::numbers::pi / 180; };
    const double cx = std::cos(rad(rx)), sx = std::sin(rad(rx)), cy = std::cos(rad(ry)), sy = std::sin(rad(ry)),
                 cz = std::cos(rad(rz)), sz = std::sin(rad(rz));
    Quad q;
    const QPointF corners[] = {{-doc.width() / 2, -doc.height() / 2}, {doc.width() / 2, -doc.height() / 2},
                               {doc.width() / 2, doc.height() / 2}, {-doc.width() / 2, doc.height() / 2}};
    for (int i = 0; i < 4; ++i) {
        // Rz
        double x = corners[i].x() * cz - corners[i].y() * sz, y = corners[i].x() * sz + corners[i].y() * cz, z = 0;
        // Rx
        double y2 = y * cx - z * sx, z2 = y * sx + z * cx;
        y = y2;
        z = z2;
        // Ry
        double x2 = x * cy + z * sy;
        z2 = -x * sy + z * cy;
        x = x2;
        z = z2 + d;
        q.pts[size_t(i)] = QPointF(f * x / z + canvas.width() / 2.0, f * y / z + canvas.height() / 2.0);
    }
    return q;
}

double meanAbsDiff(const QImage& a, const QImage& b, int border)
{
    const QImage x = a.convertToFormat(QImage::Format_Grayscale8), y = b.convertToFormat(QImage::Format_Grayscale8);
    if (x.size() != y.size())
        return 255;
    double sum = 0;
    qint64 n = 0;
    for (int j = border; j < x.height() - border; ++j) {
        const uchar* ra = x.constScanLine(j);
        const uchar* rb = y.constScanLine(j);
        for (int i = border; i < x.width() - border; ++i) {
            sum += std::abs(int(ra[i]) - int(rb[i]));
            ++n;
        }
    }
    return n ? sum / n : 255;
}

QString savePng(const QTemporaryDir& dir, const QString& name, const QImage& img)
{
    const QString path = dir.filePath(name);
    img.save(path, "PNG");
    return path;
}

CombinePage pageFor(const QString& path)
{
    ImportResult r = CombineSession::importFiles({path});
    EXPECT_EQ(r.pages.size(), 1) << r.errors.join("; ").toStdString();
    return r.pages.value(0);
}

QColor dominantColorAt(const QImage& img, const QPoint& p)
{
    return QColor(img.pixel(p));
}

bool closeColor(const QColor& a, const QColor& b, int tol = 40)
{
    return std::abs(a.red() - b.red()) < tol && std::abs(a.green() - b.green()) < tol && std::abs(a.blue() - b.blue()) < tol;
}

// Types Unicode text (Polish letters) as real key events; QTest::keyClicks only handles ASCII.
void typeText(QWidget* w, const QString& text)
{
    for (const QChar ch : text) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, QString(ch));
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_unknown, Qt::NoModifier, QString(ch));
        QApplication::sendEvent(w, &press);
        QApplication::sendEvent(w, &release);
    }
}

// Saves and restores the system clipboard around GUI tests (it belongs to the user).
struct ClipboardGuard {
    QMimeData* saved = new QMimeData;
    ClipboardGuard()
    {
        if (const QMimeData* m = QApplication::clipboard()->mimeData())
            for (const QString& f : m->formats())
                saved->setData(f, m->data(f));
    }
    ~ClipboardGuard() { QApplication::clipboard()->setMimeData(saved); }
};

} // namespace

// ===========================================================================
// Import

TEST(CombineImport, PngJpgUnsupportedAndCorrupt)
{
    QTemporaryDir dir;
    const QString png = savePng(dir, "a.png", makeDocument(0, QSize(300, 400)));
    QStringList jpgs;
    for (int i = 0; i < 3; ++i) {
        const QString j = dir.filePath(QStringLiteral("photo_%1.jpg").arg(i));
        makeDocument(i, QSize(300, 400)).save(j, "JPEG", 90);
        jpgs << j;
    }
    const QString txt = dir.filePath("notes.txt");
    QFile t(txt);
    t.open(QIODevice::WriteOnly);
    t.write("not an image");
    t.close();
    const QString broken = dir.filePath("broken.jpg");
    QFile jf(jpgs[0]);
    jf.open(QIODevice::ReadOnly);
    const QByteArray head = jf.read(600);
    QFile b(broken);
    b.open(QIODevice::WriteOnly);
    b.write(head); // truncated JPEG
    b.close();

    ImportResult r = CombineSession::importFiles(QStringList{png} + jpgs + QStringList{txt, broken});
    EXPECT_EQ(r.pages.size(), 4);
    EXPECT_EQ(r.errors.size(), 2) << r.errors.join("; ").toStdString();
    EXPECT_EQ(r.pages[0].sourceSize, QSize(300, 400));
    EXPECT_EQ(r.pages[0].displayName, "a.png");
    EXPECT_FALSE(r.pages[0].sourceSha256.isEmpty());
}

TEST(CombineImport, ExifOrientationIsApplied)
{
    if (!havePython())
        GTEST_SKIP() << "python + Pillow needed to write EXIF";
    QTemporaryDir dir;
    const QString path = dir.filePath("rotated.jpg");
    // 400x300 landscape pixels with EXIF orientation 6 (rotate 90° CW) -> displayed portrait 300x400.
    ASSERT_TRUE(runPython("import sys\nfrom PIL import Image\n"
                          "im = Image.new('RGB', (400, 300), 'white')\n"
                          "im.paste((200, 0, 0), (0, 0, 60, 60))\n"
                          "exif = im.getexif(); exif[274] = 6\n"
                          "im.save(sys.argv[1], 'JPEG', exif=exif.tobytes(), quality=95)\n",
                          {path}));
    const CombinePage p = pageFor(path);
    EXPECT_EQ(p.sourceSize, QSize(300, 400));
    const QImage img = ImageProcessor::loadOriented(path);
    EXPECT_EQ(img.size(), QSize(300, 400));
    // The red block (top-left in raw pixels) moves to the top-right after a 90° CW rotation.
    EXPECT_TRUE(closeColor(img.pixel(img.width() - 20, 20), QColor(200, 0, 0)));
}

TEST(CombineImport, ClipboardBitmapAndFileListsWithoutTouchingTheSystemClipboard)
{
    QTemporaryDir dir;
    // Bitmap (e.g. Win+Shift+S screenshot): stored as PNG in the session, no file needed.
    QMimeData shot;
    QImage screenshot(640, 360, QImage::Format_RGB32);
    screenshot.fill(QColor("#1976d2"));
    shot.setImageData(screenshot);
    ImportResult r = CombineSession::importMime(&shot);
    ASSERT_EQ(r.pages.size(), 1);
    EXPECT_TRUE(r.pages[0].fromClipboard);
    EXPECT_TRUE(QFile::exists(r.pages[0].sourcePath));
    EXPECT_EQ(QImageReader(r.pages[0].sourcePath).size(), QSize(640, 360));
    // Pasting again adds another page.
    EXPECT_EQ(CombineSession::importMime(&shot).pages.size(), 1);
    // Files copied in Explorer arrive as URLs.
    QMimeData files;
    QList<QUrl> urls;
    for (int i = 0; i < 3; ++i)
        urls << QUrl::fromLocalFile(savePng(dir, QStringLiteral("c%1.png").arg(i), makeDocument(i, QSize(200, 260))));
    files.setUrls(urls);
    EXPECT_TRUE(CombineSession::mimeHasImportable(&files));
    EXPECT_EQ(CombineSession::importMime(&files).pages.size(), 3);
    QMimeData text;
    text.setText("hello");
    EXPECT_FALSE(CombineSession::mimeHasImportable(&text));
}

// ===========================================================================
// Arrange

TEST(CombineArrange, ReorderRemoveDuplicateKeepEdits)
{
    QTemporaryDir dir;
    CombineDocument doc;
    QList<CombinePage> pages;
    for (int i = 0; i < 5; ++i) {
        CombinePage p;
        p.sourcePath = savePng(dir, QStringLiteral("p%1.png").arg(i), makeDocument(i, QSize(100, 140)));
        p.displayName = QStringLiteral("image_%1.png").arg(i + 1, 2, 10, QLatin1Char('0'));
        p.sourceSize = QSize(100, 140);
        pages << p;
    }
    // Import order 4,1,3,2,5 like the specification example.
    doc.addPages({pages[3], pages[0], pages[2], pages[1], pages[4]});
    // Annotate the page that is currently first (image_04).
    CombinePage edited = doc.page(0);
    Annotation a;
    a.type = Annotation::Type::Pen;
    a.points = {QPointF(0.1, 0.1), QPointF(0.5, 0.5)};
    edited.annotations << a;
    doc.updatePage(edited, "draw");
    // Reorder to 1,2,3,4,5 by moves.
    doc.movePages({1}, 0); // image_01 to front
    doc.movePages({3}, 1); // image_02 after 01
    doc.movePages({3}, 2); // image_03 after 02
    QStringList names;
    for (const CombinePage& p : doc.pages())
        names << p.displayName;
    EXPECT_EQ(names, QStringList({"image_01.png", "image_02.png", "image_03.png", "image_04.png", "image_05.png"}));
    // The annotation moved with its page.
    EXPECT_EQ(doc.page(3).displayName, "image_04.png");
    EXPECT_EQ(doc.page(3).annotations.size(), 1);
    // Multi-move, duplicate, remove, undo/redo.
    doc.sortByName();
    doc.duplicatePages({3});
    EXPECT_EQ(doc.count(), 6);
    EXPECT_EQ(doc.page(4).displayName, "image_04.png");
    EXPECT_NE(doc.page(4).id, doc.page(3).id);
    EXPECT_EQ(doc.page(4).annotations.size(), 1);
    doc.removePages({0, 5});
    EXPECT_EQ(doc.count(), 4);
    doc.undo();
    EXPECT_EQ(doc.count(), 6);
    doc.redo();
    EXPECT_EQ(doc.count(), 4);
    // Model numbering follows the order.
    PageListModel model(&doc);
    EXPECT_TRUE(model.data(model.index(0), Qt::DisplayRole).toString().startsWith("1. image_02"));
}

// ===========================================================================
// Perspective correction

TEST(CombinePerspective, RectifiesASyntheticPhotographedDocument)
{
    const QImage doc = makeDocument(2);
    const Quad truth = quadOf({QPointF(420, 120), QPointF(1180, 190), QPointF(1290, 1110), QPointF(300, 1050)});
    const QImage photo = photograph(doc, truth);
    PerspectiveSettings s;
    s.aspect = AspectMode::Custom;
    s.customRatio = double(doc.height()) / doc.width();
    QString err;
    const QImage out = ImageProcessor::rectify(photo, truth, s, &err);
    ASSERT_FALSE(out.isNull()) << err.toStdString();
    // Proportions restored exactly; content matches the original document.
    EXPECT_NEAR(double(out.height()) / out.width(), double(doc.height()) / doc.width(), 0.01);
    const QImage ref = doc.scaled(out.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    EXPECT_LT(meanAbsDiff(out, ref, 4), 9.0);
    // Corner marks sit in the corners again (all four), identification block in place.
    const int s4 = out.width() / 21;
    for (const QPoint& c : {QPoint(8 + s4 / 2, 8 + s4 / 2), QPoint(out.width() - 8 - s4 / 2, 8 + s4 / 2),
                            QPoint(out.width() - 8 - s4 / 2, out.height() - 8 - s4 / 2), QPoint(8 + s4 / 2, out.height() - 8 - s4 / 2)})
        EXPECT_LT(qGray(out.pixel(c)), 80) << c.x() << "," << c.y();
    const int m = out.width() / 14;
    EXPECT_TRUE(closeColor(out.pixel(m + m, m + m), kPageColors[2]));
    // It is a real homography, not a crop: the source quad is not a rectangle.
    const QTransform h = Geometry::homography(truth, s);
    EXPECT_GT(std::abs(h.m13()) + std::abs(h.m23()), 1e-6);
}

TEST(CombinePerspective, AutoProportionsStrongPerspectiveRotatedAndMargin)
{
    const QImage doc = makeDocument(1, QSize(840, 1188)); // A4 proportions
    // Real camera geometry: page tilted 40°, panned -15° and rotated ~85° in the photo.
    const Quad truth = cameraQuad(40, -15, 85, 520, 1400);
    const QImage photo = photograph(doc, truth);
    PerspectiveSettings s; // Auto
    QString err;
    const QImage out = ImageProcessor::rectify(photo, truth, s, &err);
    ASSERT_FALSE(out.isNull()) << err.toStdString();
    EXPECT_GT(out.height(), out.width()); // upright portrait page again
    EXPECT_NEAR(double(out.height()) / out.width(), double(doc.height()) / doc.width(), 0.03);
    EXPECT_LT(meanAbsDiff(out, doc.scaled(out.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation), 6), 14.0);
    // Only tilted forwards: the focal length cannot be measured, the typical-lens estimate still
    // lands near the truth (the longest-edge rule would give ~0.8 here).
    const Quad tilted = cameraQuad(50, 0, 0, 700, 1600);
    const QSize ts = Geometry::rectifiedSize(tilted, s, QSizeF(1600, 1200));
    EXPECT_NEAR(double(ts.height()) / ts.width(), 297.0 / 210.0, 0.2);
    // Fixed A4 proportions give the same page.
    s.aspect = AspectMode::A4;
    const QImage a4 = ImageProcessor::rectify(photo, truth, s, &err);
    EXPECT_LT(meanAbsDiff(a4, doc.scaled(a4.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation), 6), 14.0);
    // Safety margin keeps photo content around the page (edges are not cut).
    s.marginPercent = 5;
    const QImage withMargin = ImageProcessor::rectify(photo, truth, s, &err);
    EXPECT_GT(withMargin.width(), a4.width());
    EXPECT_LT(qGray(withMargin.pixel(withMargin.width() / 2, 3)), 120); // dark table visible in the margin
}

TEST(CombinePerspective, InvalidQuadsAreRejected)
{
    const QSize img(1000, 800);
    auto check = [&](const Quad& q) { return Geometry::validate(q, img); };
    EXPECT_TRUE(check(Geometry::defaultQuad(img)).ok);
    // Crossed (TR and BR swapped)
    EXPECT_FALSE(check(quadOf({QPointF(100, 100), QPointF(900, 700), QPointF(900, 100), QPointF(100, 700)})).ok);
    // Mirrored order
    EXPECT_FALSE(check(quadOf({QPointF(100, 100), QPointF(100, 700), QPointF(900, 700), QPointF(900, 100)})).ok);
    // Degenerate (collinear)
    EXPECT_FALSE(check(quadOf({QPointF(100, 100), QPointF(500, 100), QPointF(900, 100), QPointF(500, 102)})).ok);
    // Outside the photo
    EXPECT_FALSE(check(quadOf({QPointF(-300, 100), QPointF(900, 100), QPointF(900, 700), QPointF(100, 700)})).ok);
    // Concave
    EXPECT_FALSE(check(quadOf({QPointF(100, 100), QPointF(900, 100), QPointF(300, 300), QPointF(100, 700)})).ok);
    QString err;
    EXPECT_TRUE(ImageProcessor::rectify(QImage(img, QImage::Format_RGB32),
                                        quadOf({QPointF(100, 100), QPointF(900, 700), QPointF(900, 100), QPointF(100, 700)}), {}, &err)
                    .isNull());
    EXPECT_FALSE(err.isEmpty());
    // Auto ordering repairs shuffled points.
    const Quad fixed = Geometry::orderCorners({QPointF(900, 700), QPointF(100, 100), QPointF(100, 700), QPointF(900, 100)});
    EXPECT_TRUE(check(fixed).ok);
    EXPECT_EQ(fixed.pts[0], QPointF(100, 100));
    EXPECT_EQ(fixed.pts[2], QPointF(900, 700));
}

TEST(CombinePerspective, DetectsThePageOutline)
{
    const QImage doc = makeDocument(3);
    const Quad truth = quadOf({QPointF(400, 150), QPointF(1200, 200), QPointF(1260, 1100), QPointF(330, 1060)});
    const QImage photo = photograph(doc, truth);
    const auto found = ImageProcessor::detectPage(photo);
    ASSERT_TRUE(found.has_value());
    for (int i = 0; i < 4; ++i)
        EXPECT_LT(std::hypot(found->pts[size_t(i)].x() - truth.pts[size_t(i)].x(), found->pts[size_t(i)].y() - truth.pts[size_t(i)].y()), 20.0)
            << "corner " << i;
    // A photo without a page: detection reports failure instead of inventing corners.
    QImage noise(800, 600, QImage::Format_RGB32);
    noise.fill(QColor(90, 90, 90));
    EXPECT_FALSE(ImageProcessor::detectPage(noise).has_value());
}

TEST(CombinePerspective, VeryLargeImage)
{
    const QImage doc = makeDocument(4, QSize(2480, 3508));
    const Quad truth = quadOf({QPointF(1500, 300), QPointF(4400, 450), QPointF(4600, 4100), QPointF(1100, 3900)});
    const QImage photo = photograph(doc, truth, QSize(6000, 4500)); // 27 MP
    QElapsedTimer t;
    t.start();
    QString err;
    const QImage out = ImageProcessor::rectify(photo, truth, {}, &err);
    ASSERT_FALSE(out.isNull()) << err.toStdString();
    EXPECT_GT(out.width(), 2500); // full detail kept (edge length based)
    EXPECT_LT(t.elapsed(), 20000);
}

TEST(CombinePerspective, CropIsSeparateFromPerspectiveAndRotationKeepsCorners)
{
    QTemporaryDir dir;
    const QImage doc = makeDocument(5);
    const Quad truth = quadOf({QPointF(420, 120), QPointF(1180, 190), QPointF(1290, 1110), QPointF(300, 1050)});
    CombinePage page = pageFor(savePng(dir, "photo.png", photograph(doc, truth)));
    page.quad = truth;
    page.perspective.aspect = AspectMode::A4;
    const QImage rect = ImageProcessor::render(page, ImageProcessor::Stage::Final, 0);
    page.crop = QRectF(0.25, 0.25, 0.5, 0.5);
    const QImage cropped = ImageProcessor::render(page, ImageProcessor::Stage::Final, 0);
    EXPECT_NEAR(cropped.width(), rect.width() / 2, 2);
    EXPECT_NEAR(cropped.height(), rect.height() / 2, 2);
    // Rotating a straightened page keeps the corners on the same points of the photo and turns
    // the result by 90° (the corner labels follow the new orientation).
    const QSize before = Geometry::rotatedSize(page.sourceSize, 0);
    CombinePage rotated = page;
    rotated.crop.reset();
    rotated.rotation = 90;
    rotated.quad = Geometry::rotated(truth, before, 90);
    const QImage r2 = ImageProcessor::render(rotated, ImageProcessor::Stage::Final, 0);
    const QImage expected = rect.transformed(QTransform().rotate(90));
    ASSERT_GT(r2.width(), r2.height()); // landscape now
    EXPECT_LT(meanAbsDiff(r2.scaled(expected.size()), expected, 4), 12.0);
}

// ===========================================================================
// Annotations

TEST(CombineAnnotations, LineHighlighterArrowAndEraser)
{
    QImage img(1000, 1000, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.fillRect(QRect(400, 480, 200, 40), Qt::black); // "text" under the highlighter
    p.scale(1000, 1000);
    Annotation line;
    line.type = Annotation::Type::Line;
    line.points = {QPointF(0.1, 0.1), QPointF(0.9, 0.1)};
    line.stroke = QColor(0, 0, 255);
    line.width = 0.006;
    AnnotationRender::paint(p, line);
    Annotation hl;
    hl.type = Annotation::Type::Highlighter;
    hl.points = {QPointF(0.35, 0.5), QPointF(0.65, 0.5)};
    hl.stroke = QColor(255, 220, 0);
    hl.width = 0.06;
    hl.opacity = 0.4;
    AnnotationRender::paint(p, hl);
    Annotation arrow;
    arrow.type = Annotation::Type::Arrow;
    arrow.points = {QPointF(0.2, 0.8), QPointF(0.8, 0.8)};
    arrow.stroke = Qt::red;
    arrow.width = 0.005;
    AnnotationRender::paint(p, arrow);
    p.end();
    EXPECT_TRUE(closeColor(img.pixel(500, 100), Qt::blue));
    // Highlighter: transparent - the black text stays dark, white turns light yellow.
    EXPECT_LT(qGray(img.pixel(500, 500)), 90);
    const QColor yellowish(img.pixel(370, 500));
    EXPECT_GT(yellowish.red(), 200);
    EXPECT_LT(yellowish.blue(), 200);
    EXPECT_GT(yellowish.blue(), 100); // not opaque
    EXPECT_TRUE(closeColor(img.pixel(795, 800), Qt::red)); // arrow head near the tip
    EXPECT_FALSE(AnnotationRender::arrowHead(arrow).isEmpty());
    // Eraser cuts a pen stroke in two and removes whole shapes.
    Annotation pen;
    pen.type = Annotation::Type::Pen;
    for (int i = 0; i <= 20; ++i)
        pen.points << QPointF(0.1 + i * 0.04, 0.3);
    bool changed = false;
    const QList<Annotation> parts = AnnotationRender::erase(pen, QPointF(0.5, 0.3), 0.03, &changed);
    EXPECT_TRUE(changed);
    EXPECT_EQ(parts.size(), 2);
    EXPECT_TRUE(AnnotationRender::erase(arrow, QPointF(0.5, 0.8), 0.01, &changed).isEmpty());
    // Hit testing and resize mapping.
    EXPECT_TRUE(AnnotationRender::hitTest(line, QPointF(0.5, 0.101), 0.002));
    Annotation r;
    r.type = Annotation::Type::Rect;
    r.rect = QRectF(0.1, 0.1, 0.2, 0.2);
    r.mapRect(r.rect, QRectF(0.1, 0.1, 0.4, 0.2));
    EXPECT_NEAR(r.rect.width(), 0.4, 1e-9);
}

TEST(CombineAnnotations, CanvasDrawTextUndoZoomAndPageSwitch)
{
    QTemporaryDir dir;
    CombineDocument doc;
    QList<CombinePage> pages;
    for (int i = 0; i < 2; ++i)
        pages << pageFor(savePng(dir, QStringLiteral("p%1.png").arg(i), makeDocument(i, QSize(600, 800))));
    doc.addPages(pages);
    PageCanvas canvas;
    canvas.setDocument(&doc);
    canvas.resize(900, 1000);
    canvas.show();
    canvas.setPage(doc.page(0).id);
    canvas.fitToView(QSize(900, 1000));
    QTest::qWait(50);

    // Pen stroke with the mouse.
    canvas.setTool(PageCanvas::Tool::Pen);
    const QPoint a = canvas.fromPage(QPointF(0.2, 0.2)).toPoint(), b = canvas.fromPage(QPointF(0.6, 0.3)).toPoint();
    QTest::mousePress(&canvas, Qt::LeftButton, {}, a);
    for (int i = 1; i <= 10; ++i)
        QTest::mouseMove(&canvas, a + (b - a) * i / 10);
    QTest::mouseRelease(&canvas, Qt::LeftButton, {}, b);
    ASSERT_EQ(doc.page(0).annotations.size(), 1);
    EXPECT_EQ(doc.page(0).annotations[0].type, Annotation::Type::Pen);
    EXPECT_GE(doc.page(0).annotations[0].points.size(), 5);

    // Text box: click, type (Polish), Ctrl+Enter; Ctrl+V while editing pastes text, not an image.
    canvas.setTool(PageCanvas::Tool::Text);
    QTest::mouseClick(&canvas, Qt::LeftButton, {}, canvas.fromPage(QPointF(0.1, 0.5)).toPoint());
    auto* editor = canvas.findChild<CombineTextEditor*>();
    ASSERT_TRUE(editor && editor->isVisible());
    typeText(editor, QString::fromUtf8("Zażółć gęślą jaźń"));
    QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
    ASSERT_EQ(doc.page(0).annotations.size(), 2);
    EXPECT_EQ(doc.page(0).annotations[1].text, QString::fromUtf8("Zażółć gęślą jaźń"));
    // Edit the existing text.
    QTest::mouseClick(&canvas, Qt::LeftButton, {}, canvas.fromPage(doc.page(0).annotations[1].rect.center()).toPoint());
    ASSERT_TRUE(editor->isVisible());
    QTest::keyClick(editor, Qt::Key_End, Qt::ControlModifier);
    typeText(editor, " 2");
    QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(doc.page(0).annotations[1].text, QString::fromUtf8("Zażółć gęślą jaźń 2"));

    // Zoom does not change stored coordinates; mapping round-trips.
    const QList<QPointF> before = doc.page(0).annotations[0].points;
    canvas.setZoom(canvas.zoom() * 2.3);
    EXPECT_EQ(doc.page(0).annotations[0].points, before);
    const QPointF u(0.37, 0.61);
    EXPECT_LT(QLineF(canvas.toPage(canvas.fromPage(u)), u).length(), 1e-9);

    // Move with the select tool, then undo/redo.
    canvas.setZoom(canvas.zoom() / 2.3);
    canvas.setTool(PageCanvas::Tool::Select);
    const QPointF onStroke = canvas.fromPage(before[before.size() / 2]);
    QTest::mousePress(&canvas, Qt::LeftButton, {}, onStroke.toPoint());
    QTest::mouseMove(&canvas, (onStroke + QPointF(40, 40)).toPoint());
    QTest::mouseRelease(&canvas, Qt::LeftButton, {}, (onStroke + QPointF(40, 40)).toPoint());
    EXPECT_NE(doc.page(0).annotations[0].points, before);
    doc.undo();
    EXPECT_EQ(doc.page(0).annotations[0].points, before);
    doc.redo();
    EXPECT_NE(doc.page(0).annotations[0].points, before);

    // Switching pages keeps everything.
    canvas.setPage(doc.page(1).id);
    EXPECT_TRUE(doc.page(1).annotations.isEmpty());
    canvas.setPage(doc.page(0).id);
    EXPECT_EQ(doc.page(0).annotations.size(), 2);
}

// ===========================================================================
// PDF export

namespace {
QList<CombinePage> numberedPages(const QTemporaryDir& dir, int n, bool landscapeOdd = false)
{
    QList<CombinePage> pages;
    for (int i = 0; i < n; ++i) {
        QSize s = (landscapeOdd && i % 2) ? QSize(800, 560) : QSize(560, 800);
        CombinePage p = pageFor(savePng(dir, QStringLiteral("n%1.png").arg(i), makeDocument(i, s)));
        Annotation t;
        t.type = Annotation::Type::Text;
        t.text = QStringLiteral("PAGE %1").arg(i + 1);
        t.rect = QRectF(0.1, 0.9, 0.6, 0.05);
        p.annotations << t;
        pages << p;
    }
    return pages;
}
} // namespace

TEST(CombineExport, OneAndTenPagesInOrderWithSizesAndOrientation)
{
    QTemporaryDir dir;
    ExportSettings s;
    s.pageSize = PageSizeMode::A4;
    s.orientation = OrientationMode::Auto;
    s.margins = MarginMode::Small;
    // 1 image -> 1 page
    const QList<CombinePage> one = numberedPages(dir, 1);
    ExportResult r = CombinePdfExporter::exportTo(dir.filePath("one.pdf"), one, s, false, {}, nullptr);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(PdfDocument::openFile(dir.filePath("one.pdf"))->pageCount(), 1);
    // 10 images, alternating portrait/landscape -> 10 pages in the same order.
    const QList<CombinePage> ten = numberedPages(dir, 10, true);
    r = CombinePdfExporter::exportTo(dir.filePath("ten.pdf"), ten, s, false, {}, nullptr);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    auto pdf = PdfDocument::openFile(dir.filePath("ten.pdf"));
    ASSERT_TRUE(pdf);
    ASSERT_EQ(pdf->pageCount(), 10);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(pdf->pageText(i).contains(QStringLiteral("PAGE %1").arg(i + 1))) << i;
        const QSizeF size = pdf->pageSize(i);
        if (i % 2) {
            EXPECT_NEAR(size.width(), 841.9, 1.0) << "landscape A4";
            EXPECT_NEAR(size.height(), 595.3, 1.0);
        } else {
            EXPECT_NEAR(size.width(), 595.3, 1.0) << "portrait A4";
        }
        // The page's identification colour block is where the document has it.
        const QImage page = pdf->render(i, 1.0);
        const QRectF ir = CombinePdfExporter::plan(ten[i], s).imageRect;
        const int m = int(ir.width() / 14);
        const QPoint probe(int(ir.left()) + 2 * m, int(ir.top()) + 2 * m);
        EXPECT_TRUE(closeColor(page.pixel(probe), kPageColors[i])) << "page " << i;
    }
    // Forced portrait; Auto (original size) follows the image.
    s.orientation = OrientationMode::Portrait;
    EXPECT_LT(CombinePdfExporter::plan(ten[1], s).pageSize.width(), CombinePdfExporter::plan(ten[1], s).pageSize.height());
    s.pageSize = PageSizeMode::Auto;
    const QSizeF autoSize = CombinePdfExporter::plan(ten[1], s).pageSize;
    EXPECT_GT(autoSize.width(), autoSize.height());
    // Refuses to overwrite without permission.
    s.pageSize = PageSizeMode::A4;
    EXPECT_FALSE(CombinePdfExporter::exportTo(dir.filePath("one.pdf"), one, s, false, {}, nullptr).ok);
}

TEST(CombineExport, PerspectiveAnnotationsVectorAndPolishText)
{
    QTemporaryDir dir;
    const QImage doc = makeDocument(6);
    const Quad truth = quadOf({QPointF(420, 120), QPointF(1180, 190), QPointF(1290, 1110), QPointF(300, 1050)});
    CombinePage page = pageFor(savePng(dir, "photo.png", photograph(doc, truth)));
    page.quad = truth;
    page.perspective.aspect = AspectMode::A4;
    Annotation hl;
    hl.type = Annotation::Type::Highlighter;
    hl.points = {QPointF(0.1, 0.3), QPointF(0.8, 0.3)};
    hl.stroke = QColor(255, 220, 0);
    hl.width = 0.03;
    hl.opacity = 0.4;
    Annotation arrow;
    arrow.type = Annotation::Type::Arrow;
    arrow.points = {QPointF(0.2, 1.0), QPointF(0.7, 1.1)};
    arrow.stroke = QColor(211, 47, 47);
    arrow.width = 0.006;
    Annotation text;
    text.type = Annotation::Type::Text;
    text.text = QString::fromUtf8("Zażółć gęślą jaźń – Łódź");
    text.rect = QRectF(0.1, 1.2, 0.8, 0.06);
    text.bold = true;
    text.stroke = QColor(20, 20, 160);
    page.annotations = {hl, arrow, text};
    ExportSettings s;
    s.margins = MarginMode::None;
    s.quality = QualityMode::High;
    const QString out = dir.filePath("annotated.pdf");
    const ExportResult r = CombinePdfExporter::exportTo(out, {page}, s, false, {}, nullptr);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    auto pdf = PdfDocument::openFile(out);
    ASSERT_TRUE(pdf);
    EXPECT_TRUE(pdf->pageText(0).contains(QString::fromUtf8("Zażółć gęślą jaźń – Łódź"))) << pdf->pageText(0).toStdString();
    // Annotations are vector objects: count path objects on the page.
    {
        std::lock_guard lock(pdfiumMutex());
        PdfPageHandle h(pdf->handle(), 0);
        int paths = 0, images = 0, texts = 0;
        for (int i = 0; i < FPDFPage_CountObjects(h.get()); ++i) {
            const int t = FPDFPageObj_GetType(FPDFPage_GetObject(h.get(), i));
            paths += t == FPDF_PAGEOBJ_PATH;
            images += t == FPDF_PAGEOBJ_IMAGE;
            texts += t == FPDF_PAGEOBJ_TEXT;
        }
        EXPECT_EQ(images, 1);
        EXPECT_GE(paths, 3); // highlighter, arrow shaft, arrow head
        EXPECT_GE(texts, 1);
    }
    // The straightened page: corner marks in the page corners; the highlighter is translucent.
    const QImage img = pdf->render(0, 1.5);
    const QRectF ir = CombinePdfExporter::plan(page, s).imageRect;
    const double k = 1.5;
    const int s4 = int(ir.width() * k / 21);
    EXPECT_LT(qGray(img.pixel(int(ir.left() * k) + 8 + s4 / 2, int(ir.top() * k) + 8 + s4 / 2)), 100);
    EXPECT_LT(qGray(img.pixel(int(ir.right() * k) - 8 - s4 / 2, int(ir.bottom() * k) - 8 - s4 / 2)), 100);
    const QColor under(img.pixel(int((ir.left() + 0.12 * ir.width()) * k), int((ir.top() + 0.3 * ir.width()) * k)));
    EXPECT_GT(under.red(), 200);
    EXPECT_GT(under.blue(), 90); // not fully opaque yellow
    // Independent parser.
    if (havePython())
        EXPECT_TRUE(runPython("import sys, pypdf\nr = pypdf.PdfReader(sys.argv[1])\n"
                              "t = r.pages[0].extract_text()\n"
                              "sys.exit(0 if len(r.pages) == 1 and 'Za\\u017c\\u00f3\\u0142\\u0107 g\\u0119\\u015bl\\u0105 ja\\u017a\\u0144' in t and '\\u0141\\u00f3d\\u017a' in t else 1)\n",
                              {out}));
}

TEST(CombineExport, UneditedJpegIsEmbeddedWithoutReencoding)
{
    QTemporaryDir dir;
    const QString jpg = dir.filePath("photo.jpg");
    ASSERT_TRUE(makeDocument(7, QSize(900, 1200)).save(jpg, "JPEG", 88));
    QFile f(jpg);
    f.open(QIODevice::ReadOnly);
    const QByteArray original = f.readAll();
    CombinePage page = pageFor(jpg);
    ExportSettings s;
    s.quality = QualityMode::Balanced;
    ASSERT_TRUE(CombinePdfExporter::plan(page, s).passthroughJpeg);
    const QString out = dir.filePath("jpeg.pdf");
    ASSERT_TRUE(CombinePdfExporter::exportTo(out, {page}, s, false, {}, nullptr).ok);
    auto pdf = PdfDocument::openFile(out);
    std::lock_guard lock(pdfiumMutex());
    PdfPageHandle h(pdf->handle(), 0);
    FPDF_PAGEOBJECT img = nullptr;
    for (int i = 0; i < FPDFPage_CountObjects(h.get()); ++i)
        if (FPDFPageObj_GetType(FPDFPage_GetObject(h.get(), i)) == FPDF_PAGEOBJ_IMAGE)
            img = FPDFPage_GetObject(h.get(), i);
    ASSERT_TRUE(img);
    const unsigned long len = FPDFImageObj_GetImageDataRaw(img, nullptr, 0);
    QByteArray raw(int(len), 0);
    FPDFImageObj_GetImageDataRaw(img, raw.data(), len);
    EXPECT_EQ(raw, original) << "the JPEG stream must be the original bytes";
}

TEST(CombineExport, CancelledExportWritesNothing)
{
    QTemporaryDir dir;
    const QList<CombinePage> pages = numberedPages(dir, 4);
    std::atomic_bool cancel{true};
    const ExportResult r = CombinePdfExporter::exportTo(dir.filePath("x.pdf"), pages, {}, false, {}, &cancel);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.cancelled);
    EXPECT_FALSE(QFile::exists(dir.filePath("x.pdf")));
}

// ===========================================================================
// Session / project

TEST(CombineSessionTest, AutosaveRestoreAndProjectWithEmbeddedClipboardImage)
{
    QTemporaryDir dir;
    CombineDocument doc;
    CombinePage file = pageFor(savePng(dir, "file.png", makeDocument(0, QSize(300, 400))));
    QImage clip(320, 200, QImage::Format_RGB32);
    clip.fill(Qt::darkCyan);
    QString err;
    CombinePage pasted = CombineSession::pageFromImage(clip, "Pasted image", &err);
    Annotation a;
    a.type = Annotation::Type::Rect;
    a.rect = QRectF(0.1, 0.1, 0.3, 0.2);
    pasted.annotations << a;
    pasted.quad = Geometry::defaultQuad(pasted.sourceSize);
    doc.addPages({file, pasted});
    ASSERT_TRUE(CombineSession::autosave(doc));
    EXPECT_TRUE(CombineSession::hasRecoverableSession());
    CombineDocument restored;
    ASSERT_TRUE(CombineSession::restoreSession(restored, &err)) << err.toStdString();
    ASSERT_EQ(restored.count(), 2);
    EXPECT_EQ(restored.page(1).annotations.size(), 1);
    EXPECT_TRUE(restored.page(1).quad.has_value());
    // Project file: the clipboard image is embedded, the file source referenced.
    const QString proj = dir.filePath("work.folvio");
    ASSERT_TRUE(CombineSession::saveProject(doc, proj, false, &err)) << err.toStdString();
    CombineDocument opened;
    QStringList warnings;
    ASSERT_TRUE(CombineSession::openProject(opened, proj, &warnings, &err)) << err.toStdString();
    EXPECT_EQ(opened.count(), 2);
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join(";").toStdString();
    EXPECT_TRUE(QFile::exists(opened.page(1).sourcePath));
    // A missing source is reported, never silently ignored.
    QFile::remove(file.sourcePath);
    EXPECT_EQ(CombineSession::sourceProblems(opened).size(), 1);
    CombineSession::discardSession(nullptr);
    EXPECT_FALSE(CombineSession::hasRecoverableSession());
}

// ===========================================================================
// End to end through the Qt GUI: Import -> Reorder -> Perspective -> Annotate -> Export -> Reopen

TEST(CombineGui, ImportReorderStraightenAnnotateExportReopen)
{
    ClipboardGuard clipboard;
    QTemporaryDir dir;
    const Quad truth = quadOf({QPointF(420, 120), QPointF(1180, 190), QPointF(1290, 1110), QPointF(300, 1050)});
    QStringList files;
    // Import order 3,1,2 (file names) - the user reorders to 1,2,3.
    for (int i : {2, 0, 1})
        files << savePng(dir, QStringLiteral("image_%1.png").arg(i + 1, 2, 10, QLatin1Char('0')), photograph(makeDocument(i), truth));

    CombineWorkspace ws;
    ws.resize(1500, 950);
    ws.show();
    QTest::qWaitForWindowExposed(&ws);
    ws.importFiles(files);
    QSignalSpy imported(&ws, &CombineWorkspace::importFinished);
    ASSERT_TRUE(imported.wait(20000));
    ASSERT_EQ(ws.document()->count(), 3);

    // A screenshot pasted with Ctrl+V becomes page 4.
    QImage shot(500, 300, QImage::Format_RGB32);
    shot.fill(QColor("#455a64"));
    QApplication::clipboard()->setImage(shot);
    ws.setFocus();
    QTest::keyClick(&ws, Qt::Key_V, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(ws.document()->count(), 4, 5000);
    EXPECT_TRUE(ws.document()->page(3).fromClipboard);

    // Reorder with the keyboard in the thumbnail grid: image_01 (row 1) up to row 0.
    QListView* grid = nullptr;
    for (auto* v : ws.findChildren<QListView*>())
        if (v->model() == ws.model() && v->isVisible())
            grid = v;
    ASSERT_TRUE(grid);
    grid->setFocus();
    grid->selectionModel()->select(ws.model()->index(1), QItemSelectionModel::ClearAndSelect);
    QTest::keyClick(grid, Qt::Key_Up, Qt::ControlModifier);
    // image_02 (now row 2) stays after image_03? Move image_03 (row 1) down once.
    grid->selectionModel()->select(ws.model()->index(1), QItemSelectionModel::ClearAndSelect);
    QTest::keyClick(grid, Qt::Key_Down, Qt::ControlModifier);
    QStringList order;
    for (const CombinePage& p : ws.document()->pages())
        order << p.displayName;
    ASSERT_EQ(order.mid(0, 3), QStringList({"image_01.png", "image_02.png", "image_03.png"}));

    // Edit page 1 (selected in the grid): perspective by dragging the four handles onto the page corners.
    grid->selectionModel()->select(ws.model()->index(0), QItemSelectionModel::ClearAndSelect);
    ws.setStep(CombineWorkspace::Step::Edit);
    QTest::qWait(150);
    PageCanvas* canvas = ws.canvas();
    ASSERT_EQ(canvas->pageId(), ws.document()->page(0).id);
    auto* toolGroup = ws.findChildren<QButtonGroup*>().value(1);
    QAbstractButton* perspectiveButton = nullptr;
    for (auto* g : ws.findChildren<QButtonGroup*>())
        if (auto* b = g->button(int(PageCanvas::Tool::Perspective)); b && b->text() == QObject::tr("Perspective"))
            perspectiveButton = b;
    Q_UNUSED(toolGroup)
    ASSERT_TRUE(perspectiveButton);
    QTest::mouseClick(perspectiveButton, Qt::LeftButton);
    QTest::qWait(100);
    ASSERT_EQ(canvas->tool(), PageCanvas::Tool::Perspective);
    auto* scroll = qobject_cast<QScrollArea*>(canvas->parentWidget()->parentWidget());
    for (int c = 0; c < 4; ++c) {
        const QPoint from = canvas->fullToWidget(canvas->pendingQuad()->pts[size_t(c)]).toPoint();
        const QPoint to = canvas->fullToWidget(truth.pts[size_t(c)]).toPoint();
        if (scroll)
            scroll->ensureVisible(from.x(), from.y());
        QTest::mousePress(canvas, Qt::LeftButton, {}, from);
        QTest::mouseMove(canvas, (from + to) / 2);
        QTest::mouseMove(canvas, to);
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, to);
    }
    for (int c = 0; c < 4; ++c)
        EXPECT_LT(QLineF(canvas->pendingQuad()->pts[size_t(c)], truth.pts[size_t(c)]).length(), 6.0) << "corner " << c;
    QPushButton* straighten = nullptr;
    for (auto* b : ws.findChildren<QPushButton*>())
        if (b->text() == QObject::tr("Straighten"))
            straighten = b;
    ASSERT_TRUE(straighten && straighten->isEnabled());
    QTest::mouseClick(straighten, Qt::LeftButton);
    ASSERT_TRUE(ws.document()->page(0).quad.has_value());

    // Annotate: pen stroke and a text box with Polish characters.
    for (auto* g : ws.findChildren<QButtonGroup*>())
        if (auto* b = g->button(int(PageCanvas::Tool::Pen)); b && b->text() == QObject::tr("Pen"))
            QTest::mouseClick(b, Qt::LeftButton);
    ASSERT_EQ(canvas->tool(), PageCanvas::Tool::Pen);
    const QPoint p1 = canvas->fromPage(QPointF(0.15, 0.6)).toPoint(), p2 = canvas->fromPage(QPointF(0.7, 0.62)).toPoint();
    QTest::mousePress(canvas, Qt::LeftButton, {}, p1);
    for (int i = 1; i <= 8; ++i)
        QTest::mouseMove(canvas, p1 + (p2 - p1) * i / 8);
    QTest::mouseRelease(canvas, Qt::LeftButton, {}, p2);
    for (auto* g : ws.findChildren<QButtonGroup*>())
        if (auto* b = g->button(int(PageCanvas::Tool::Text)); b && b->text() == QObject::tr("Text"))
            QTest::mouseClick(b, Qt::LeftButton);
    QTest::mouseClick(canvas, Qt::LeftButton, {}, canvas->fromPage(QPointF(0.1, 1.2)).toPoint());
    auto* editor = canvas->findChild<CombineTextEditor*>();
    ASSERT_TRUE(editor && editor->isVisible());
    typeText(editor, QString::fromUtf8("Sprawdzone – zażółć"));
    // Ctrl+V inside the text box pastes text, it does not import a page.
    QApplication::clipboard()->setText(QStringLiteral(" OK"));
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
    EXPECT_EQ(ws.document()->count(), 4);
    ASSERT_EQ(ws.document()->page(0).annotations.size(), 2);
    EXPECT_EQ(ws.document()->page(0).annotations[1].text, QString::fromUtf8("Sprawdzone – zażółć OK"));

    // Undo/redo through the shortcut keeps working in the workspace.
    canvas->setFocus();
    QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(ws.document()->page(0).annotations.size(), 1);
    QTest::keyClick(canvas, Qt::Key_Y, Qt::ControlModifier);
    EXPECT_EQ(ws.document()->page(0).annotations.size(), 2);

    // Export and reopen.
    ws.setStep(CombineWorkspace::Step::Export);
    const QString out = dir.filePath(QString::fromUtf8("Wynik łączenia.pdf"));
    QSignalSpy exported(&ws, &CombineWorkspace::exportFinished);
    ws.exportToPath(out, false);
    ASSERT_TRUE(exported.wait(60000));
    ASSERT_TRUE(exported.first().first().toBool()) << ws.lastExportError().toStdString();
    auto pdf = PdfDocument::openFile(out);
    ASSERT_TRUE(pdf);
    ASSERT_EQ(pdf->pageCount(), 4);
    EXPECT_TRUE(pdf->pageText(0).contains(QString::fromUtf8("Sprawdzone – zażółć OK")));
    const ExportSettings s = ws.document()->exportSettings();
    for (int i = 0; i < 3; ++i) {
        const QImage img = pdf->render(i, 1.0);
        const QRectF ir = CombinePdfExporter::plan(ws.document()->page(i), s).imageRect;
        // Page 1 was straightened: its colour block is at the page's top-left like in the document.
        if (i == 0) {
            const int m = int(ir.width() / 14);
            EXPECT_TRUE(closeColor(img.pixel(int(ir.left()) + 2 * m, int(ir.top()) + 2 * m), kPageColors[0]));
        }
    }
    // Pages 2 and 3 keep the user's order (their documents 1 and 2 have different colour blocks).
    auto blockColorInPhoto = [&](int pageIndex) {
        const QImage img = pdf->render(pageIndex, 1.0);
        const QRectF ir = CombinePdfExporter::plan(ws.document()->page(pageIndex), s).imageRect;
        // In the unstraightened photo the block sits near truth TL + offset; sample inside the page area.
        const QPointF photoPoint = truth.pts[0] + QPointF(120, 110);
        const double k = ir.width() / 1600.0;
        return QColor(img.pixel(int(ir.left() + photoPoint.x() * k), int(ir.top() + photoPoint.y() * k)));
    };
    EXPECT_TRUE(closeColor(blockColorInPhoto(1), kPageColors[1], 70));
    EXPECT_TRUE(closeColor(blockColorInPhoto(2), kPageColors[2], 70));
    // Source files untouched.
    for (const QString& f : files)
        EXPECT_TRUE(QFile::exists(f));
    CombineSession::discardSession(nullptr);
}
