// Recognize Text flows in the Auto Change image editor (shared with Combine):
// select an area -> recognise (printed / handwriting) -> edit -> copy / insert / replace (with preview),
// undo, export; consent before any download; warnings for uneven backgrounds and formulas.
#include "TestSupport.h"

#include "combine/CombineModel.h"
#include "combine/CombinePdfExporter.h"
#include "combine/CombineWorkspace.h"
#include "combine/PageCanvas.h"
#include "combine/RecognizePanel.h"
#include "core/persistence/Database.h"
#include "engines/ocr/HandwritingOcr.h"
#include "engines/ocr/TesseractEngine.h"
#include "engines/pdf/PdfDocument.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDir>
#include <QImageReader>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <gtest/gtest.h>

#include <iostream>

using namespace folvio;
using namespace folvio::combine;

namespace {

// Paper photo with printed Polish lines (synthetic, rendered here).
QImage notePage(bool grid)
{
    QImage img(1600, 1000, QImage::Format_RGB32);
    img.fill(QColor(246, 243, 232));
    QPainter p(&img);
    if (grid) {
        p.setPen(QPen(QColor(150, 180, 220), 2));
        for (int x = 0; x < img.width(); x += 40)
            p.drawLine(x, 0, x, img.height());
        for (int y = 0; y < img.height(); y += 40)
            p.drawLine(0, y, img.width(), y);
    }
    QFont f("Arial");
    f.setPixelSize(54);
    p.setFont(f);
    p.setPen(QColor(20, 40, 120));
    p.drawText(120, 260, QString::fromUtf8("Zażółć gęślą jaźń"));
    p.drawText(120, 360, QStringLiteral("Plan na jutro: zakupy"));
    return img;
}

void drag(QWidget* w, QPointF from, QPointF to)
{
    QTest::mousePress(w, Qt::LeftButton, {}, from.toPoint());
    for (int i = 1; i <= 6; ++i)
        QTest::mouseMove(w, (from + (to - from) * (i / 6.0)).toPoint());
    QTest::mouseRelease(w, Qt::LeftButton, {}, to.toPoint());
}

struct ImageEditor {
    QTemporaryDir dir;
    QString file;
    CombineWorkspace ws{nullptr, CombineWorkspace::Mode::ImageEditor};
    PageCanvas* canvas = nullptr;
    RecognizePanel* panel = nullptr;
    explicit ImageEditor(bool grid = false)
    {
        Database::instance().open();
        file = dir.filePath("note.png");
        notePage(grid).save(file);
        ws.resize(1400, 950);
        ws.show();
        ws.activateWindow();
        EXPECT_TRUE(QTest::qWaitForWindowActive(&ws, 3000));
        QSignalSpy imported(&ws, &CombineWorkspace::importFinished);
        ws.openImageFile(file);
        EXPECT_TRUE(imported.wait(20000));
        QTest::qWait(150);
        canvas = ws.canvas();
        panel = ws.recognizePanel();
    }
    ~ImageEditor() { ws.closeImage(); }
    // Drags the Recognize rectangle over 'pageArea' (page units) like a user.
    bool recognize(const QRectF& pageArea, TextRecognition::Mode mode)
    {
        QTest::mouseClick(ws.findChild<QAbstractButton*>("image-recognize"), Qt::LeftButton);
        EXPECT_EQ(canvas->tool(), PageCanvas::Tool::Recognize);
        panel->setMode(mode);
        QSignalSpy done(panel, &RecognizePanel::recognitionFinished);
        drag(canvas, canvas->fromPage(pageArea.topLeft()), canvas->fromPage(pageArea.bottomRight()));
        if (!done.wait(60000))
            return false;
        return done.first().first().toBool();
    }
};

const QRectF kTextArea(60.0 / 1600, 180.0 / 1600, 900.0 / 1600, 220.0 / 1600); // both lines, page units

double cer(const QString& truth, const QString& hyp)
{
    const QString t = truth.simplified(), h = hyp.simplified();
    std::vector<int> prev(size_t(h.size()) + 1), cur(prev.size());
    for (int j = 0; j <= h.size(); ++j)
        prev[size_t(j)] = j;
    for (int i = 1; i <= t.size(); ++i) {
        cur[0] = i;
        for (int j = 1; j <= h.size(); ++j)
            cur[size_t(j)] = std::min({prev[size_t(j)] + 1, cur[size_t(j - 1)] + 1, prev[size_t(j - 1)] + (t[i - 1] != h[j - 1] ? 1 : 0)});
        std::swap(prev, cur);
    }
    return t.isEmpty() ? 0 : double(prev[size_t(h.size())]) / t.size();
}

} // namespace

