// Reconstructed (rich) editor pipeline, DOCX specifics and shutdown behaviour.
#include "TestSupport.h"

#include "core/conversions/ConversionRegistry.h"
#include "core/conversions/OutputValidator.h"
#include "core/deps/DependencyManager.h"
#include "core/jobs/ConversionJob.h"
#include "editor/rich/RichDocumentIO.h"
#include "engines/pdf/PdfDocument.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>

#include <QPainter>
#include <QDir>
#include <gtest/gtest.h>

using namespace folvio;
using namespace folvio::test;

namespace {

// Minimal WordprocessingML package with a header and a footer, written with Python's zipfile.
bool makeDocxWithHeaderFooter(const QString& path)
{
    const char* script = R"PY(
import sys, zipfile
ct = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
<Override PartName="/word/header1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml"/>
<Override PartName="/word/footer1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml"/>
</Types>'''
rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>'''
docrels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rH" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/header" Target="header1.xml"/>
<Relationship Id="rF" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer" Target="footer1.xml"/>
</Relationships>'''
W = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"'
doc = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document {W}><w:body>
<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Body heading</w:t></w:r></w:p>
<w:p><w:r><w:t xml:space="preserve">Treść dokumentu z polskimi znakami: żółw.</w:t></w:r></w:p>
<w:sectPr><w:headerReference w:type="default" r:id="rH"/><w:footerReference w:type="default" r:id="rF"/>
<w:pgSz w:w="11906" w:h="16838"/><w:pgMar w:top="1417" w:right="1417" w:bottom="1417" w:left="1417" w:header="708" w:footer="708" w:gutter="0"/></w:sectPr>
</w:body></w:document>'''
hdr = f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:hdr {W}><w:p><w:r><w:t>HEADER-ACME-2026</w:t></w:r></w:p></w:hdr>'
ftr = f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:ftr {W}><w:p><w:r><w:t>FOOTER-CONFIDENTIAL</w:t></w:r></w:p></w:ftr>'
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('[Content_Types].xml', ct)
    z.writestr('_rels/.rels', rels)
    z.writestr('word/_rels/document.xml.rels', docrels)
    z.writestr('word/document.xml', doc)
    z.writestr('word/header1.xml', hdr)
    z.writestr('word/footer1.xml', ftr)
)PY";
    return runPython(QString::fromUtf8(script), {path});
}

} // namespace

TEST(Docx, HeadersAndFootersSurviveConversionToPdf)
{
    if (!havePython())
        GTEST_SKIP() << "python not available to build the DOCX fixture";
    if (!DependencyManager::instance().available("soffice"))
        GTEST_SKIP() << "LibreOffice not installed";
    QTemporaryDir dir;
    const QString docx = dir.filePath("report.docx");
    ASSERT_TRUE(makeDocxWithHeaderFooter(docx));
    EXPECT_TRUE(OutputValidator::validate(docx, "docx").ok);
    ConversionRequest req;
    req.inputPath = docx;
    req.inputFormat = "docx";
    req.outputFormat = "pdf";
    req.outputPath = dir.filePath("report.pdf");
    std::atomic_bool cancel{false};
    const auto r = ConversionExecutor::execute(req, ConversionPlanner().plan("docx", "pdf"), &cancel, {});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const QString text = pdfText(r.outputs.first());
    EXPECT_TRUE(text.contains("HEADER-ACME-2026")) << text.toStdString();
    EXPECT_TRUE(text.contains("FOOTER-CONFIDENTIAL"));
    EXPECT_TRUE(text.contains(QString::fromUtf8("żółw")));
}

