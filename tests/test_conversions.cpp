// Real conversions on real files. Each test validates the produced content,
// not just the converter's exit status.
#include "TestSupport.h"

#include "core/conversions/ConversionRegistry.h"
#include "core/conversions/OutputValidator.h"
#include "core/deps/DependencyManager.h"
#include "core/files/FileFormat.h"
#include "core/jobs/ConversionJob.h"
#include "engines/pdf/PdfDocument.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

using namespace folvio;
using namespace folvio::test;

namespace {

ConversionResult convert(const QString& input, const QString& to, const QString& outDir, QVariantMap options = {})
{
    const DetectedFile d = FormatDetector::detect(input);
    ConversionRequest req;
    req.inputPath = input;
    req.inputFormat = d.formatId;
    req.outputFormat = to;
    req.outputPath = QDir(outDir).filePath(QFileInfo(input).completeBaseName() + "." + FormatRegistry::preferredExtension(to));
    req.options = options;
    const ConversionPlan plan = ConversionPlanner().plan(req.inputFormat, to);
    std::atomic_bool cancel{false};
    return ConversionExecutor::execute(req, plan, &cancel, {});
}

#define REQUIRE_TOOL(id)                                                                                   \
    if (!DependencyManager::instance().available(id))                                                      \
    GTEST_SKIP() << id << " is not installed"

bool makeVideo(const QString& path, bool withAudio, int seconds = 2)
{
    QStringList args = {"-f", "lavfi", "-i", QString("testsrc=size=320x240:rate=25:duration=%1").arg(seconds)};
    if (withAudio)
        args << "-f" << "lavfi" << "-i" << QString("sine=frequency=440:duration=%1").arg(seconds);
    args << "-c:v" << "libx264" << "-pix_fmt" << "yuv420p";
    if (withAudio)
        args << "-c:a" << "aac" << "-shortest";
    args << path;
    return runFfmpeg(args);
}

} // namespace

TEST(Convert, PngToJpgFlattensTransparency)
{
    QTemporaryDir dir;
    const QString png = dir.filePath("logo.png");
    ASSERT_TRUE(makePng(png, 200, 120, true));
    const auto r = convert(png, "jpg", dir.path(), {{"quality", 85}});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.outputs.size(), 1);
    QImageReader reader(r.outputs.first());
    EXPECT_EQ(reader.format(), "jpeg");
    const QImage img = reader.read();
    EXPECT_EQ(img.size(), QSize(200, 120));
    EXPECT_GT(qGray(img.pixel(2, 2)), 240); // transparent corner became white, not black
}

TEST(Convert, NameConflictsNeverOverwrite)
{
    QTemporaryDir dir;
    const QString png = dir.filePath("pic.png");
    ASSERT_TRUE(makePng(png, 50, 50, false));
    const auto a = convert(png, "jpg", dir.path());
    const auto b = convert(png, "jpg", dir.path());
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_NE(a.outputs.first(), b.outputs.first());
    EXPECT_EQ(QFileInfo(b.outputs.first()).fileName(), "pic (1).jpg");
}

TEST(Convert, PolishAndLongPaths)
{
    QTemporaryDir dir;
    const QString sub = dir.filePath(QString::fromUtf8("Dokumenty źródłowe ąę/") + QString(60, QChar(u'ż')));
    ASSERT_TRUE(QDir().mkpath(sub));
    const QString png = sub + "/" + QString(80, 'n') + QString::fromUtf8("_łódź.png");
    ASSERT_TRUE(makePng(png, 40, 40, false));
    const auto r = convert(png, "webp", sub);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(OutputValidator::validate(r.outputs.first(), "webp").ok);
}

TEST(Convert, ImagesToPdfAndBack)
{
    QTemporaryDir dir;
    const QString png = dir.filePath("scan.png");
    ASSERT_TRUE(makePng(png, 600, 800, false));
    const auto r = convert(png, "pdf", dir.path());
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    auto doc = PdfDocument::openFile(r.outputs.first());
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->pageCount(), 1);
    // PDF -> PNG: multi-page produces one image per page.
    const QString multi = dir.filePath("three.pdf");
    ASSERT_TRUE(makeTextPdf(multi, {{"1"}, {"2"}, {"3"}}));
    const auto r2 = convert(multi, "png", dir.path(), {{"dpi", "96"}});
    ASSERT_TRUE(r2.ok) << r2.error.toStdString();
    EXPECT_EQ(r2.outputs.size(), 3);
    for (const QString& o : r2.outputs)
        EXPECT_TRUE(OutputValidator::validate(o, "png").ok);
}

