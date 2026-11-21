// Handwriting recognition (PP-OCRv6 small via ONNX Runtime).
// - Always: the dictionary reader, behaviour without models.
// - With models (FOLVIO_HANDWRITING_MODELS or the installed module): synthetic Polish/English
//   samples, and - when FOLVIO_HANDWRITING_SAMPLES points to the GNHK evaluation pages
//   (real handwriting, CC-BY-4.0, not stored in the repository) - parity with the reference
//   prototype and CER against the human transcription. See docs/HANDWRITING_OCR.md.
#include "TestSupport.h"

#include "engines/ocr/HandwritingOcr.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImageReader>
#include <QPainter>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <iostream>

using namespace folvio;

namespace {

int levenshtein(const QString& a, const QString& b)
{
    std::vector<int> prev(size_t(b.size()) + 1), cur(size_t(b.size()) + 1);
    for (int j = 0; j <= b.size(); ++j)
        prev[size_t(j)] = j;
    for (int i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (int j = 1; j <= b.size(); ++j)
            cur[size_t(j)] = std::min({prev[size_t(j)] + 1, cur[size_t(j - 1)] + 1, prev[size_t(j - 1)] + (a[i - 1] != b[j - 1] ? 1 : 0)});
        std::swap(prev, cur);
    }
    return prev[size_t(b.size())];
}

double cer(const QString& truth, const QString& hyp)
{
    const QString t = truth.simplified(), h = hyp.simplified();
    return t.isEmpty() ? (h.isEmpty() ? 0.0 : 1.0) : double(levenshtein(t, h)) / t.size();
}

bool modelsReady()
{
    QString reason;
    if (!HandwritingOcr::isAvailable(&reason)) {
        std::cout << "[  SKIPPED ] handwriting models/runtime not available: " << reason.toStdString() << "\n";
        return false;
    }
    return true;
}

QImage textImage(const QStringList& lines, int px = 46)
{
    QFont f("Arial");
    f.setPixelSize(px);
    const QFontMetrics fm(f);
    int w = 0;
    for (const QString& l : lines)
        w = std::max(w, fm.horizontalAdvance(l));
    QImage img(w + 80, int(lines.size()) * fm.lineSpacing() * 2 + 60, QImage::Format_RGB32);
    img.fill(QColor(250, 249, 245));
    QPainter p(&img);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(f);
    p.setPen(QColor(25, 35, 90));
    for (int i = 0; i < lines.size(); ++i)
        p.drawText(40, 40 + fm.ascent() + i * fm.lineSpacing() * 2, lines[i]);
    return img;
}

} // namespace

TEST(HandwritingOcr, ReadsPaddleCharacterDictionaries)
{
    const QByteArray yml = "Global:\n  model_name: x\nPostProcess:\n  name: CTCLabelDecode\n  character_dict:\n  - a\n"
                           "  - '#'\n  - ''''\n  - \"\\\"\"\n  - \"\\u0105\"\n  - ł\n  - ' '\n  - '0'\nPreProcess:\n  x: 1\n";
    const QStringList d = HandwritingOcr::parseCharacterDict(yml);
    EXPECT_EQ(d, (QStringList{"a", "#", "'", "\"", QString::fromUtf8("ą"), QString::fromUtf8("ł"), " ", "0"}));
    // Models without PostProcess.character_dict give nothing (and are refused when loading).
    EXPECT_TRUE(HandwritingOcr::parseCharacterDict("Global:\n  a: 1\n").isEmpty());
}

TEST(HandwritingOcr, ModelManifestIsPinnedAndUnavailableWithoutModels)
{
    // Every model file is fetched from a fixed commit and verified with SHA-256.
    for (const auto& f : HandwritingOcr::modelFiles()) {
        EXPECT_TRUE(f.url.startsWith("https://huggingface.co/PaddlePaddle/")) << f.url.toStdString();
        EXPECT_TRUE(f.url.contains(QRegularExpression("/resolve/[0-9a-f]{40}/"))) << f.url.toStdString();
        EXPECT_EQ(f.sha256.size(), 64);
        EXPECT_GT(f.size, 0);
    }
    EXPECT_LT(HandwritingOcr::downloadSize(), 40 * 1024 * 1024);

    const QByteArray saved = qgetenv("FOLVIO_HANDWRITING_MODELS");
    QTemporaryDir empty;
    qputenv("FOLVIO_HANDWRITING_MODELS", empty.path().toUtf8());
    EXPECT_FALSE(HandwritingOcr::modelsInstalled());
    QString reason;
    EXPECT_FALSE(HandwritingOcr::isAvailable(&reason));
    EXPECT_FALSE(reason.isEmpty());
    const RecognitionResult r = HandwritingOcr::recognize(textImage({"abc"}));
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
    if (saved.isEmpty())
        qunsetenv("FOLVIO_HANDWRITING_MODELS");
    else
        qputenv("FOLVIO_HANDWRITING_MODELS", saved);
}

TEST(HandwritingOcr, InstalledModelsMatchTheManifestAndDictionary)
{
    if (!modelsReady())
        GTEST_SKIP();
    for (const auto& f : HandwritingOcr::modelFiles()) {
        QFile file(HandwritingOcr::modelDir() + "/" + f.name);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        QCryptographicHash h(QCryptographicHash::Sha256);
        h.addData(&file);
        EXPECT_EQ(h.result().toHex(), f.sha256) << f.name.toStdString();
    }
    QFile yml(HandwritingOcr::modelDir() + "/rec.yml");
    ASSERT_TRUE(yml.open(QIODevice::ReadOnly));
    const QStringList d = HandwritingOcr::parseCharacterDict(yml.readAll());
    // Same list as PaddleOCR's own YAML reader (count and SHA-256 of the '\n'-joined list).
    EXPECT_EQ(d.size(), 18708);
    EXPECT_EQ(QCryptographicHash::hash(d.join('\n').toUtf8(), QCryptographicHash::Sha256).toHex(),
              QByteArray("f7aa897ca828a4c7c9e2739c30f9161a33306d532f020bcdb91dcfb664a5507e"));
    for (const QChar c : QString::fromUtf8("ąćęłńóśźżĄĆĘŁŃÓŚŹŻ"))
        EXPECT_TRUE(d.contains(QString(c))) << QString(c).toStdString();
}

