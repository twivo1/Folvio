#include "engines/documents/WordConverter.h"

#include "core/deps/DependencyManager.h"
#include "core/files/FileFormat.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace folvio {

QList<ConversionCapability> WordConverter::capabilities() const
{
    QList<ConversionCapability> caps;
    caps << ConversionCapability{"pdf", "docx", 70, false,
                                 QObject::tr("PDF is reconstructed into an editable document by Microsoft Word. "
                                             "This is not lossless: complex layouts, fonts and spacing may change.")};
    for (const QString& in : {"docx", "doc", "rtf", "odt"})
        caps << ConversionCapability{in, "pdf", 95, false, {}};
    caps << ConversionCapability{"doc", "docx", 92, false, {}};
    caps << ConversionCapability{"rtf", "docx", 90, false, {}};
    return caps;
}

ConversionResult WordConverter::convert(const ConversionRequest& request, ConversionContext& ctx)
{
    return runConversion(request.inputPath, request.inputFormat, request.outputFormat, request.outputPath, ctx);
}

ConversionResult WordConverter::runConversion(const QString& inputPath, const QString& inputFormat,
                                              const QString& outputFormat, const QString& outputPath,
                                              ConversionContext& ctx)
{
    const ToolInfo word = DependencyManager::instance().tool("word");
    if (!word.available)
        return ConversionResult::failure(QObject::tr("Microsoft Word is not available."));
    const QString script = QCoreApplication::applicationDirPath() + "/engines/office/word_convert.ps1";
    if (!QFileInfo::exists(script))
        return ConversionResult::failure(QObject::tr("Word helper script is missing."));

    const QString work = ctx.tempDir() + "/word-" + QString::number(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(work);
    const QString inCopy = work + "/document." + FormatRegistry::preferredExtension(inputFormat);
    const QString outTmp = work + "/result." + FormatRegistry::preferredExtension(outputFormat);
    if (!QFile::copy(inputPath, inCopy))
        return ConversionResult::failure(QObject::tr("Could not prepare the input file."));

    ProcessSpec spec;
    spec.program = word.path;
    spec.arguments = {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                      QDir::toNativeSeparators(script), "-InputPath", QDir::toNativeSeparators(inCopy),
                      "-OutputPath", QDir::toNativeSeparators(outTmp), "-Format", outputFormat};
    spec.timeoutMs = 4 * 60 * 1000;
    ctx.stepProgress(0.1, QObject::tr("Microsoft Word is converting…"));
    const ProcessResult pr = ctx.run(spec);
    if (pr.cancelled) {
        ConversionResult r;
        r.cancelled = true;
        return r;
    }
    if (!QFileInfo::exists(outTmp) || QFileInfo(outTmp).size() == 0) {
        const QString err = QString::fromLocal8Bit(pr.stdErr).trimmed();
        return ConversionResult::failure(err.isEmpty() ? pr.describe() : QObject::tr("Word: %1").arg(err));
    }
    QFile::remove(outputPath);
    if (!QFile::rename(outTmp, outputPath))
        return ConversionResult::failure(QObject::tr("Could not store the converted file."));
    QDir(work).removeRecursively();
    ConversionResult r;
    r.ok = true;
    r.outputs << outputPath;
    return r;
}

} // namespace folvio