TEST(RecognizeText, PrintedAreaInsertAsTextKeepsTheImageAndExports)
{
    if (!TesseractEngine::isAvailable())
        GTEST_SKIP() << "Tesseract not installed";
    ImageEditor e;
    const QByteArray sourceBefore = [&] {
        QFile f(e.file);
        f.open(QIODevice::ReadOnly);
        return f.readAll();
    }();
    ASSERT_TRUE(e.recognize(kTextArea, TextRecognition::Mode::Printed)) << e.panel->lastResult().error.toStdString();
    // The dragged area is the one recognised (page units), the result is editable.
    EXPECT_NEAR(e.panel->area().left(), kTextArea.left(), 0.01);
    EXPECT_NEAR(e.panel->area().width(), kTextArea.width(), 0.01);
    const QString text = e.panel->textEdit()->toPlainText();
    EXPECT_LT(cer(QString::fromUtf8("Zażółć gęślą jaźń\nPlan na jutro: zakupy"), text), 0.1) << text.toStdString();
    EXPECT_EQ(e.panel->lastResult().engine, "printed");
    EXPECT_FALSE(e.panel->textEdit()->isReadOnly());
    EXPECT_TRUE(e.panel->findChild<QLabel*>("recognize-status")->text().contains("%"));
    test::saveScreenshot(&e.ws, "recognize-printed");

    // Copy.
    QTest::mouseClick(e.panel->findChild<QAbstractButton*>("recognize-copy"), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(QApplication::clipboard()->text(), text, 3000);

    // Corrected by the user, then inserted as a normal, editable text annotation below the area.
    e.panel->textEdit()->setPlainText(QString::fromUtf8("Zażółć gęślą jaźń\nPlan na jutro: zakupy"));
    QTest::mouseClick(e.panel->findChild<QAbstractButton*>("recognize-insert"), Qt::LeftButton);
    const CombinePage page = e.ws.document()->page(0);
    ASSERT_EQ(page.annotations.size(), 1);
    const Annotation& t = page.annotations[0];
    EXPECT_EQ(t.type, Annotation::Type::Text);
    EXPECT_EQ(t.text, QString::fromUtf8("Zażółć gęślą jaźń\nPlan na jutro: zakupy"));
    EXPECT_GE(t.rect.top(), kTextArea.bottom());
    EXPECT_TRUE(e.canvas->recognizeArea().isEmpty());

    // The image file is untouched; Undo removes the text.
    QFile f(e.file);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), sourceBefore);
    e.ws.document()->undo();
    EXPECT_TRUE(e.ws.document()->page(0).annotations.isEmpty());
    e.ws.document()->redo();

    // Convert to PDF: the inserted text is real text with Polish characters.
    const QString pdf = e.dir.filePath("note.pdf");
    QSignalSpy exported(&e.ws, &CombineWorkspace::exportFinished);
    e.ws.exportToPath(pdf, false);
    ASSERT_TRUE(exported.wait(60000));
    ASSERT_TRUE(exported.first().first().toBool()) << e.ws.lastExportError().toStdString();
    EXPECT_TRUE(PdfDocument::openFile(pdf)->pageText(0).contains(QString::fromUtf8("gęślą")));
}