TEST(RichEditor, DocxRoundTripThroughEditorModel)
{
    if (!havePython() || !DependencyManager::instance().available("soffice"))
        GTEST_SKIP() << "python or LibreOffice not available";
    QTemporaryDir dir;
    const QString docx = dir.filePath("edit me.docx");
    ASSERT_TRUE(makeDocxWithHeaderFooter(docx));
    std::atomic_bool cancel{false};
    const auto im = RichDocumentIO::importFile(docx, "docx", dir.filePath("work"), &cancel);
    ASSERT_TRUE(im.ok) << im.error.toStdString();
    QTextDocument doc;
    doc.setHtml(im.content);
    ASSERT_TRUE(doc.toPlainText().contains("Body heading"));
    // Edit: replace a word and add a table + a list item.
    QTextCursor c = doc.find("Body heading");
    ASSERT_FALSE(c.isNull());
    c.insertText("Edited heading");
    QTextCursor end(&doc);
    end.movePosition(QTextCursor::End);
    end.insertBlock();
    end.insertTable(2, 2);
    end.insertText("Cell A1");
    // Serialize: ODT -> DOCX via LibreOffice, validate, and read back.
    QString err;
    const QString odt = dir.filePath("tmp.odt");
    ASSERT_TRUE(RichDocumentIO::writeIntermediate(&doc, "odt", odt, &err)) << err.toStdString();
    const QString out = dir.filePath("saved.docx");
    ASSERT_TRUE(RichDocumentIO::convertFromOdt(odt, "docx", out, dir.filePath("work2"), &cancel, &err)) << err.toStdString();
    EXPECT_TRUE(OutputValidator::validate(out, "docx").ok);
    const auto back = RichDocumentIO::importFile(out, "docx", dir.filePath("work3"), &cancel);
    ASSERT_TRUE(back.ok) << back.error.toStdString();
    QTextDocument check;
    check.setHtml(back.content);
    const QString plain = check.toPlainText();
    EXPECT_TRUE(plain.contains("Edited heading")) << plain.toStdString();
    EXPECT_TRUE(plain.contains("Cell A1"));
    EXPECT_TRUE(plain.contains(QString::fromUtf8("żółw")));
    // PDF export of the editor model.
    const QString pdf = dir.filePath("saved.pdf");
    ASSERT_TRUE(RichDocumentIO::writeIntermediate(&doc, "pdf", pdf, &err)) << err.toStdString();
    EXPECT_TRUE(pdfText(pdf).contains("Edited heading"));
}

TEST(RichEditor, MarkdownAndTextSaveBack)
{
    QTemporaryDir dir;
    const QString md = dir.filePath("notes.md");
    QFile f(md);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("# Plan\n\n- pierwszy punkt\n- drugi punkt\n");
    f.close();
    std::atomic_bool cancel{false};
    const auto im = RichDocumentIO::importFile(md, "md", dir.filePath("w"), &cancel);
    ASSERT_TRUE(im.ok);
    QTextDocument doc;
    doc.setMarkdown(im.content);
    QTextCursor c = doc.find("drugi");
    c.insertText("trzeci");
    QString err;
    ASSERT_TRUE(RichDocumentIO::writeIntermediate(&doc, "md", md, &err));
    QFile r(md);
    ASSERT_TRUE(r.open(QIODevice::ReadOnly));
    const QString saved = QString::fromUtf8(r.readAll());
    EXPECT_TRUE(saved.contains("# Plan"));
    EXPECT_TRUE(saved.contains("trzeci punkt"));
    EXPECT_TRUE(saved.contains("- pierwszy punkt") || saved.contains("* pierwszy punkt"));
}

TEST(Stability, ShutdownDuringProcessingStopsQuickly)
{
    if (!DependencyManager::instance().available("ffmpeg"))
        GTEST_SKIP() << "ffmpeg not installed";
    QTemporaryDir dir;
    const QString mp4 = dir.filePath("long.mp4");
    ASSERT_TRUE(runFfmpeg({"-f", "lavfi", "-i", "testsrc=size=640x360:rate=25:duration=60", "-c:v", "libx264",
                           "-pix_fmt", "yuv420p", mp4}));
    auto* queue = new JobQueue;
    QList<ConversionJob*> jobs;
    for (int i = 0; i < 3; ++i) {
        ConversionRequest req;
        req.inputPath = mp4;
        req.inputFormat = "mp4";
        req.outputFormat = "webm";
        req.outputPath = dir.filePath(QStringLiteral("out%1.webm").arg(i));
        jobs << queue->submit(req, ConversionPlanner().plan("mp4", "webm"));
    }
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 1500)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    t.restart();
    EXPECT_TRUE(queue->shutdown(15000));
    EXPECT_LT(t.elapsed(), 10000);
    delete queue;
    for (int i = 0; i < 3; ++i)
        EXPECT_FALSE(QFile::exists(dir.filePath(QStringLiteral("out%1.webm").arg(i))));
}

