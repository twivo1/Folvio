#include "engines/documents/LibreOfficeConverter.h"

#include "core/deps/DependencyManager.h"
#include "core/files/FileFormat.h"
#include "core/files/PathUtils.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QUrl>

#include <array>
#include <mutex>

namespace folvio {

namespace {

const QStringList kWriterIn = {"docx", "doc", "odt", "rtf", "txt", "html"};
const QStringList kWriterOut = {"pdf", "docx", "doc", "odt", "rtf", "txt", "html"};
const QStringList kCalcIn = {"xlsx", "xls", "ods", "csv"};
const QStringList kCalcOut = {"pdf", "xlsx", "ods", "csv"};
const QStringList kImpressIn = {"pptx", "ppt", "odp"};
const QStringList kImpressOut = {"pdf", "pptx", "odp"};

QString filterFor(const QString& in, const QString& out)
{
    const bool calc = kCalcIn.contains(in);
    const bool impress = kImpressIn.contains(in);
    if (out == "pdf") {
        if (calc) return QStringLiteral("pdf:calc_pdf_Export");
        if (impress) return QStringLiteral("pdf:impress_pdf_Export");
        return QStringLiteral("pdf:writer_pdf_Export");
    }
    if (out == "docx") return QStringLiteral("docx:MS Word 2007 XML");
    if (out == "doc") return QStringLiteral("doc:MS Word 97");
    if (out == "odt") return QStringLiteral("odt:writer8");
    if (out == "rtf") return QStringLiteral("rtf:Rich Text Format");
    if (out == "txt") return QStringLiteral("txt:Text (encoded):UTF8");
    if (out == "html") return QStringLiteral("html:HTML (StarWriter):EmbedImages");
    if (out == "xlsx") return QStringLiteral("xlsx:Calc MS Excel 2007 XML");
    if (out == "ods") return QStringLiteral("ods:calc8");
    if (out == "csv") return QStringLiteral("csv:Text - txt - csv (StarCalc):44,34,76,1,,0,false,true,false,false");
    if (out == "pptx") return QStringLiteral("pptx:Impress MS PowerPoint 2007 XML");
    if (out == "odp") return QStringLiteral("odp:impress8");
    return out;
}

QString inputFilterFor(const QString& in)
{
    if (in == "pdf") return QStringLiteral("writer_pdf_import");
    if (in == "csv") return QStringLiteral("CSV:44,34,76,1");
    if (in == "html") return QStringLiteral("HTML (StarWriter)");
    if (in == "txt") return QStringLiteral("Text (encoded):UTF8");
    return {};
}

// Hardened profile: macros never execute, no first-start wizard, no update checks.
const char* kRegistry = R"(<?xml version="1.0" encoding="UTF-8"?>
<oor:items xmlns:oor="http://openoffice.org/2001/registry" xmlns:xs="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
<item oor:path="/org.openoffice.Office.Common/Security/Scripting"><prop oor:name="MacroSecurityLevel" oor:op="fuse"><value>3</value></prop></item>
<item oor:path="/org.openoffice.Office.Common/Security/Scripting"><prop oor:name="DisableMacrosExecution" oor:op="fuse"><value>true</value></prop></item>
<item oor:path="/org.openoffice.Office.Common/Security/Scripting"><prop oor:name="BlockUntrustedRefererLinks" oor:op="fuse"><value>true</value></prop></item>
<item oor:path="/org.openoffice.Office.Common/Misc"><prop oor:name="FirstRun" oor:op="fuse"><value>false</value></prop></item>
<item oor:path="/org.openoffice.Office.Common/Misc"><prop oor:name="ShowTipOfTheDay" oor:op="fuse"><value>false</value></prop></item>
<item oor:path="/org.openoffice.Office.Jobs/Jobs/org.openoffice.Office.Jobs:Job['UpdateCheck']/Arguments"><prop oor:name="AutoCheckEnabled" oor:op="fuse"><value>false</value></prop></item>
<item oor:path="/org.openoffice.Office.Common/Load"><prop oor:name="UpdateDocMode" oor:op="fuse"><value>0</value></prop></item>
</oor:items>
)";

constexpr int kProfileSlots = 2;
std::array<std::timed_mutex, kProfileSlots>& slotMutexes()
{
    static std::array<std::timed_mutex, kProfileSlots> m;
    return m;
}

QString prepareProfile(int slot)
{
    const QString dir = PathUtils::appDataDir() + QStringLiteral("/lo-profile-%1").arg(slot);
    const QString userDir = dir + "/user";
    QDir().mkpath(userDir);
    const QString reg = userDir + "/registrymodifications.xcu";
    if (!QFileInfo::exists(reg)) {
        QFile f(reg);
        if (f.open(QIODevice::WriteOnly))
            f.write(kRegistry);
    }
    return dir;
}

} // namespace

