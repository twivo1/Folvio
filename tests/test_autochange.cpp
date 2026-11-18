// End-to-end tests of the native PDF editing workflow (AUTO CHANGE).
#include "TestSupport.h"

#include "core/files/SafeFileWriter.h"
#include "editor/pdf_native/PdfEditSession.h"
#include "engines/pdf/PdfDocument.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace folvio;
using namespace folvio::test;

namespace {

int findBlock(PdfEditSession& s, int page, const QString& text)
{
    for (const TextBlock& b : s.blocks(page))
        if (b.text().contains(text))
            return b.id;
    return -1;
}

bool replaceBlockText(PdfEditSession& s, int page, const QString& find, const QString& replacement, QString* err = nullptr)
{
    const int id = findBlock(s, page, find);
    if (id < 0)
        return false;
    EditOp op;
    op.kind = EditOp::Kind::ReplaceBlock;
    op.page = page;
    op.blockId = id;
    QString text = s.blockState(page, id).text;
    text.replace(find, replacement);
    op.text = text;
    return s.apply(op, err);
}

QString normalized(QString s)
{
    s.remove(QRegularExpression("\\s+"));
    return s;
}

} // namespace

// The acceptance scenario from the specification, step by step.
TEST(AutoChange, EndToEndOriginalToEditedDocument)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("umowa.pdf");
    const QString backups = dir.filePath("backups");
    // 1. Create a test PDF containing "Original document" plus other content.
    ASSERT_TRUE(makeTextPdf(pdf, {{"Original document", "Second paragraph stays.", "Third line: 12345"}}));
    const QByteArray originalHash = sha256OfFile(pdf);

    {
        // 2. Open it in Auto Change.
        PdfEditSession session;
        QString err;
        ASSERT_EQ(session.open(pdf, {}, &err), PdfEditSession::OpenStatus::Ok) << err.toStdString();
        ASSERT_TRUE(session.analysis().hasTextLayer);
        // 3. Change "Original document" to "Edited document".
        ASSERT_TRUE(replaceBlockText(session, 0, "Original document", "Edited document", &err)) << err.toStdString();
        EXPECT_TRUE(session.isModified());
        // The in-memory edited document already shows the change (preview).
        EXPECT_TRUE(session.current()->pageText(0).contains("Edited document"));
        // 4. Save.
        PdfEditSession::SaveOptions o;
        o.backupDir = backups;
        const auto r = session.save(o);
        ASSERT_TRUE(r.ok) << r.error.toStdString();
        EXPECT_FALSE(session.isModified());
        // 9. Backup exists and is byte-identical to the original.
        ASSERT_FALSE(r.backupPath.isEmpty());
        EXPECT_TRUE(QFileInfo::exists(r.backupPath));
        EXPECT_EQ(sha256OfFile(r.backupPath), originalHash);
        // 5. Close the editor (session destroyed at end of scope).
    }

    // 6. Re-open the saved PDF.
    auto reopened = PdfDocument::openFile(pdf);
    ASSERT_TRUE(reopened);
    const QString text = reopened->allText();
    // 7. New text is really in the PDF.
    EXPECT_TRUE(text.contains("Edited document")) << text.toStdString();
    EXPECT_FALSE(text.contains("Original document"));
    // 8. Remaining content was not removed.
    EXPECT_TRUE(text.contains("Second paragraph stays."));
    EXPECT_TRUE(text.contains("Third line: 12345"));
    // No stray temp files next to the document.
    const QStringList leftovers = QDir(dir.path()).entryList({".~folvio-*"}, QDir::Files | QDir::Hidden);
    EXPECT_TRUE(leftovers.isEmpty());
}

TEST(AutoChange, PolishCharactersRoundTrip)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("zażółć.pdf");
    ASSERT_TRUE(makeTextPdf(pdf, {{"Umowa najmu lokalu", "Kwota: 1000 PLN"}}));
    PdfEditSession s;
    ASSERT_EQ(s.open(pdf), PdfEditSession::OpenStatus::Ok);
    QString err;
    ASSERT_TRUE(replaceBlockText(s, 0, "Umowa najmu lokalu", "Zażółć gęślą jaźń – Łódź", &err)) << err.toStdString();
    PdfEditSession::SaveOptions o;
    o.backupDir = dir.filePath("b");
    ASSERT_TRUE(s.save(o).ok);
    const QString text = pdfText(pdf);
    EXPECT_TRUE(text.contains(QString::fromUtf8("Zażółć gęślą jaźń – Łódź"))) << text.toStdString();
    EXPECT_TRUE(text.contains("Kwota: 1000 PLN"));
}

