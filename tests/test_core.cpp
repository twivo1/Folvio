#include "TestSupport.h"

#include "core/conversions/ConversionRegistry.h"
#include "core/conversions/OutputValidator.h"
#include "core/deps/DependencyManager.h"
#include "core/files/FileFormat.h"
#include "core/files/PathUtils.h"
#include "core/files/SafeFileWriter.h"
#include "core/process/ProcessRunner.h"
#include "engines/data/TextDataConverter.h"
#include "engines/media/FfmpegConverter.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <QTest>

#include <gtest/gtest.h>

using namespace folvio;

TEST(FormatDetector, SniffsMagicBytes)
{
    EXPECT_EQ(FormatDetector::sniff("%PDF-1.7\n", "pdf"), "pdf");
    EXPECT_EQ(FormatDetector::sniff(QByteArray("\x89PNG\r\n\x1A\n0000", 12), ""), "png");
    EXPECT_EQ(FormatDetector::sniff(QByteArray("\xFF\xD8\xFF\xE0", 4), "png"), "jpg"); // content beats extension
    EXPECT_EQ(FormatDetector::sniff(QByteArray("RIFF\0\0\0\0WEBPVP8 ", 16), ""), "webp");
    EXPECT_EQ(FormatDetector::sniff(QByteArray("RIFF\0\0\0\0WAVEfmt ", 16), ""), "wav");
    EXPECT_EQ(FormatDetector::sniff(QByteArray("\0\0\0\x18" "ftypheic\0\0\0\0mif1heic", 28), ""), "heic");
    EXPECT_EQ(FormatDetector::sniff(QByteArray("\0\0\0\x1C" "ftypavif\0\0\0\0avifmif1", 28), ""), "avif");
    EXPECT_EQ(FormatDetector::sniff(QByteArray("\0\0\0\x20" "ftypisom\0\0\x02\0isomiso2", 28), ""), "mp4");
    EXPECT_EQ(FormatDetector::sniff(QByteArray("PK\x03\x04....word/document.xml", 30), "zip"), "docx");
    EXPECT_EQ(FormatDetector::sniff("fLaC\0\0", ""), "flac");
    EXPECT_EQ(FormatDetector::sniff("{\\rtf1\\ansi", ""), "rtf");
    EXPECT_EQ(FormatDetector::sniff("a,b\n1,2\n", "csv"), "csv");
    EXPECT_EQ(FormatDetector::sniff("<?xml version=\"1.0\"?><svg xmlns=", ""), "svg");
}

TEST(FormatDetector, DetectsMismatchAndEmptyFiles)
{
    QTemporaryDir dir;
    const QString fake = dir.filePath("photo.jpg");
    ASSERT_TRUE(test::makePng(fake, 10, 10, false));
    const DetectedFile d = FormatDetector::detect(fake);
    EXPECT_EQ(d.formatId, "png");
    EXPECT_FALSE(d.contentMatchesExtension);
    QFile empty(dir.filePath("empty.pdf"));
    ASSERT_TRUE(empty.open(QIODevice::WriteOnly));
    empty.close();
    EXPECT_FALSE(FormatDetector::detect(empty.fileName()).ok());
    EXPECT_FALSE(FormatDetector::detect(dir.filePath("missing.pdf")).ok());
}

TEST(PathUtils, SanitizesAndAvoidsConflicts)
{
    EXPECT_EQ(PathUtils::sanitizeFileName("a<b>c:d\"e/f\\g|h?i*j"), "a_b_c_d_e_f_g_h_i_j");
    EXPECT_EQ(PathUtils::sanitizeFileName("CON"), "_CON");
    EXPECT_EQ(PathUtils::sanitizeFileName("name. . "), "name");
    EXPECT_EQ(PathUtils::sanitizeFileName(""), "output");
    EXPECT_LE(PathUtils::sanitizeFileName(QString(400, 'x')).size(), 180);
    QTemporaryDir dir;
    const QString p = dir.filePath("raport.pdf");
    EXPECT_EQ(PathUtils::uniquePath(p), p);
    QFile(p).open(QIODevice::WriteOnly);
    EXPECT_EQ(QFileInfo(PathUtils::uniquePath(p)).fileName(), "raport (1).pdf");
    QFile(dir.filePath("raport (1).pdf")).open(QIODevice::WriteOnly);
    EXPECT_EQ(QFileInfo(PathUtils::uniquePath(p)).fileName(), "raport (2).pdf");
}

