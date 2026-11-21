#include "engines/ocr/TesseractEngine.h"

#include "core/deps/DependencyManager.h"
#include "core/process/ProcessRunner.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QUuid>

namespace folvio {

QString OcrPageResult::text() const
{
    QString out;
    int line = -1;
    for (const OcrWord& w : words) {
        if (line != -1 && w.line != line)
            out += QLatin1Char('\n');
        else if (line != -1)
            out += QLatin1Char(' ');
        out += w.text;
        line = w.line;
    }
    return out;
}

bool TesseractEngine::isAvailable(QString* reason)
{
    if (!DependencyManager::instance().available("tesseract")) {
        if (reason)
            *reason = QObject::tr("Tesseract OCR is not installed.");
        return false;
    }
    if (availableLanguages().isEmpty()) {
        if (reason)
            *reason = QObject::tr("No OCR language data was found.");
        return false;
    }
    return true;
}

QStringList TesseractEngine::availableLanguages()
{
    QStringList l = DependencyManager::instance().tesseractLanguages();
    l.removeAll("osd");
    return l;
}

QString TesseractEngine::defaultLanguages()
{
    const QStringList l = availableLanguages();
    QStringList pick;
    for (const char* lang : {"pol", "eng"})
        if (l.contains(QString::fromLatin1(lang)))
            pick << QString::fromLatin1(lang);
    if (pick.isEmpty() && !l.isEmpty())
        pick << l.first();
    return pick.join('+');
}

QList<OcrWord> TesseractEngine::parseTsv(const QByteArray& tsv)
{
    // level page_num block_num par_num line_num word_num left top width height conf text
    QList<OcrWord> words;
    const QList<QByteArray> lines = tsv.split('\n');
    for (int i = 1; i < lines.size(); ++i) {
        const QList<QByteArray> f = lines[i].split('\t');
        if (f.size() < 12 || f[0].trimmed() != "5")
            continue;
        const QString text = QString::fromUtf8(f[11]).trimmed();
        if (text.isEmpty())
            continue;
        OcrWord w;
        w.text = text;
        w.box = QRect(f[6].toInt(), f[7].toInt(), f[8].toInt(), f[9].toInt());
        w.confidence = f[10].toDouble();
        // Unique line key across blocks/paragraphs.
        w.line = f[2].toInt() * 100000 + f[3].toInt() * 1000 + f[4].toInt();
        words << w;
    }
    return words;
}

OcrPageResult TesseractEngine::recognize(const QImage& image, const QString& languages, const QString& workDir,
                                         const std::atomic_bool* cancel)
{
    OcrPageResult r;
    r.imageSize = image.size();
    QString why;
    if (!isAvailable(&why)) {
        r.error = why;
        return r;
    }
    const QStringList langs = languages.split('+', Qt::SkipEmptyParts);
    const QString tessdata = DependencyManager::instance().tessdataDir(langs);
    if (tessdata.isEmpty()) {
        r.error = QObject::tr("Language data for %1 is missing.").arg(languages);
        return r;
    }
    Q_UNUSED(workDir)
    QByteArray png;
    {
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
            r.error = QObject::tr("Could not prepare the page image.");
            return r;
        }
    }
    ProcessSpec spec;
    spec.program = DependencyManager::instance().path("tesseract");
    // No file paths on the command line: Windows builds of Tesseract read their arguments in the
    // ANSI code page, so a folder like "Zażółć" or a user name outside it would break OCR.
    // The image goes through stdin, the TSV comes back on stdout, and the language data are found
    // relative to the working directory (set through the Unicode process API).
    // TSV is requested via a variable rather than the "tsv" config file: the bundled tessdata folder
    // contains only language models, no configs/ directory.
    spec.workingDirectory = tessdata;
    spec.stdinData = png;
    spec.arguments = {"stdin", "stdout", "--tessdata-dir", ".", "-l", languages, "--psm", "3",
                      "-c", "tessedit_create_tsv=1", "-c", "tessedit_create_txt=0"};
    spec.timeoutMs = 5 * 60 * 1000;
    spec.environment.insert("OMP_THREAD_LIMIT", "2");
    const ProcessResult pr = ProcessRunner::run(spec, [cancel] { return cancel && cancel->load(); });
    if (!pr.succeeded()) {
        r.error = pr.cancelled ? QObject::tr("Cancelled.") : pr.describe();
        return r;
    }
    r.words = parseTsv(pr.stdOut);
    double sum = 0;
    for (const OcrWord& w : r.words)
        sum += w.confidence;
    r.meanConfidence = r.words.isEmpty() ? 0 : sum / r.words.size();
    r.ok = true;
    return r;
}

} // namespace folvio