TEST(AutoChange, EditingOnePageLeavesOtherPagesPixelIdentical)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("multi.pdf");
    ASSERT_TRUE(makeTextPdf(pdf, {{"Page one heading", "Body text on page one."},
                                  {"Page two heading", "Change this sentence please.", "Unrelated text below."},
                                  {"Page three heading", "Final page content."}}));
    auto before = PdfDocument::openFile(pdf);
    ASSERT_TRUE(before);
    const double scale = 1.5;
    const QImage p0 = before->render(0, scale), p1 = before->render(1, scale), p2 = before->render(2, scale);

    PdfEditSession s;
    ASSERT_EQ(s.open(pdf), PdfEditSession::OpenStatus::Ok);
    const int id = findBlock(s, 1, "Change this sentence please.");
    ASSERT_GE(id, 0);
    const QRectF blockPdf = s.blocks(1)[id].bounds;
    ASSERT_TRUE(replaceBlockText(s, 1, "Change this sentence please.", "Changed sentence."));
    PdfEditSession::SaveOptions o;
    o.backupDir = dir.filePath("b");
    ASSERT_TRUE(s.save(o).ok);

    auto after = PdfDocument::openFile(pdf);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->pageCount(), 3);
    // Untouched pages render identically.
    EXPECT_EQ(pixelDifference(p0, after->render(0, scale)), 0.0);
    EXPECT_EQ(pixelDifference(p2, after->render(2, scale)), 0.0);
    // On the edited page, everything outside the edited paragraph is unchanged.
    const double pageH = after->pageSize(1).height();
    // Edited line may be wider or narrower; ignore the full width of its line band.
    const QRect ignore(0, int((pageH - blockPdf.bottom() - 4) * scale), p1.width(),
                       int((blockPdf.height() + 8) * scale));
    const QImage a1 = after->render(1, scale);
    EXPECT_LT(pixelDifference(p1, a1, ignore), 0.0005);
    EXPECT_GT(pixelDifference(p1, a1), 0.0); // and the edit is visible
    const QString t = after->pageText(1);
    EXPECT_TRUE(t.contains("Changed sentence."));
    EXPECT_TRUE(t.contains("Page two heading"));
    EXPECT_TRUE(t.contains("Unrelated text below."));
}

TEST(AutoChange, UndoRedoAndDelete)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("undo.pdf");
    // Blank lines in between make these two separate paragraphs.
    ASSERT_TRUE(makeTextPdf(pdf, {{"Alpha line", "", "", "", "Beta line"}}));
    PdfEditSession s;
    ASSERT_EQ(s.open(pdf), PdfEditSession::OpenStatus::Ok);
    ASSERT_TRUE(replaceBlockText(s, 0, "Alpha line", "Gamma line"));
    EXPECT_TRUE(s.current()->pageText(0).contains("Gamma line")) << s.current()->pageText(0).toStdString();
    ASSERT_TRUE(s.undo());
    EXPECT_FALSE(s.isModified());
    EXPECT_TRUE(s.current()->pageText(0).contains("Alpha line"));
    ASSERT_TRUE(s.redo());
    EXPECT_TRUE(s.current()->pageText(0).contains("Gamma line"));
    // Delete a block (empty text).
    EditOp del;
    del.page = 0;
    del.blockId = findBlock(s, 0, "Beta line");
    ASSERT_GE(del.blockId, 0);
    del.text.clear();
    ASSERT_TRUE(s.apply(del));
    EXPECT_FALSE(s.current()->pageText(0).contains("Beta line"));
    EXPECT_TRUE(s.current()->pageText(0).contains("Gamma line"));
}

TEST(AutoChange, AddTextBoxAndStyleChange)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("add.pdf");
    ASSERT_TRUE(makeTextPdf(pdf, {{"Header text"}}));
    PdfEditSession s;
    ASSERT_EQ(s.open(pdf), PdfEditSession::OpenStatus::Ok);
    EditOp add;
    add.kind = EditOp::Kind::AddText;
    add.page = 0;
    add.addedId = s.nextAddedId();
    add.text = "Approved – Zatwierdzono";
    add.style.family = "Arial";
    add.style.size = 18;
    add.style.bold = true;
    add.style.color = QColor("#C0392B");
    add.origin = QPointF(72, 500);
    ASSERT_TRUE(s.apply(add));
    // Restyle the header to 20 pt bold.
    EditOp style;
    style.page = 0;
    style.blockId = findBlock(s, 0, "Header text");
    style.text = "Header text";
    style.style = s.blockState(0, style.blockId).style;
    style.style.size = 20;
    style.style.bold = true;
    style.styleChanged = true;
    ASSERT_TRUE(s.apply(style));
    QByteArray out;
    ASSERT_TRUE(s.serialize(&out, nullptr));
    QString err;
    EXPECT_TRUE(s.verifyOutput(out, &err)) << err.toStdString();
    auto doc = PdfDocument::openData(out);
    ASSERT_TRUE(doc);
    const QString text = doc->pageText(0);
    EXPECT_TRUE(text.contains(QString::fromUtf8("Approved – Zatwierdzono")));
    bool foundLargeBold = false;
    for (const PdfTextRun& r : doc->textRuns(0))
        if (r.text.contains("Header") && std::abs(r.effectiveSize - 20) < 0.6 && r.bold)
            foundLargeBold = true;
    EXPECT_TRUE(foundLargeBold);
}