TEST(HandwritingOcr, SyntheticPolishAndEnglishLines)
{
    if (!modelsReady())
        GTEST_SKIP();
    const QStringList truth = {QString::fromUtf8("Zażółć gęślą jaźń"), QString::fromUtf8("Notatki z wykładu: całki i pochodne"),
                               QStringLiteral("Meeting notes 2026: budget and plan")};
    const RecognitionResult r = HandwritingOcr::recognize(textImage(truth));
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.lines.size(), truth.size()) << r.text().toStdString();
    for (int i = 0; i < truth.size(); ++i) {
        EXPECT_LT(cer(truth[i], r.lines[i].text), 0.08) << r.lines[i].text.toStdString();
        EXPECT_GT(r.lines[i].confidence, 0.5);
    }
    EXPECT_TRUE(r.text().contains(QString::fromUtf8("ł"))) << r.text().toStdString();
    EXPECT_EQ(r.engine, "handwriting");
}

TEST(HandwritingOcr, RealHandwritingMatchesReferencePipeline)
{
    const QString dir = qEnvironmentVariable("FOLVIO_HANDWRITING_SAMPLES");
    if (dir.isEmpty() || !QFile::exists(dir + "/ref.json"))
        GTEST_SKIP() << "FOLVIO_HANDWRITING_SAMPLES (GNHK evaluation pages) not set";
    if (!modelsReady())
        GTEST_SKIP();
    QFile f(dir + "/ref.json");
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QJsonObject ref = QJsonDocument::fromJson(f.readAll()).object();
    ASSERT_FALSE(ref.isEmpty());
    double sumCer = 0;
    for (auto it = ref.begin(); it != ref.end(); ++it) {
        QImageReader reader(dir + "/" + it.key());
        reader.setAutoTransform(true); // camera photos: EXIF orientation, as the app loads them
        const QImage page = reader.read();
        ASSERT_FALSE(page.isNull()) << it.key().toStdString();
        const RecognitionResult r = HandwritingOcr::recognize(page);
        ASSERT_TRUE(r.ok) << r.error.toStdString();
        const QString proto = it.value().toObject().value("prototype").toString();
        const QString gt = it.value().toObject().value("gt").toString();
        // Same models and post-processing as the Python reference: nearly identical text.
        EXPECT_LT(cer(proto, r.text()), 0.03) << it.key().toStdString() << "\n" << r.text().toStdString();
        const double c = cer(gt, r.text());
        sumCer += c;
        std::cout << "  " << it.key().toStdString() << "  CER vs human transcription: " << c << "\n";
    }
    EXPECT_LT(sumCer / ref.size(), 0.35);
}

// Real download through the app's downloader (Hugging Face redirects, TLS, SHA-256), opt-in:
// FOLVIO_NETWORK_TESTS=1. Only the public model files are fetched; nothing is uploaded.
TEST(HandwritingOcr, DownloadsAndVerifiesTheModels)
{
    if (qEnvironmentVariable("FOLVIO_NETWORK_TESTS") != "1")
        GTEST_SKIP() << "network tests are opt-in (FOLVIO_NETWORK_TESTS=1)";
    const QByteArray saved = qgetenv("FOLVIO_HANDWRITING_MODELS");
    QTemporaryDir dir;
    qputenv("FOLVIO_HANDWRITING_MODELS", dir.path().toUtf8());
    HandwritingModelDownloader d;
    QSignalSpy done(&d, &HandwritingModelDownloader::finished);
    QSignalSpy progress(&d, &HandwritingModelDownloader::progress);
    d.start();
    ASSERT_TRUE(done.wait(600000));
    EXPECT_TRUE(done.first().at(0).toBool()) << done.first().at(1).toString().toStdString();
    EXPECT_TRUE(HandwritingOcr::modelsInstalled());
    EXPECT_GT(progress.size(), 3);
    EXPECT_TRUE(QDir(dir.path()).entryList({"*.part"}, QDir::Files).isEmpty());
    // A damaged file is detected and fetched again.
    {
        QFile f(dir.path() + "/rec.yml");
        ASSERT_TRUE(f.open(QIODevice::ReadWrite));
        f.seek(10);
        f.write("X");
    }
    QSignalSpy again(&d, &HandwritingModelDownloader::finished);
    d.start();
    ASSERT_TRUE(again.wait(600000));
    EXPECT_TRUE(again.first().at(0).toBool());
    QFile yml(dir.path() + "/rec.yml");
    ASSERT_TRUE(yml.open(QIODevice::ReadOnly));
    EXPECT_EQ(QCryptographicHash::hash(yml.readAll(), QCryptographicHash::Sha256).toHex(),
              QByteArray("ab078671bb49f06228eadccd34f1bb501e157f7a047095ffb943ba81512c77d1"));
    yml.close(); // Windows cannot delete open files
    EXPECT_TRUE(HandwritingOcr::removeModels());
    EXPECT_FALSE(QDir(dir.path()).exists());
    if (saved.isEmpty())
        qunsetenv("FOLVIO_HANDWRITING_MODELS");
    else
        qputenv("FOLVIO_HANDWRITING_MODELS", saved);
}