TEST(SafeFileWriter, ValidationFailureLeavesTargetUntouched)
{
    QTemporaryDir dir;
    const QString target = dir.filePath("data.txt");
    {
        QFile f(target);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("original");
    }
    const auto r = SafeFileWriter::replace(
        target,
        [](const QString& tmp, QString*) {
            QFile f(tmp);
            f.open(QIODevice::WriteOnly);
            f.write("broken");
            return true;
        },
        [](const QString&, QString* e) {
            *e = "nope";
            return false;
        });
    EXPECT_EQ(r.status, SafeFileWriter::Status::ValidationFailed);
    QFile f(target);
    f.open(QIODevice::ReadOnly);
    EXPECT_EQ(f.readAll(), "original");
    EXPECT_TRUE(QDir(dir.path()).entryList({".~folvio-*"}, QDir::Files | QDir::Hidden).isEmpty());
}

TEST(ProcessRunner, ArgumentsAreNotInterpretedByAShell)
{
    // A path containing shell metacharacters must arrive verbatim.
    QTemporaryDir dir;
    const QString weird = dir.filePath("a & echo HACKED > pwned.txt.bin");
    ProcessSpec spec;
#ifdef Q_OS_WIN
    spec.program = QDir::fromNativeSeparators(qEnvironmentVariable("ComSpec", "C:/Windows/System32/cmd.exe"));
    spec.arguments = {"/c", "if", "exist", QDir::toNativeSeparators(weird), "(echo yes)", "else", "(echo no)"};
#else
    spec.program = QStringLiteral("/bin/sh");
    spec.arguments = {"-c", "test -e \"$1\" && echo yes || echo no", "sh", weird};
#endif
    spec.workingDirectory = dir.path();
    const ProcessResult r = ProcessRunner::run(spec);
    EXPECT_TRUE(r.started);
#ifndef Q_OS_WIN
    EXPECT_TRUE(r.stdOut.contains("no")); // the argument arrived as one literal path
#endif
    EXPECT_FALSE(QFileInfo::exists(dir.filePath("pwned.txt")));
}

TEST(ProcessRunner, TimeoutAndCancelKillProcessTree)
{
    ProcessSpec spec;
#ifdef Q_OS_WIN
    spec.program = qEnvironmentVariable("SystemRoot", "C:/Windows") + "/System32/WindowsPowerShell/v1.0/powershell.exe";
    spec.arguments = {"-NoProfile", "-Command", "Start-Sleep -Seconds 30"};
#else
    // The shell starts a grandchild; cancelling must kill it as well.
    spec.program = QStringLiteral("/bin/sh");
    spec.arguments = {"-c", "sleep 30 & echo $!; wait"};
#endif
    spec.timeoutMs = 1500;
    QElapsedTimer t;
    t.start();
    ProcessResult r = ProcessRunner::run(spec);
    EXPECT_TRUE(r.timedOut);
    EXPECT_LT(t.elapsed(), 10000);
#ifndef Q_OS_WIN
    const qint64 grandchild = r.stdOut.trimmed().toLongLong();
    ASSERT_GT(grandchild, 0);
    QTest::qWait(200);
    // Gone (or at most a zombie waiting for init to reap it).
    QFile stat(QStringLiteral("/proc/%1/stat").arg(grandchild));
    if (stat.open(QIODevice::ReadOnly))
        EXPECT_TRUE(stat.readAll().contains(") Z ")) << "grandchild still running";
#endif
    spec.timeoutMs = 60000;
    t.restart();
    r = ProcessRunner::run(spec, [&] { return t.elapsed() > 800; });
    EXPECT_TRUE(r.cancelled);
    EXPECT_LT(t.elapsed(), 10000);
}

TEST(TextData, CsvParsingAndJsonRoundTrip)
{
    const QString csv = "name;city;amount;code\r\n\"Kowalski, Jan\";Łódź;12.5;007\r\n\"Quote \"\"x\"\"\";Kraków;3;010\r\n";
    EXPECT_EQ(TextDataConverter::detectDelimiter(csv), QChar(';'));
    const auto rows = TextDataConverter::parseCsv(csv);
    ASSERT_EQ(rows.size(), 3);
    EXPECT_EQ(rows[1][0], "Kowalski, Jan");
    EXPECT_EQ(rows[2][0], "Quote \"x\"");
    QByteArray json;
    QString err;
    ASSERT_TRUE(TextDataConverter::csvToJson(csv, true, true, QChar(), &json, &err)) << err.toStdString();
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    ASSERT_EQ(arr.size(), 2);
    EXPECT_EQ(arr[0].toObject().value("city").toString(), QString::fromUtf8("Łódź"));
    EXPECT_DOUBLE_EQ(arr[0].toObject().value("amount").toDouble(), 12.5);
    EXPECT_EQ(arr[0].toObject().value("code").toString(), "007"); // leading zeros preserved
    EXPECT_TRUE(json.indexOf("\"name\"") < json.indexOf("\"city\"")); // column order kept
    QString back;
    ASSERT_TRUE(TextDataConverter::jsonToCsv(json, ',', &back, &err)) << err.toStdString();
    EXPECT_TRUE(back.startsWith("name,city,amount,code"));
    EXPECT_TRUE(back.contains("\"Kowalski, Jan\""));
}

