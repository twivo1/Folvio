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