#include "editor/pdf_native/PdfEditSession.h"
#include "engines/ocr/TesseractEngine.h"

// Regression (found by the installer test on an English Windows): Tesseract for Windows reads its
// arguments in the ANSI code page, so OCR failed when the language data or the work folder were in a
// path with characters outside it (installation folder "Zażółć", user names like "Łukasz").
// Folvio now passes no paths at all (stdin/stdout, relative tessdata). "Ψ 日本" is outside every
// European code page, so this fails without the fix on any Windows.
TEST(Ocr, WorksWhenTheLanguageDataPathHasNonAnsiCharacters)
{
    QString why;
    if (!TesseractEngine::isAvailable(&why))
        GTEST_SKIP() << why.toStdString();
    const QString source = DependencyManager::instance().tessdataDir({"eng"});
    ASSERT_FALSE(source.isEmpty());
    QTemporaryDir dir;
    const QString unicodeDir = dir.filePath(QString::fromUtf8("OCR Ψ 日本 Zażółć"));
    ASSERT_TRUE(QDir().mkpath(unicodeDir));
    ASSERT_TRUE(QFile::copy(source + "/eng.traineddata", unicodeDir + "/eng.traineddata"));
    qputenv("FOLVIO_TESSDATA_DIR", unicodeDir.toUtf8());
    QImage img(1000, 300, QImage::Format_Grayscale8);
    img.fill(255);
    {
        QPainter p(&img);
        QFont f("Arial");
        f.setPixelSize(48);
        p.setFont(f);
        p.drawText(60, 160, "Invoice number 4711");
    }
    const OcrPageResult r = TesseractEngine::recognize(img, "eng", dir.filePath(QString::fromUtf8("work Ψ")));
    qunsetenv("FOLVIO_TESSDATA_DIR");
    ASSERT_EQ(DependencyManager::instance().tessdataDir({"eng"}), source);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.text().contains("4711")) << r.text().toStdString();
}

TEST(Ocr, ScannedPdfGetsSearchableLayerWithoutChangingTheImage)
{
    QString why;
    if (!TesseractEngine::isAvailable(&why))
        GTEST_SKIP() << why.toStdString();
    QTemporaryDir dir;
    const QString pdf = dir.filePath("scan.pdf");
    ASSERT_TRUE(makeScannedPdf(pdf, "Invoice number 4711\nTotal amount 120 PLN\nZapłacono gotówką"));
    PdfEditSession s;
    ASSERT_EQ(s.open(pdf), PdfEditSession::OpenStatus::Ok);
    ASSERT_TRUE(s.analysis().isScanned);
    const QImage before = s.original()->render(0, 1.0);

    const QImage page = s.original()->render(0, 300.0 / 72.0, false).convertToFormat(QImage::Format_Grayscale8);
    const QString langs = TesseractEngine::defaultLanguages();
    EXPECT_TRUE(langs.contains("pol")) << langs.toStdString();
    const OcrPageResult r = TesseractEngine::recognize(page, langs, dir.filePath("ocr"));
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const QString text = r.text();
    EXPECT_TRUE(text.contains("4711")) << text.toStdString();
    EXPECT_TRUE(text.contains("Invoice"));
    for (const OcrWord& w : r.words) {
        EXPECT_GE(w.confidence, 0.0);
        EXPECT_LE(w.confidence, 100.0);
    }
    // Apply as an invisible text layer.
    const QTransform toPdf = s.original()->pageToDevice(0, r.imageSize).inverted();
    EditOp op;
    op.kind = EditOp::Kind::OcrLayer;
    op.page = 0;
    for (const OcrWord& w : r.words)
        op.words.append({w.text, toPdf.mapRect(QRectF(w.box))});
    ASSERT_TRUE(s.apply(op));
    PdfEditSession::SaveOptions o;
    o.backupDir = dir.filePath("b");
    const auto res = s.save(o);
    ASSERT_TRUE(res.ok) << res.error.toStdString();
    auto saved = PdfDocument::openFile(pdf);
    ASSERT_TRUE(saved);
    EXPECT_TRUE(saved->pageText(0).contains("4711"));
    EXPECT_TRUE(saved->analyze().hasTextLayer);
    // Invisible text: the page still looks exactly like the scan.
    EXPECT_LT(pixelDifference(before, saved->render(0, 1.0)), 0.0005);
}