TEST(AutoChange, FailedSaveKeepsOriginalIntact)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("safe.pdf");
    ASSERT_TRUE(makeTextPdf(pdf, {{"Keep me safe"}}));
    const QByteArray hash = sha256OfFile(pdf);
    PdfEditSession s;
    ASSERT_EQ(s.open(pdf), PdfEditSession::OpenStatus::Ok);
    ASSERT_TRUE(replaceBlockText(s, 0, "Keep me safe", "Changed"));
    PdfEditSession::SaveOptions o;
    o.backupDir = dir.filePath("b");

    SafeFileWriter::setFailureInjection(SafeFileWriter::FailurePoint::BeforeSwap);
    auto r = s.save(o);
    SafeFileWriter::setFailureInjection(SafeFileWriter::FailurePoint::None);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(sha256OfFile(pdf), hash);
    EXPECT_TRUE(s.isModified());

    // Corruption after the swap is detected and the backup restored.
    SafeFileWriter::setFailureInjection(SafeFileWriter::FailurePoint::CorruptAfterSwap);
    r = s.save(o);
    SafeFileWriter::setFailureInjection(SafeFileWriter::FailurePoint::None);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.status, SafeFileWriter::Status::RestoredAfterFailure);
    EXPECT_EQ(sha256OfFile(pdf), hash);

    // A normal save afterwards works.
    r = s.save(o);
    EXPECT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(pdfText(pdf).contains("Changed"));
}

TEST(AutoChange, DetectsExternalModificationAndReadOnly)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("ext.pdf");
    ASSERT_TRUE(makeTextPdf(pdf, {{"Shared file"}}));
    PdfEditSession s;
    ASSERT_EQ(s.open(pdf), PdfEditSession::OpenStatus::Ok);
    ASSERT_TRUE(replaceBlockText(s, 0, "Shared file", "Mine"));
    // Another program rewrites the file.
    ASSERT_TRUE(makeTextPdf(pdf, {{"Someone else's version"}}));
    EXPECT_TRUE(s.hasExternalChange());
    PdfEditSession::SaveOptions o;
    o.backupDir = dir.filePath("b");
    auto r = s.save(o);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.status, SafeFileWriter::Status::ExternallyModified);
    EXPECT_TRUE(pdfText(pdf).contains("Someone else's version"));

    // Read-only files are never overwritten.
    QTemporaryDir dir2;
    const QString ro = dir2.filePath("ro.pdf");
    ASSERT_TRUE(makeTextPdf(ro, {{"Read only"}}));
    PdfEditSession s2;
    ASSERT_EQ(s2.open(ro), PdfEditSession::OpenStatus::Ok);
    ASSERT_TRUE(replaceBlockText(s2, 0, "Read only", "Hacked"));
    QFile::setPermissions(ro, QFile::ReadOwner | QFile::ReadUser);
    r = s2.save(o);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.status, SafeFileWriter::Status::ReadOnly);
    QFile::setPermissions(ro, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
    EXPECT_TRUE(pdfText(ro).contains("Read only"));
}

TEST(AutoChange, SaveAsCreatesNewFileAndLeavesOriginal)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("src.pdf");
    ASSERT_TRUE(makeTextPdf(pdf, {{"Source text"}}));
    const QByteArray hash = sha256OfFile(pdf);
    PdfEditSession s;
    ASSERT_EQ(s.open(pdf), PdfEditSession::OpenStatus::Ok);
    ASSERT_TRUE(replaceBlockText(s, 0, "Source text", "Copy text"));
    const QString copy = dir.filePath("Kopia ąę.pdf");
    auto r = s.saveAs(copy, false);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(pdfText(copy).contains("Copy text"));
    EXPECT_EQ(sha256OfFile(pdf), hash);
    // Refuses to overwrite without permission.
    r = s.saveAs(copy, false);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.status, SafeFileWriter::Status::TargetExists);
}