QList<ConversionCapability> LibreOfficeConverter::capabilities() const
{
    QList<ConversionCapability> caps;
    auto add = [&](const QStringList& ins, const QStringList& outs, int fidelity) {
        for (const QString& i : ins)
            for (const QString& o : outs) {
                if (i == o)
                    continue;
                ConversionCapability c{i, o, fidelity, false, {}};
                if (o == "txt") {
                    c.lossy = true;
                    c.note = QObject::tr("Formatting, images and tables are removed in plain text.");
                }
                if (o == "csv")
                    c.note = QObject::tr("CSV stores only the first sheet, without formatting.");
                if (o == "html" && i != "txt")
                    c.fidelity = 70;
                if (o == "doc" || o == "rtf")
                    c.fidelity = fidelity - 10;
                caps << c;
            }
    };
    add(kWriterIn, kWriterOut, 85);
    add(kCalcIn, kCalcOut, 85);
    add(kImpressIn, kImpressOut, 85);
    // LibreOffice imports PDF into positioned frames: editable, but not reflowed text.
    caps << ConversionCapability{"pdf", "docx", 40, false,
                                 QObject::tr("LibreOffice rebuilds PDF pages from positioned text frames; "
                                             "layout is approximated and text may not reflow.")};
    caps << ConversionCapability{"pdf", "odt", 40, false,
                                 QObject::tr("Layout is approximated (positioned text frames).")};
    return caps;
}

QList<OptionSpec> LibreOfficeConverter::options(const QString& from, const QString& to) const
{
    Q_UNUSED(from)
    Q_UNUSED(to)
    return {};
}

ConversionResult LibreOfficeConverter::convert(const ConversionRequest& request, ConversionContext& ctx)
{
    return runConversion(request.inputPath, request.inputFormat, request.outputFormat, request.outputPath, ctx,
                         request.options);
}

ConversionResult LibreOfficeConverter::runConversion(const QString& inputPath, const QString& inputFormat,
                                                     const QString& outputFormat, const QString& outputPath,
                                                     ConversionContext& ctx, const QVariantMap& options)
{
    Q_UNUSED(options)
    const QString soffice = DependencyManager::instance().path("soffice");
    if (soffice.isEmpty())
        return ConversionResult::failure(QObject::tr("LibreOffice is not installed."));

    // Work on a copy with a neutral name: protects against odd file names and
    // guarantees we know exactly which file LibreOffice will produce.
    const QString work = ctx.tempDir() + "/lo-" + QString::number(quintptr(QThread::currentThreadId()), 16)
                         + QString::number(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(work + "/out");
    const QString inCopy = work + "/document." + FormatRegistry::preferredExtension(inputFormat);
    if (!QFile::copy(inputPath, inCopy))
        return ConversionResult::failure(QObject::tr("Could not prepare the input file."));

    // Acquire a free profile slot (LibreOffice cannot share a profile concurrently).
    int slot = -1;
    while (slot < 0) {
        for (int i = 0; i < kProfileSlots && slot < 0; ++i)
            if (slotMutexes()[i].try_lock_for(std::chrono::milliseconds(50)))
                slot = i;
        if (ctx.isCancelled()) {
            ConversionResult r;
            r.cancelled = true;
            return r;
        }
    }
    std::unique_lock slotLock(slotMutexes()[slot], std::adopt_lock);
    const QString profile = prepareProfile(slot);

    ProcessSpec spec;
    spec.program = soffice;
    spec.arguments << QStringLiteral("-env:UserInstallation=") + QUrl::fromLocalFile(profile).toString()
                   << "--headless" << "--invisible" << "--nologo" << "--norestore" << "--nolockcheck"
                   << "--nodefault" << "--nofirststartwizard";
    const QString infilter = inputFilterFor(inputFormat);
    if (!infilter.isEmpty())
        spec.arguments << QStringLiteral("--infilter=") + infilter;
    spec.arguments << "--convert-to" << filterFor(inputFormat, outputFormat) << "--outdir"
                   << QDir::toNativeSeparators(work + "/out") << QDir::toNativeSeparators(inCopy);
    spec.workingDirectory = work;
    spec.timeoutMs = 5 * 60 * 1000;
    // LibreOffice reads these to avoid touching the user's real profile/temp.
    spec.environment.insert("TMP", QDir::toNativeSeparators(work));
    spec.environment.insert("TEMP", QDir::toNativeSeparators(work));
    spec.environment.insert("TMPDIR", work);

    ctx.stepProgress(0.1, QObject::tr("LibreOffice is converting…"));
    const ProcessResult pr = ctx.run(spec);
    if (pr.cancelled) {
        ConversionResult r;
        r.cancelled = true;
        return r;
    }
    const QString produced = work + "/out/document." + FormatRegistry::preferredExtension(outputFormat);
    if (!QFileInfo::exists(produced)) {
        QString why = pr.succeeded() ? QObject::tr("LibreOffice did not produce an output file.") : pr.describe();
        const QString err = QString::fromLocal8Bit(pr.stdErr).trimmed();
        if (err.contains("source file could not be loaded", Qt::CaseInsensitive))
            why = QObject::tr("LibreOffice could not load the document (damaged or unsupported).");
        return ConversionResult::failure(why);
    }
    QFile::remove(outputPath);
    if (!QFile::rename(produced, outputPath) && !QFile::copy(produced, outputPath))
        return ConversionResult::failure(QObject::tr("Could not store the converted file."));
    QDir(work).removeRecursively();
    ctx.stepProgress(1.0);
    ConversionResult r;
    r.ok = true;
    r.outputs << outputPath;
    return r;
}

} // namespace folvio
