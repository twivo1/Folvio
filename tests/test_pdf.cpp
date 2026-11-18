#include "TestSupport.h"

#include "editor/pdf_native/FontMatcher.h"
#include "editor/pdf_native/PdfTextLayout.h"
#include "engines/pdf/PdfDocument.h"

#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace folvio;
using namespace folvio::test;

TEST(Pdf, SimpleTextAndAnalysis)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("simple.pdf");
    ASSERT_TRUE(makeTextPdf(pdf, {{"Hello PDF world"}}));
    auto doc = PdfDocument::openFile(pdf);
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->pageCount(), 1);
    EXPECT_NEAR(doc->pageSize(0).width(), 595.0, 1.0);
    EXPECT_TRUE(doc->pageText(0).contains("Hello PDF world"));
    const PdfAnalysis a = doc->analyze();
    EXPECT_TRUE(a.hasTextLayer);
    EXPECT_FALSE(a.isScanned);
    EXPECT_FALSE(a.encrypted);
    EXPECT_EQ(a.signatureCount, 0);
    const QImage img = doc->render(0, 1.0);
    EXPECT_EQ(img.width(), 595);
    EXPECT_LT(pixelDifference(img, QImage(img.size(), QImage::Format_RGB32)), 1.0);
}

TEST(Pdf, MultiPageAndPolishText)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("polskie ąćęłńóśźż.pdf");
    QList<QStringList> pages;
    for (int i = 0; i < 12; ++i)
        pages << QStringList{QStringLiteral("Strona %1 – zażółć gęślą jaźń").arg(i + 1)};
    ASSERT_TRUE(makeTextPdf(pdf, pages));
    auto doc = PdfDocument::openFile(pdf);
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->pageCount(), 12);
    EXPECT_TRUE(doc->pageText(11).contains(QString::fromUtf8("Strona 12 – zażółć gęślą jaźń")));
}

TEST(Pdf, TablesAndImagesDetected)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("table.pdf");
    ASSERT_TRUE(makeTablePdf(pdf));
    auto doc = PdfDocument::openFile(pdf);
    ASSERT_TRUE(doc);
    const PdfAnalysis a = doc->analyze();
    EXPECT_TRUE(a.likelyHasTables);
    EXPECT_GE(a.totalImages, 1);
    // Table cells become separate blocks (column gaps split lines).
    const auto blocks = PdfTextLayout::build(doc->textRuns(0), 0, doc->horizontalRules(0));
    QStringList texts;
    for (const auto& b : blocks)
        texts << b.text();
    EXPECT_TRUE(texts.contains("Item")) << texts.join(" | ").toStdString();
    EXPECT_TRUE(texts.contains("19.99"));
}

TEST(Pdf, ScannedDocumentDetected)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("scan.pdf");
    ASSERT_TRUE(makeScannedPdf(pdf, "Scanned invoice\nTotal 120 PLN"));
    auto doc = PdfDocument::openFile(pdf);
    ASSERT_TRUE(doc);
    const PdfAnalysis a = doc->analyze();
    EXPECT_FALSE(a.hasTextLayer);
    EXPECT_TRUE(a.isScanned);
}

TEST(Pdf, CorruptedAndNonPdfFilesFailCleanly)
{
    QTemporaryDir dir;
    const QString bad = dir.filePath("bad.pdf");
    QFile f(bad);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("%PDF-1.7\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\ngarbage garbage");
    f.close();
    PdfDocument::OpenError err = PdfDocument::OpenError::None;
    QString msg;
    EXPECT_FALSE(PdfDocument::openFile(bad, {}, &err, &msg));
    EXPECT_FALSE(msg.isEmpty());
    // Truncated real PDF.
    const QString good = dir.filePath("good.pdf");
    ASSERT_TRUE(makeTextPdf(good, {{"a"}, {"b"}}));
    QFile g(good);
    ASSERT_TRUE(g.open(QIODevice::ReadOnly));
    const QByteArray half = g.readAll().left(200);
    auto doc = PdfDocument::openData(half, {}, &err, &msg);
    EXPECT_FALSE(doc);
}

TEST(Pdf, UnusualFontsAreMatchedAndEditable)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("fonts.pdf");
    ASSERT_TRUE(makeTextPdf(pdf, {{"Monospaced line"}}, "Courier New"));
    auto doc = PdfDocument::openFile(pdf);
    ASSERT_TRUE(doc);
    const auto runs = doc->textRuns(0);
    ASSERT_FALSE(runs.isEmpty());
    const FontMatch m = FontMatcher::match(runs.first().fontName, false, false, false, true);
    ASSERT_TRUE(m.isValid());
    EXPECT_EQ(m.family, "Courier New") << runs.first().fontName.toStdString();
    // Exotic names fall back to a sensible installed family.
    EXPECT_TRUE(FontMatcher::match("ABCDEF+SomeCustomGrotesk-Bold", true, false, false, false).isValid());
    EXPECT_EQ(FontMatcher::match("TimesNewRomanPS-BoldMT", true, false, true, false).family, "Times New Roman");
    EXPECT_EQ(FontMatcher::match("Helvetica-Oblique", false, true, false, false).family, "Arial");
}

TEST(Pdf, LayoutGroupsParagraphLines)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("para.pdf");
    ASSERT_TRUE(makeTextPdf(pdf, {{"Line one of paragraph", "line two of paragraph", "", "", "", "Separate block"}}));
    auto doc = PdfDocument::openFile(pdf);
    const auto blocks = PdfTextLayout::build(doc->textRuns(0), 0);
    ASSERT_EQ(blocks.size(), 2);
    EXPECT_EQ(blocks[0].lineCount(), 2);
    EXPECT_EQ(blocks[0].text(), "Line one of paragraph\nline two of paragraph");
    EXPECT_EQ(blocks[1].text(), "Separate block");
}