TEST(AutoChange, MultiLineParagraphReflowsWithinBlock)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("para.pdf");
    ASSERT_TRUE(makeTextPdf(pdf, {{"This is the first line of a paragraph", "and this is its second line here.",
                                   "", "", "Footer stays put"}}));
    PdfEditSession s;
    ASSERT_EQ(s.open(pdf), PdfEditSession::OpenStatus::Ok);
    const int id = findBlock(s, 0, "first line");
    ASSERT_GE(id, 0);
    EXPECT_EQ(s.blocks(0)[id].lineCount(), 2);
    EditOp op;
    op.page = 0;
    op.blockId = id;
    op.text = "This is the first line of a paragraph\nand now the second line is a good deal longer than before.";
    ASSERT_TRUE(s.apply(op));
    QByteArray out;
    ASSERT_TRUE(s.serialize(&out, nullptr));
    QString err;
    EXPECT_TRUE(s.verifyOutput(out, &err)) << err.toStdString();
    auto doc = PdfDocument::openData(out);
    const QString t = normalized(doc->pageText(0));
    EXPECT_TRUE(t.contains(normalized("and now the second line is a good deal longer than before.")));
    EXPECT_TRUE(t.contains("Footerstaysput"));
}

TEST(AutoChange, RecoveryRestoresUnsavedEdits)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("crash.pdf");
    ASSERT_TRUE(makeTextPdf(pdf, {{"Before crash"}}));
    {
        PdfEditSession s;
        ASSERT_EQ(s.open(pdf), PdfEditSession::OpenStatus::Ok);
        ASSERT_TRUE(replaceBlockText(s, 0, "Before crash", "Recovered edit"));
        s.writeRecovery();
        // Simulated crash: session destroyed without saving or discarding.
    }
    const auto pending = PdfEditSession::pendingRecoveries();
    auto it = std::find_if(pending.begin(), pending.end(), [&](const RecoveryInfo& i) {
        return QFileInfo(i.documentPath) == QFileInfo(pdf);
    });
    ASSERT_NE(it, pending.end());
    PdfEditSession s;
    ASSERT_EQ(s.open(pdf), PdfEditSession::OpenStatus::Ok);
    QString err;
    ASSERT_TRUE(s.restoreFromRecovery(it->recoveryFile, &err)) << err.toStdString();
    EXPECT_TRUE(s.isModified());
    EXPECT_TRUE(s.current()->pageText(0).contains("Recovered edit"));
    s.discardRecovery();
}

TEST(AutoChange, PasswordProtectedPdf)
{
    if (!havePython())
        GTEST_SKIP() << "python + pypdf not available to create an encrypted fixture";
    QTemporaryDir dir;
    const QString plain = dir.filePath("plain.pdf");
    const QString enc = dir.filePath("secret.pdf");
    ASSERT_TRUE(makeTextPdf(plain, {{"Secret contract text"}}));
    ASSERT_TRUE(runPython("import sys, pypdf\n"
                          "r = pypdf.PdfReader(sys.argv[1]); w = pypdf.PdfWriter()\n"
                          "[w.add_page(p) for p in r.pages]\n"
                          "w.encrypt(user_password='pass123', owner_password='owner', algorithm='RC4-128')\n"
                          "w.write(sys.argv[2])\n",
                          {plain, enc}));
    PdfEditSession s;
    QString err;
    EXPECT_EQ(s.open(enc, {}, &err), PdfEditSession::OpenStatus::NeedsPassword);
    EXPECT_EQ(s.open(enc, "wrong", &err), PdfEditSession::OpenStatus::NeedsPassword);
    ASSERT_EQ(s.open(enc, "pass123", &err), PdfEditSession::OpenStatus::Ok) << err.toStdString();
    EXPECT_TRUE(s.analysis().encrypted);
    ASSERT_TRUE(replaceBlockText(s, 0, "Secret contract text", "Edited secret"));
    // We must tell the user whether encryption survives the save.
    const QStringList warnings = s.preSaveWarnings();
    PdfEditSession::SaveOptions o;
    o.backupDir = dir.filePath("b");
    const auto r = s.save(o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const bool stillEncrypted = !PdfDocument::openFile(enc);
    if (stillEncrypted) {
        EXPECT_TRUE(pdfText(enc, "pass123").contains("Edited secret"));
        EXPECT_FALSE(warnings.join(' ').contains("no longer be password protected"));
    } else {
        EXPECT_TRUE(warnings.join(' ').contains("no longer be password protected"));
    }
}