TEST(TextData, RejectsInconsistentOrNestedData)
{
    QByteArray json;
    QString err;
    EXPECT_FALSE(TextDataConverter::csvToJson("a,b\n1,2,3\n", true, true, ',', &json, &err));
    EXPECT_TRUE(err.contains("Row 2"));
    QString csv;
    EXPECT_FALSE(TextDataConverter::jsonToCsv(R"([{"a":1,"b":{"x":2}}])", ',', &csv, &err));
    EXPECT_FALSE(TextDataConverter::jsonToCsv(R"({"not":"a table"})", ',', &csv, &err));
    EXPECT_FALSE(TextDataConverter::jsonToCsv("{broken", ',', &csv, &err));
}

TEST(Planner, OffersOnlyRealRoutes)
{
    ConversionPlanner planner;
    // Direct routes.
    EXPECT_TRUE(planner.plan("png", "jpg").isDirect());
    EXPECT_TRUE(planner.plan("csv", "json").isDirect());
    // No nonsense routes.
    EXPECT_FALSE(planner.plan("mp3", "pdf").isValid());
    EXPECT_FALSE(planner.plan("pdf", "mp4").isValid());
    EXPECT_FALSE(planner.plan("jpg", "docx").isValid());
    // Lossy audio is never chained through another lossy codec.
    const ConversionPlan p = planner.plan("ogg", "mp3");
    if (p.isValid())
        EXPECT_TRUE(p.isDirect());
    // PDF targets offered: only things that really work.
    QStringList targets;
    for (const auto& plan : planner.availableTargets("pdf"))
        targets << plan.to;
    EXPECT_TRUE(targets.contains("png"));
    EXPECT_FALSE(targets.contains("mp3"));
}

TEST(Planner, UsesIntermediateFormatOnlyWhenSensible)
{
    ConversionPlanner planner;
    const ConversionPlan p = planner.plan("pptx", "png");
    if (!DependencyManager::instance().available("soffice"))
        GTEST_SKIP() << "LibreOffice not installed";
    ASSERT_TRUE(p.isValid());
    ASSERT_EQ(p.steps.size(), 2);
    EXPECT_EQ(p.steps[0].capability.to, "pdf");
}

TEST(Planner, MissingDependencyHidesRoutes)
{
    // A converter whose required tool is unavailable must not produce routes.
    class Fake : public IConverter {
    public:
        QString id() const override { return "fake"; }
        QString displayName() const override { return "Fake"; }
        QList<ConversionCapability> capabilities() const override { return {{"txt", "avif", 99, false, {}}}; }
        QStringList requiredTools() const override { return {"nonexistent-tool"}; }
        ConversionResult convert(const ConversionRequest&, ConversionContext&) override { return {}; }
    };
    ConversionRegistry reg;
    reg.add(std::make_unique<Fake>());
    ConversionPlanner planner(reg);
    EXPECT_FALSE(planner.plan("txt", "avif").isValid());
    QString why;
    EXPECT_FALSE(reg.byId("fake")->isAvailable(&why));
}

TEST(OutputValidator, DetectsTruncatedAndBogusOutputs)
{
    QTemporaryDir dir;
    const QString pdf = dir.filePath("ok.pdf");
    ASSERT_TRUE(test::makeTextPdf(pdf, {{"x"}}));
    EXPECT_TRUE(OutputValidator::validate(pdf, "pdf").ok);
    QFile f(pdf);
    ASSERT_TRUE(f.open(QIODevice::ReadWrite));
    const QByteArray all = f.readAll();
    f.resize(0);
    f.write(all.left(all.size() / 3));
    f.close();
    EXPECT_FALSE(OutputValidator::validate(pdf, "pdf").ok);

    const QString png = dir.filePath("x.png");
    QFile p(png);
    ASSERT_TRUE(p.open(QIODevice::WriteOnly));
    p.write("\x89PNG\r\n\x1A\ngarbage");
    p.close();
    EXPECT_FALSE(OutputValidator::validate(png, "png").ok);

    const QString docx = dir.filePath("x.docx");
    QFile d(docx);
    ASSERT_TRUE(d.open(QIODevice::WriteOnly));
    d.write("PK\x03\x04 not really a zip");
    d.close();
    EXPECT_FALSE(OutputValidator::validate(docx, "docx").ok);
}

TEST(Ffmpeg, ContainerCompatibilityRules)
{
    EXPECT_TRUE(FfmpegConverter::containerAccepts("mkv", "h264", "aac"));
    EXPECT_TRUE(FfmpegConverter::containerAccepts("mp4", "h264", "aac"));
    EXPECT_FALSE(FfmpegConverter::containerAccepts("mp4", "vp8", "vorbis"));
    EXPECT_FALSE(FfmpegConverter::containerAccepts("webm", "h264", "aac"));
    EXPECT_TRUE(FfmpegConverter::containerAccepts("webm", "vp9", "opus"));
}