TEST(Convert, MarkdownToHtmlAndPdf)
{
    QTemporaryDir dir;
    const QString md = dir.filePath("notes.md");
    QFile f(md);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("# Tytuł\n\nZwykły **pogrubiony** tekst.\n\n| a | b |\n|---|---|\n| 1 | 2 |\n");
    f.close();
    const auto html = convert(md, "html", dir.path());
    ASSERT_TRUE(html.ok) << html.error.toStdString();
    QFile h(html.outputs.first());
    ASSERT_TRUE(h.open(QIODevice::ReadOnly));
    const QString content = QString::fromUtf8(h.readAll());
    EXPECT_TRUE(content.contains(QString::fromUtf8("Tytuł")));
    EXPECT_TRUE(content.contains("<table"));
    const auto pdf = convert(md, "pdf", dir.path());
    ASSERT_TRUE(pdf.ok) << pdf.error.toStdString();
    EXPECT_TRUE(pdfText(pdf.outputs.first()).contains(QString::fromUtf8("pogrubiony")));
}

TEST(Convert, DocxToPdfAndPdfToDocx)
{
    REQUIRE_TOOL("soffice");
    QTemporaryDir dir;
    // Build a DOCX with headers/footers via LibreOffice from HTML is not possible;
    // instead create ODT->DOCX from HTML text first.
    const QString html = dir.filePath("source.html");
    QFile f(html);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("<html><body><h1>Raport kwartalny</h1><p>Treść z polskimi znakami: zażółć gęślą jaźń.</p>"
            "<table border=1><tr><td>A</td><td>B</td></tr><tr><td>1</td><td>2</td></tr></table></body></html>");
    f.close();
    const auto docx = convert(html, "docx", dir.path());
    ASSERT_TRUE(docx.ok) << docx.error.toStdString();
    EXPECT_TRUE(OutputValidator::zipEntries(docx.outputs.first()).contains("word/document.xml"));

    const auto pdf = convert(docx.outputs.first(), "pdf", dir.path());
    ASSERT_TRUE(pdf.ok) << pdf.error.toStdString();
    const QString text = pdfText(pdf.outputs.first());
    EXPECT_TRUE(text.contains("Raport kwartalny")) << text.toStdString();
    EXPECT_TRUE(text.contains(QString::fromUtf8("zażółć gęślą jaźń")));

    // PDF -> DOCX (Word when installed, LibreOffice otherwise) - reconstruction, check text survives.
    const QString srcPdf = dir.filePath("letter.pdf");
    ASSERT_TRUE(makeTextPdf(srcPdf, {{"Dear customer,", "Your order 4711 has shipped."}}));
    const auto back = convert(srcPdf, "docx", dir.path());
    ASSERT_TRUE(back.ok) << back.error.toStdString();
    // Verify text by converting the DOCX to TXT.
    const auto txt = convert(back.outputs.first(), "txt", dir.path());
    ASSERT_TRUE(txt.ok) << txt.error.toStdString();
    QFile t(txt.outputs.first());
    ASSERT_TRUE(t.open(QIODevice::ReadOnly));
    const QString plain = QString::fromUtf8(t.readAll());
    EXPECT_TRUE(plain.contains("4711")) << plain.toStdString();
}

TEST(Convert, XlsxCsvJsonChain)
{
    REQUIRE_TOOL("soffice");
    QTemporaryDir dir;
    const QString csv = dir.filePath("data.csv");
    QFile f(csv);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("product,qty,price\nPaper,10,4.5\nInk,2,19.99\n");
    f.close();
    const auto xlsx = convert(csv, "xlsx", dir.path());
    ASSERT_TRUE(xlsx.ok) << xlsx.error.toStdString();
    const auto json = convert(xlsx.outputs.first(), "json", dir.path());
    ASSERT_TRUE(json.ok) << json.error.toStdString();
    QFile j(json.outputs.first());
    ASSERT_TRUE(j.open(QIODevice::ReadOnly));
    const QByteArray data = j.readAll();
    EXPECT_TRUE(data.contains("\"product\": \"Ink\"")) << data.toStdString();
}

TEST(Convert, Mp4ToMp3)
{
    REQUIRE_TOOL("ffmpeg");
    QTemporaryDir dir;
    const QString mp4 = dir.filePath("clip.mp4");
    ASSERT_TRUE(makeVideo(mp4, true));
    const auto r = convert(mp4, "mp3", dir.path(), {{"audioBitrate", "128k"}});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const auto probe = OutputValidator::probeMedia(r.outputs.first());
    EXPECT_TRUE(probe.hasAudio);
    EXPECT_FALSE(probe.hasVideo);
    EXPECT_EQ(probe.audioCodec, "mp3");
    EXPECT_NEAR(probe.durationSec, 2.0, 0.3);
}

TEST(Convert, Mp4RemuxAndReencode)
{
    REQUIRE_TOOL("ffmpeg");
    QTemporaryDir dir;
    const QString mp4 = dir.filePath("clip.mp4");
    ASSERT_TRUE(makeVideo(mp4, true));
    // MP4 -> MKV: streams copied, codec unchanged.
    const auto mkv = convert(mp4, "mkv", dir.path());
    ASSERT_TRUE(mkv.ok) << mkv.error.toStdString();
    EXPECT_EQ(OutputValidator::probeMedia(mkv.outputs.first()).videoCodec, "h264");
    // MP4 -> MP4 with another codec (H.265).
    const QString out = dir.filePath("out");
    QDir().mkpath(out);
    // Same-format conversion is not offered by the planner (from == to), so re-encode MKV -> MP4 with H.265.
    const auto h265 = convert(mkv.outputs.first(), "mp4", out, {{"videoCodec", "h265"}});
    ASSERT_TRUE(h265.ok) << h265.error.toStdString();
    const auto probe = OutputValidator::probeMedia(h265.outputs.first());
    EXPECT_EQ(probe.videoCodec, "hevc");
    EXPECT_TRUE(probe.hasAudio);
}

