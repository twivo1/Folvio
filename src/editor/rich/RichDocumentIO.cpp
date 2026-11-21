#include "editor/rich/RichDocumentIO.h"

#include "core/conversions/ConversionRegistry.h"
#include "core/deps/DependencyManager.h"
#include "engines/documents/LibreOfficeConverter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPageSize>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTextDocument>
#include <QTextDocumentWriter>
#include <QTextFrame>

namespace folvio {

QStringList RichDocumentIO::editableFormats()
{
    return {"docx", "doc", "odt", "rtf", "txt", "md", "html", "pdf"};
}

bool RichDocumentIO::canEdit(const QString& format)
{
    return editableFormats().contains(format);
}

bool RichDocumentIO::canSaveBack(const QString& format, QString* reason)
{
    if (format == "txt" || format == "md" || format == "html" || format == "pdf")
        return true;
    if (format == "docx" || format == "doc" || format == "odt" || format == "rtf") {
        if (DependencyManager::instance().available("soffice"))
            return true;
        if (reason)
            *reason = QObject::tr("Saving %1 files requires LibreOffice.").arg(format.toUpper());
        return false;
    }
    if (reason)
        *reason = QObject::tr("This format cannot be saved by the editor.");
    return false;
}

QString RichDocumentIO::fidelityWarning(const QString& format)
{
    if (format == "pdf")
        return QObject::tr("The PDF will be regenerated from the reflowed document. Page layout, fonts and spacing "
                           "will differ from the original PDF.");
    if (format == "docx" || format == "doc" || format == "odt" || format == "rtf")
        return QObject::tr("The document is rewritten from Folvio's editor. Text, paragraph and character formatting, "
                           "lists, tables and images are kept; advanced features such as tracked changes, comments, "
                           "fields, text boxes, headers/footers and section layouts may be lost.");
    if (format == "md")
        return QObject::tr("Formatting that Markdown cannot express (colours, fonts, alignment) is not saved.");
    if (format == "txt")
        return QObject::tr("Plain text keeps no formatting, images or tables.");
    return {};
}

namespace {

QString readTextFile(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return {};
    }
    const QByteArray data = f.readAll();
    auto enc = QStringConverter::encodingForData(data);
    QStringDecoder dec(enc ? *enc : QStringDecoder::Utf8);
    QString text = dec.decode(data);
    if (!enc && dec.hasError())
        text = QString::fromLocal8Bit(data);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    return text;
}

ConversionResult runPlan(const QString& in, const QString& from, const QString& to, const QString& out,
                         const QString& workDir, const std::atomic_bool* cancel)
{
    const ConversionPlan plan = ConversionPlanner().plan(from, to);
    if (!plan.isValid())
        return ConversionResult::failure(QObject::tr("No installed engine can convert %1 to %2.").arg(from.toUpper(), to.toUpper()));
    // Run the converters directly so the output stays in the work directory.
    ConversionContext ctx(workDir, {}, cancel);
    QString current = in, currentFormat = from;
    ConversionResult r;
    for (int i = 0; i < plan.steps.size(); ++i) {
        const PlanStep& s = plan.steps[i];
        ConversionRequest req;
        req.inputPath = current;
        req.inputFormat = currentFormat;
        req.outputFormat = s.capability.to;
        req.outputPath = i + 1 == plan.steps.size() ? out : workDir + "/step" + QString::number(i) + "." + s.capability.to;
        r = s.converter->convert(req, ctx);
        if (!r.ok)
            return r;
        current = req.outputPath;
        currentFormat = s.capability.to;
        r.warnings << s.converter->displayName();
    }
    return r;
}

} // namespace

