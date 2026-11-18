#include "app/SelfTest.h"

#include "combine/CombinePdfExporter.h"
#include "combine/CombineSession.h"
#include "core/conversions/ConversionRegistry.h"
#include "core/conversions/OutputValidator.h"
#include "core/deps/DependencyManager.h"
#include "core/files/FileFormat.h"
#include "core/jobs/ConversionJob.h"
#include "core/process/ProcessRunner.h"
#include "editor/pdf_native/PdfEditSession.h"
#include "engines/ocr/TesseractEngine.h"
#include "engines/pdf/PdfDocument.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <functional>

namespace folvio {

namespace {

struct Step {
    QString name;
    QString status; // PASS, FAIL, SKIP
    QString detail;
    qint64 ms = 0;
};

QString compactText(QString s)
{
    s.remove(QRegularExpression(QStringLiteral("\\s+")));
    return s;
}

bool textPdf(const QString& path, const QStringList& lines)
{
    QPdfWriter w(path);
    w.setPageSize(QPageSize(QPageSize::A4));
    w.setResolution(72);
    w.setPageMargins(QMarginsF(0, 0, 0, 0));
    QPainter p;
    if (!p.begin(&w))
        return false;
    QFont f(QStringLiteral("Arial"));
    f.setPointSizeF(14);
    p.setFont(f);
    int y = 100;
    for (const QString& l : lines) {
        p.drawText(72, y, l);
        y += 28;
    }
    return p.end();
}

ConversionResult convert(const QString& input, const QString& to, const QString& outDir)
{
    const DetectedFile d = FormatDetector::detect(input);
    ConversionRequest req;
    req.inputPath = input;
    req.inputFormat = d.formatId;
    req.outputFormat = to;
    req.outputPath = QDir(outDir).filePath(QFileInfo(input).completeBaseName() + "." + FormatRegistry::preferredExtension(to));
    const ConversionPlan plan = ConversionPlanner().plan(req.inputFormat, to);
    std::atomic_bool cancel{false};
    return ConversionExecutor::execute(req, plan, &cancel, {});
}

// --- individual checks; return empty string on success, else the failure reason ---

QString checkCombine(const QString& dir)
{
    QStringList files;
    const QColor colors[] = {QColor("#d32f2f"), QColor("#1976d2"), QColor("#388e3c")};
    for (int i = 0; i < 3; ++i) {
        QImage img(600, 840, QImage::Format_RGB32);
        img.fill(Qt::white);
        QPainter p(&img);
        p.fillRect(40, 40, 200, 120, colors[i]);
        p.setFont(QFont(QStringLiteral("Arial"), 28));
        p.drawText(40, 300, QStringLiteral("Page %1").arg(i + 1));
        p.end();
        const QString f = QDir(dir).filePath(QStringLiteral("page_%1.png").arg(i + 1));
        if (!img.save(f, "PNG"))
            return QStringLiteral("cannot write PNG");
        files << f;
    }
    const combine::ImportResult imported = combine::CombineSession::importFiles(files);
    if (imported.pages.size() != 3)
        return QStringLiteral("import: %1").arg(imported.errors.join("; "));
    QList<combine::CombinePage> pages = imported.pages;
    combine::Annotation note;
    note.type = combine::Annotation::Type::Text;
    note.text = QString::fromUtf8("Zażółć gęślą jaźń");
    note.rect = QRectF(0.1, 0.6, 0.7, 0.06);
    pages[0].annotations << note;
    const QString out = QDir(dir).filePath(QStringLiteral("combined.pdf"));
    const combine::ExportResult r = combine::CombinePdfExporter::exportTo(out, pages, combine::ExportSettings(), false, {}, nullptr);
    if (!r.ok)
        return r.error;
    auto pdf = PdfDocument::openFile(out);
    if (!pdf || pdf->pageCount() != 3)
        return QStringLiteral("exported PDF does not have 3 pages");
    if (!pdf->pageText(0).contains(note.text))
        return QStringLiteral("annotation text missing in the PDF");
    return {};
}

QString checkAutoChange(const QString& dir)
{
    const QString pdf = QDir(dir).filePath(QStringLiteral("edit.pdf"));
    if (!textPdf(pdf, {QStringLiteral("Original document"), QStringLiteral("Second paragraph stays."),
                       QStringLiteral("Third line: 12345")}))
        return QStringLiteral("cannot create the test PDF");
    PdfEditSession s;
    QString err;
    if (s.open(pdf, {}, &err) != PdfEditSession::OpenStatus::Ok)
        return QStringLiteral("open: %1").arg(err);
    int block = -1;
    for (const TextBlock& b : s.blocks(0))
        if (s.blockState(0, b.id).text.contains(QStringLiteral("Original document")))
            block = b.id;
    if (block < 0)
        return QStringLiteral("paragraph not found");
    EditOp op;
    op.page = 0;
    op.blockId = block;
    // Replace a fragment of the paragraph (the three lines form one block).
    op.text = s.blockState(0, block).text.replace(QStringLiteral("Original document"), QString::fromUtf8("Edited document – zażółć"));
    if (!s.apply(op, &err))
        return QStringLiteral("edit: %1").arg(err);
    PdfEditSession::SaveOptions o;
    o.backupDir = QDir(dir).filePath(QStringLiteral("backups"));
    const auto r = s.save(o);
    if (!r.ok)
        return QStringLiteral("save: %1").arg(r.error);
    auto saved = PdfDocument::openFile(pdf);
    const QString text = saved ? compactText(saved->pageText(0)) : QString();
    for (const QString& expected : {QString::fromUtf8("Edited document – zażółć"), QStringLiteral("Second paragraph stays."),
                                    QStringLiteral("Third line: 12345")})
        if (!text.contains(compactText(expected)))
            return QStringLiteral("text after save is wrong: %1").arg(text.left(200));
    return {};
}

QString checkImageConversion(const QString& dir)
{
    QImage img(320, 200, QImage::Format_ARGB32);
    img.fill(QColor(30, 120, 200, 180));
    const QString png = QDir(dir).filePath(QStringLiteral("picture.png"));
    img.save(png, "PNG");
    const ConversionResult r = convert(png, QStringLiteral("jpg"), dir);
    if (!r.ok)
        return r.error;
    return QImage(r.outputs.value(0)).size() == img.size() ? QString() : QStringLiteral("JPG has the wrong size");
}

QString checkMarkdown(const QString& dir)
{
    const QString md = QDir(dir).filePath(QStringLiteral("notes.md"));
    QFile f(md);
    if (!f.open(QIODevice::WriteOnly))
        return QStringLiteral("cannot write Markdown");
    f.write("# Raport\n\nZażółć **gęślą** jaźń.\n");
    f.close();
    const ConversionResult r = convert(md, QStringLiteral("pdf"), dir);
    if (!r.ok)
        return r.error;
    auto pdf = PdfDocument::openFile(r.outputs.value(0));
    return pdf && pdf->pageText(0).contains(QStringLiteral("Raport")) ? QString() : QStringLiteral("PDF text missing");
}

QString checkMedia(const QString& dir)
{
    // Test clip from FFmpeg's built-in generators (native encoders only, no GPL libraries needed).
    const QString mp4 = QDir(dir).filePath(QStringLiteral("clip.mp4"));
    ProcessSpec spec;
    spec.program = DependencyManager::instance().path(QStringLiteral("ffmpeg"));
    spec.arguments = {"-y", "-hide_banner", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=25:duration=2", "-f", "lavfi", "-i",
                      "sine=frequency=440:duration=2", "-c:v", "mpeg4", "-c:a", "aac", "-shortest", mp4};
    spec.timeoutMs = 120000;
    const ProcessResult made = ProcessRunner::run(spec);
    if (!made.succeeded() || !QFileInfo::exists(mp4))
        return QStringLiteral("could not create the test video: %1").arg(made.describe());
    const ConversionResult r = convert(mp4, QStringLiteral("mp3"), dir);
    if (!r.ok)
        return r.error;
    return QFileInfo(r.outputs.value(0)).size() > 1000 ? QString() : QStringLiteral("MP3 is empty");
}

QString checkOffice(const QString& dir)
{
    const QString html = QDir(dir).filePath(QStringLiteral("letter.html"));
    QFile f(html);
    if (!f.open(QIODevice::WriteOnly))
        return QStringLiteral("cannot write HTML");
    f.write("<html><body><h1>Raport kwartalny</h1><p>Zamówienie 4711 – zażółć gęślą jaźń.</p></body></html>");
    f.close();
    const ConversionResult docx = convert(html, QStringLiteral("docx"), dir);
    if (!docx.ok)
        return QStringLiteral("HTML -> DOCX: %1").arg(docx.error);
    const ConversionResult pdf = convert(docx.outputs.value(0), QStringLiteral("pdf"), dir);
    if (!pdf.ok)
        return QStringLiteral("DOCX -> PDF: %1").arg(pdf.error);
    auto doc = PdfDocument::openFile(pdf.outputs.value(0));
    if (!doc || !doc->pageText(0).contains(QStringLiteral("4711")))
        return QStringLiteral("DOCX -> PDF lost the text");
    // PDF -> DOCX (Word on Windows when installed, otherwise LibreOffice).
    const QString back = QDir(dir).filePath(QStringLiteral("back"));
    QDir().mkpath(back);
    const ConversionResult rebuilt = convert(pdf.outputs.value(0), QStringLiteral("docx"), back);
    if (!rebuilt.ok)
        return QStringLiteral("PDF -> DOCX: %1").arg(rebuilt.error);
    if (!OutputValidator::zipEntries(rebuilt.outputs.value(0)).contains(QStringLiteral("word/document.xml")))
        return QStringLiteral("PDF -> DOCX produced no Word document");
    return {};
}

QString checkOcr(const QString& dir)
{
    QImage img(1240, 600, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    QFont f(QStringLiteral("Arial"));
    f.setPixelSize(48);
    p.setFont(f);
    p.setPen(Qt::black);
    p.drawText(120, 200, QStringLiteral("Invoice number 4711"));
    p.drawText(120, 300, QString::fromUtf8("Zapłacono gotówką"));
    p.end();
    const OcrPageResult r = TesseractEngine::recognize(img.convertToFormat(QImage::Format_Grayscale8),
                                                       TesseractEngine::defaultLanguages(), QDir(dir).filePath("ocr"));
    if (!r.ok)
        return r.error;
    return r.text().contains(QStringLiteral("4711")) ? QString() : QStringLiteral("recognised text: %1").arg(r.text().left(120));
}

} // namespace

int runSelfTest(const QStringList& requiredEngines, const QString& reportPath)
{
#ifdef Q_OS_WIN
    // Folvio.exe is a GUI program; print the report into the console it was started from, if any.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool redirected = out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN;
    if (!redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* unused = nullptr;
        freopen_s(&unused, "CONOUT$", "w", stdout);
        SetConsoleOutputCP(CP_UTF8);
    }
#endif
    QTemporaryDir root;
    QList<Step> steps;
    auto run = [&](const QString& name, const QString& engine, const std::function<QString(const QString&)>& fn) {
        Step s;
        s.name = name;
        if (!engine.isEmpty() && !DependencyManager::instance().available(engine)) {
            s.status = requiredEngines.contains(engine) ? QStringLiteral("FAIL") : QStringLiteral("SKIP");
            s.detail = QStringLiteral("%1 is not installed").arg(engine);
            steps << s;
            return;
        }
        const QString dir = root.filePath(name);
        QDir().mkpath(dir);
        QElapsedTimer t;
        t.start();
        QString err;
        try {
            err = fn(dir);
        } catch (const std::exception& e) {
            err = QString::fromLocal8Bit(e.what());
        }
        s.ms = t.elapsed();
        s.status = err.isEmpty() ? QStringLiteral("PASS") : QStringLiteral("FAIL");
        s.detail = err;
        steps << s;
    };
    run(QStringLiteral("combine-3-png-to-pdf"), {}, checkCombine);
    run(QStringLiteral("autochange-edit-and-save"), {}, checkAutoChange);
    run(QStringLiteral("convert-png-to-jpg"), {}, checkImageConversion);
    run(QStringLiteral("convert-markdown-to-pdf"), {}, checkMarkdown);
    run(QStringLiteral("convert-mp4-to-mp3"), QStringLiteral("ffmpeg"), checkMedia);
    run(QStringLiteral("convert-html-docx-pdf-docx"), QStringLiteral("soffice"), checkOffice);
    run(QStringLiteral("ocr-pol-eng"), QStringLiteral("tesseract"), checkOcr);

    QString report;
    QTextStream ts(&report);
    ts << "Folvio " << QCoreApplication::applicationVersion() << " self-test\n";
    ts << "System: " << QSysInfo::prettyProductName() << " (" << QSysInfo::currentCpuArchitecture() << "), Qt "
       << qVersion() << ", platform " << QGuiApplication::platformName() << "\n";
    for (const ToolInfo& t : DependencyManager::instance().all())
        ts << "Engine " << t.id << ": " << (t.available ? "available " + t.version : QStringLiteral("missing")) << "\n";
    int failures = 0;
    for (const Step& s : steps) {
        ts << s.status << "  " << s.name << "  (" << s.ms << " ms)" << (s.detail.isEmpty() ? QString() : "  " + s.detail) << "\n";
        failures += s.status == QLatin1String("FAIL") ? 1 : 0;
    }
    ts << (failures ? QStringLiteral("RESULT: FAILED (%1)\n").arg(failures) : QStringLiteral("RESULT: OK\n"));
    ts.flush();
    std::fputs(report.toUtf8().constData(), stdout);
    std::fflush(stdout);
    if (!reportPath.isEmpty()) {
        QFile f(reportPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(report.toUtf8());
    }
    return failures ? 1 : 0;
}

} // namespace folvio