TEST(RecognizeText, ReplaceSelectionPreviewsBeforeAfterAndUndoRestores)
{
    if (!TesseractEngine::isAvailable())
        GTEST_SKIP() << "Tesseract not installed";
    ImageEditor e;
    ASSERT_TRUE(e.recognize(kTextArea, TextRecognition::Mode::Printed));
    // The dialog: variants, before/after previews that differ, plain paper -> matched background.
    bool checked = false;
    QTimer::singleShot(400, e.panel, [&] {
        auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dlg)
            return;
        const QPixmap before = dlg->findChild<QLabel*>("preview-before")->pixmap();
        QPixmap after = dlg->findChild<QLabel*>("preview-after")->pixmap();
        EXPECT_FALSE(before.isNull());
        EXPECT_FALSE(after.isNull());
        EXPECT_NE(before.toImage(), after.toImage()) << "the preview must show the change";
        test::saveScreenshot(dlg, "recognize-replace-dialog");
        for (int v = 0; v < 4; ++v)
            EXPECT_TRUE(dlg->findChild<QAbstractButton*>(QStringLiteral("variant-%1").arg(v)));
        EXPECT_TRUE(dlg->findChild<QAbstractButton*>("variant-2")->isChecked()) << "plain paper: matched background";
        // Switching the variant updates the preview.
        dlg->findChild<QAbstractButton*>("variant-3")->click();
        EXPECT_NE(dlg->findChild<QLabel*>("preview-after")->pixmap().toImage(), after.toImage());
        dlg->findChild<QAbstractButton*>("variant-2")->click();
        checked = true;
        dlg->findChild<QAbstractButton*>("replace-apply")->click();
    });
    QTest::mouseClick(e.panel->findChild<QAbstractButton*>("recognize-replace"), Qt::LeftButton);
    ASSERT_TRUE(checked) << "the replace dialog did not open";
    const CombinePage page = e.ws.document()->page(0);
    ASSERT_EQ(page.annotations.size(), 2);
    EXPECT_EQ(page.annotations[0].type, Annotation::Type::Rect);
    EXPECT_TRUE(page.annotations[0].hasFill);
    // Matched paper colour, not white.
    EXPECT_LT(std::abs(page.annotations[0].fill.red() - 246) + std::abs(page.annotations[0].fill.blue() - 232), 20);
    EXPECT_EQ(page.annotations[1].type, Annotation::Type::Text);
    EXPECT_LE(page.annotations[1].rect.bottom(), kTextArea.bottom() + 1e-6) << "the text fits into the selection";
    // The cover alone hides the old ink inside the area (no blue strokes left under the new text).
    CombinePage coverOnly = page;
    coverOnly.annotations = {page.annotations[0]};
    const QImage after = RecognizePanel::renderPage(coverOnly, 1600);
    int blueInk = 0;
    for (int y = 190; y < 400; y += 2)
        for (int x = 70; x < 950; x += 2) {
            const QColor c(after.pixel(x, y));
            blueInk += c.blue() > 90 && c.red() < 60 && c.green() < 70 ? 1 : 0;
        }
    EXPECT_EQ(blueInk, 0);
    // One Undo step restores the original look.
    e.ws.document()->undo();
    EXPECT_TRUE(e.ws.document()->page(0).annotations.isEmpty());
}

TEST(RecognizeText, GridPaperWarnsAndDefaultsToOverlayMathIsFlagged)
{
    if (!TesseractEngine::isAvailable())
        GTEST_SKIP() << "Tesseract not installed";
    ImageEditor e(true);
    ASSERT_TRUE(e.recognize(kTextArea, TextRecognition::Mode::Printed));
    // Tesseract often reads nothing through grid lines (documented limitation); the user can always
    // type or correct the text - the replace options depend on the area, not on the recogniser.
    if (e.panel->textEdit()->toPlainText().trimmed().isEmpty())
        e.panel->textEdit()->setPlainText(QString::fromUtf8("Zażółć gęślą jaźń"));
    EXPECT_FALSE(e.panel->analyzeBackground().uniform);
    bool seen = false;
    QTimer::singleShot(400, e.panel, [&] {
        auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dlg)
            return;
        bool warning = false;
        for (auto* l : dlg->findChildren<QLabel*>())
            warning |= l->text().contains("grid", Qt::CaseInsensitive);
        EXPECT_TRUE(warning);
        EXPECT_TRUE(dlg->findChild<QAbstractButton*>("variant-0")->isChecked()) << "overlay keeps the grid visible";
        seen = true;
        dlg->reject();
    });
    QTest::mouseClick(e.panel->findChild<QAbstractButton*>("recognize-replace"), Qt::LeftButton);
    EXPECT_TRUE(seen);
    EXPECT_TRUE(e.ws.document()->page(0).annotations.isEmpty()) << "Cancel changes nothing";

    RecognitionResult math;
    math.ok = true;
    math.lines << RecognizedLine{QStringLiteral("x^2 + 3x = (7 - 2) / 4"), 0.5, {}};
    EXPECT_TRUE(RecognizePanel::looksLikeMathOrDrawing(math));
    RecognitionResult words;
    words.lines << RecognizedLine{QString::fromUtf8("Kupić mleko i chleb, 2 sztuki"), 0.9, {}};
    EXPECT_FALSE(RecognizePanel::looksLikeMathOrDrawing(words));
}