RichDocumentIO::Imported RichDocumentIO::importFile(const QString& path, const QString& format, const QString& workDir,
                                                    const std::atomic_bool* cancel)
{
    Imported im;
    QDir().mkpath(workDir);
    QString err;
    if (format == "txt") {
        im.kind = Kind::PlainText;
        im.content = readTextFile(path, &err);
        im.ok = err.isEmpty();
        im.error = err;
        return im;
    }
    if (format == "md") {
        im.kind = Kind::Markdown;
        im.content = readTextFile(path, &err);
        im.baseDir = QFileInfo(path).absolutePath();
        im.ok = err.isEmpty();
        im.error = err;
        return im;
    }
    if (format == "html") {
        im.content = readTextFile(path, &err);
        im.baseDir = QFileInfo(path).absolutePath();
        im.ok = err.isEmpty();
        im.error = err;
        im.warnings << QObject::tr("CSS layout features beyond basic formatting are not supported by the editor.");
        return im;
    }

    QString source = path;
    QString sourceFormat = format;
    if (format == "pdf") {
        // Reconstruct an editable document (Word if available, otherwise LibreOffice).
        const QString docx = workDir + "/reconstructed.docx";
        const ConversionResult r = runPlan(path, "pdf", "docx", docx, workDir, cancel);
        if (!r.ok) {
            im.error = r.error;
            return im;
        }
        im.engine = r.warnings.value(0);
        source = docx;
        sourceFormat = "docx";
        im.warnings << QObject::tr("The PDF was reconstructed into an editable document by %1. This is not lossless: "
                                   "check the layout before saving.").arg(im.engine);
    }
    if (!DependencyManager::instance().available("soffice")) {
        im.error = QObject::tr("Opening %1 files in the editor requires LibreOffice.").arg(sourceFormat.toUpper());
        return im;
    }
    ConversionContext ctx(workDir, {}, cancel);
    const QString html = workDir + "/document.html";
    const ConversionResult r = LibreOfficeConverter::runConversion(source, sourceFormat, "html", html, ctx);
    if (!r.ok) {
        im.error = r.error;
        return im;
    }
    im.content = readTextFile(html, &err);
    im.baseDir = workDir;
    im.ok = err.isEmpty();
    im.error = err;
    if (im.engine.isEmpty())
        im.engine = QStringLiteral("LibreOffice");
    return im;
}

bool RichDocumentIO::writeIntermediate(QTextDocument* doc, const QString& format, const QString& path, QString* error)
{
    if (format == "pdf") {
        QPdfWriter writer(path);
        writer.setCreator(QStringLiteral("Folvio"));
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(300);
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        // The editor document is paginated (A4 at 96 dpi with page margins), so
        // print() reproduces exactly the pages shown in the editor, scaled to 300 dpi.
        doc->print(&writer);
        if (QFileInfo(path).size() <= 0) {
            *error = QObject::tr("The PDF could not be written.");
            return false;
        }
        return true;
    }
    QByteArray data;
    if (format == "odt") {
        QTextDocumentWriter w(path, "ODF");
        if (!w.write(doc)) {
            *error = QObject::tr("The document could not be serialized: %1").arg(w.device() ? w.device()->errorString() : QString());
            return false;
        }
        return true;
    }
    if (format == "html") {
        QString html = doc->toHtml();
        if (!html.contains("charset"))
            html.replace("<head>", "<head><meta charset=\"utf-8\">");
        data = html.toUtf8();
    } else if (format == "md") {
        data = doc->toMarkdown(QTextDocument::MarkdownDialectGitHub).toUtf8();
    } else if (format == "txt") {
        data = doc->toPlainText().toUtf8();
    } else {
        *error = QObject::tr("Unsupported format.");
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

bool RichDocumentIO::convertFromOdt(const QString& odtPath, const QString& targetFormat, const QString& outputPath,
                                    const QString& workDir, const std::atomic_bool* cancel, QString* error)
{
    if (targetFormat == "odt") {
        QFile::remove(outputPath);
        if (!QFile::copy(odtPath, outputPath)) {
            *error = QObject::tr("Could not write the document.");
            return false;
        }
        return true;
    }
    ConversionContext ctx(workDir, {}, cancel);
    const ConversionResult r = LibreOfficeConverter::runConversion(odtPath, "odt", targetFormat, outputPath, ctx);
    if (!r.ok)
        *error = r.error;
    return r.ok;
}

} // namespace folvio