TEST(Convert, FlacToMp3AndVideoWithoutAudio)
{
    REQUIRE_TOOL("ffmpeg");
    QTemporaryDir dir;
    const QString flac = dir.filePath("tone.flac");
    ASSERT_TRUE(runFfmpeg({"-f", "lavfi", "-i", "sine=frequency=220:duration=3", "-c:a", "flac", flac}));
    const auto mp3 = convert(flac, "mp3", dir.path());
    ASSERT_TRUE(mp3.ok) << mp3.error.toStdString();
    EXPECT_EQ(OutputValidator::probeMedia(mp3.outputs.first()).audioCodec, "mp3");
    // A video without an audio stream cannot become MP3: clear error, no output file.
    const QString silent = dir.filePath("silent.mp4");
    ASSERT_TRUE(makeVideo(silent, false));
    const auto fail = convert(silent, "mp3", dir.path());
    EXPECT_FALSE(fail.ok);
    EXPECT_TRUE(fail.error.contains("no audio", Qt::CaseInsensitive)) << fail.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(dir.filePath("silent.mp3")));
}

TEST(Convert, Mp4ToGifAndFrames)
{
    REQUIRE_TOOL("ffmpeg");
    QTemporaryDir dir;
    const QString mp4 = dir.filePath("clip.mp4");
    ASSERT_TRUE(makeVideo(mp4, true, 3));
    const auto gif = convert(mp4, "gif", dir.path(), {{"gifWidth", "160"}});
    ASSERT_TRUE(gif.ok) << gif.error.toStdString();
    QImageReader reader(gif.outputs.first());
    EXPECT_GT(reader.imageCount(), 10);
    EXPECT_EQ(reader.size().width(), 160);
    const auto frames = convert(mp4, "png", dir.path(), {{"frameRate", "1"}});
    ASSERT_TRUE(frames.ok) << frames.error.toStdString();
    EXPECT_GE(frames.outputs.size(), 3);
    EXPECT_TRUE(QFileInfo(frames.outputs.first()).absolutePath().endsWith("clip (PNG)"));
}

TEST(Convert, CancelStopsJobAndLeavesNoOutput)
{
    REQUIRE_TOOL("ffmpeg");
    QTemporaryDir dir;
    const QString mp4 = dir.filePath("long.mp4");
    ASSERT_TRUE(makeVideo(mp4, true, 60));
    JobQueue queue;
    ConversionRequest req;
    req.inputPath = mp4;
    req.inputFormat = "mp4";
    req.outputFormat = "webm";
    req.outputPath = dir.filePath("long.webm");
    req.options = {{"quality", "high"}};
    ConversionJob* job = queue.submit(req, ConversionPlanner().plan("mp4", "webm"));
    QElapsedTimer t;
    t.start();
    while (job->state() != ConversionJob::State::Running && t.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(800);
    job->cancel();
    while (!job->isFinished() && t.elapsed() < 30000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    EXPECT_EQ(job->state(), ConversionJob::State::Cancelled);
    EXPECT_FALSE(QFileInfo::exists(dir.filePath("long.webm")));
}

TEST(Convert, HeicAndAvif)
{
    REQUIRE_TOOL("ffmpeg");
    QTemporaryDir dir;
    const QString png = dir.filePath("photo.png");
    ASSERT_TRUE(makePng(png, 256, 128, false));
    const auto avif = convert(png, "avif", dir.path());
    ASSERT_TRUE(avif.ok) << avif.error.toStdString();
    EXPECT_EQ(FormatDetector::detect(avif.outputs.first()).formatId, "avif");
    const auto back = convert(avif.outputs.first(), "jpg", dir.path());
    ASSERT_TRUE(back.ok) << back.error.toStdString();
    EXPECT_EQ(QImageReader(back.outputs.first()).size(), QSize(256, 128));
}

TEST(Convert, RepeatedImportIsStable)
{
    QTemporaryDir dir;
    const QString png = dir.filePath("again.png");
    ASSERT_TRUE(makePng(png, 64, 64, true));
    for (int i = 0; i < 15; ++i) {
        const DetectedFile d = FormatDetector::detect(png);
        ASSERT_TRUE(d.ok());
        const auto r = convert(png, "bmp", dir.path());
        ASSERT_TRUE(r.ok) << r.error.toStdString();
    }
    EXPECT_EQ(QDir(dir.path()).entryList({"again*.bmp"}).size(), 15);
}