TEST(RecognizeText, HandwritingNeedsConsentAndNeverDownloadsWithoutIt)
{
    const QByteArray saved = qgetenv("FOLVIO_HANDWRITING_MODELS");
    QTemporaryDir empty;
    qputenv("FOLVIO_HANDWRITING_MODELS", empty.path().toUtf8());
    {
        ImageEditor e;
        if (!HandwritingOcr::runtimeAvailable())
            GTEST_SKIP() << "ONNX Runtime not part of this build";
        bool asked = false;
        QTimer::singleShot(400, e.panel, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box)
                return;
            asked = true;
            EXPECT_TRUE(box->text().contains("never uploaded"));
            EXPECT_TRUE(box->text().contains("huggingface.co/PaddlePaddle"));
            box->button(QMessageBox::Cancel)->click();
        });
        QTest::mouseClick(e.ws.findChild<QAbstractButton*>("image-recognize"), Qt::LeftButton);
        e.panel->setMode(TextRecognition::Mode::Handwriting);
        drag(e.canvas, e.canvas->fromPage(kTextArea.topLeft()), e.canvas->fromPage(kTextArea.bottomRight()));
        QTest::qWait(600);
        EXPECT_TRUE(asked) << "consent was not requested";
        EXPECT_FALSE(e.panel->isBusy());
        EXPECT_TRUE(QDir(empty.path()).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty()) << "nothing may be downloaded";
        EXPECT_TRUE(e.panel->findChild<QAbstractButton*>("recognize-download")->isVisible());
        e.panel->setMode(TextRecognition::Mode::Auto);
    }
    if (saved.isEmpty())
        qunsetenv("FOLVIO_HANDWRITING_MODELS");
    else
        qputenv("FOLVIO_HANDWRITING_MODELS", saved);
}

TEST(RecognizeText, HandwritingAreaOnRealSample)
{
    if (!HandwritingOcr::isAvailable())
        GTEST_SKIP() << "handwriting models not installed (FOLVIO_HANDWRITING_MODELS)";
    const QString dir = qEnvironmentVariable("FOLVIO_HANDWRITING_SAMPLES");
    if (dir.isEmpty() || !QFile::exists(dir + "/eng_AF_011.png"))
        GTEST_SKIP() << "FOLVIO_HANDWRITING_SAMPLES (GNHK evaluation pages) not set";
    Database::instance().open();
    CombineWorkspace ws(nullptr, CombineWorkspace::Mode::ImageEditor);
    ws.resize(1400, 950);
    ws.show();
    QSignalSpy imported(&ws, &CombineWorkspace::importFinished);
    ws.openImageFile(dir + "/eng_AF_011.png");
    ASSERT_TRUE(imported.wait(20000));
    QTest::qWait(150);
    RecognizePanel* panel = ws.recognizePanel();
    ws.setTool(PageCanvas::Tool::Recognize);
    panel->setMode(TextRecognition::Mode::Handwriting);
    QSignalSpy done(panel, &RecognizePanel::recognitionFinished);
    // The whole page as the area: the same input as the engine benchmark.
    const QSizeF s = ws.document()->page(0).sourceSize;
    PageCanvas* c = ws.canvas();
    drag(c, c->fromPage(QPointF(0.002, 0.002)), c->fromPage(QPointF(0.998, s.height() / s.width() - 0.002)));
    ASSERT_TRUE(done.wait(60000));
    ASSERT_TRUE(done.first().first().toBool()) << panel->lastResult().error.toStdString();
    EXPECT_EQ(panel->lastResult().engine, "handwriting");
    EXPECT_GT(panel->textEdit()->toPlainText().size(), 20);
    test::saveScreenshot(&ws, "recognize-handwriting");
    std::cout << "  recognised: " << panel->textEdit()->toPlainText().left(160).toStdString() << "\n";
    ws.closeImage();
}
