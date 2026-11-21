#include "engines/ocr/HandwritingOcr.h"

#include "core/files/PathUtils.h"
#include "engines/ocr/TesseractEngine.h"

#include <onnxruntime_c_api.h>

#include <opencv2/imgproc.hpp>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <cmath>
#include <mutex>
#include <vector>

namespace folvio {

namespace {

// Oldest ONNX Runtime API this code needs (1.17); newer runtimes keep the older entries.
constexpr uint32_t kOrtApiVersion = 17;

struct Model {
    QString file;
    const char* url;
    const char* sha256;
    qint64 size;
};

// PaddlePaddle's official ONNX exports (Apache-2.0), pinned to exact commits.
const Model kModels[] = {
    {QStringLiteral("det.onnx"),
     "https://huggingface.co/PaddlePaddle/PP-OCRv6_small_det_onnx/resolve/28fe5895c24fd108c19eb3e8479f4ab385fbfc62/inference.onnx",
     "d73e0058b7a8086bbd57f3d10b8bcd4ff95363f67e06e2762b5e814fe9c9410e", 9880512},
    {QStringLiteral("rec.onnx"),
     "https://huggingface.co/PaddlePaddle/PP-OCRv6_small_rec_onnx/resolve/b8f84f0b80c529de40b4fbb3544b84fa7233a513/inference.onnx",
     "5435fd747c9e0efe15a96d0b378d5bd157e9492ed8fd80edf08f30d02fa24634", 21159378},
    {QStringLiteral("rec.yml"),
     "https://huggingface.co/PaddlePaddle/PP-OCRv6_small_rec_onnx/resolve/b8f84f0b80c529de40b4fbb3544b84fa7233a513/inference.yml",
     "ab078671bb49f06228eadccd34f1bb501e157f7a047095ffb943ba81512c77d1", 150579},
};

// ---- ONNX Runtime, loaded at run time -------------------------------------------------

struct Runtime {
    QLibrary lib;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    QString error;
};

Runtime& runtime()
{
    static Runtime rt;
    static std::once_flag once;
    std::call_once(once, [] {
        QStringList candidates;
        const QString override = qEnvironmentVariable("FOLVIO_ONNXRUNTIME");
        if (!override.isEmpty())
            candidates << override;
        const QString app = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
        candidates << app + "/onnxruntime.dll";
#else
        candidates << app + "/libonnxruntime.so" << app + "/../lib/folvio/libonnxruntime.so"
                   << app + "/../lib/libonnxruntime.so";
#endif
        QString lastError;
        for (const QString& c : candidates) {
            if (!QFileInfo::exists(c))
                continue;
            rt.lib.setFileName(c);
            if (rt.lib.load())
                break;
            lastError = rt.lib.errorString();
        }
        if (!rt.lib.isLoaded()) {
            rt.error = lastError.isEmpty() ? QObject::tr("ONNX Runtime is not part of this installation.")
                                           : QObject::tr("ONNX Runtime could not be loaded: %1").arg(lastError);
#ifdef Q_OS_WIN
            if (!lastError.isEmpty())
                rt.error += QObject::tr(" It needs the Microsoft Visual C++ Redistributable "
                                        "(winget install Microsoft.VCRedist.2015+.x64).");
#endif
            return;
        }
        using GetBase = const OrtApiBase* (*)();
        auto getBase = reinterpret_cast<GetBase>(rt.lib.resolve("OrtGetApiBase"));
        const OrtApiBase* base = getBase ? getBase() : nullptr;
        rt.api = base ? base->GetApi(kOrtApiVersion) : nullptr;
        if (!rt.api) {
            rt.error = QObject::tr("The installed ONNX Runtime is too old.");
            return;
        }
        OrtStatus* st = rt.api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "folvio", &rt.env);
        if (st) {
            rt.error = QString::fromUtf8(rt.api->GetErrorMessage(st));
            rt.api->ReleaseStatus(st);
            rt.api = nullptr;
            return;
        }
        // Offline application: no telemetry events from the runtime.
        if (OrtStatus* t = rt.api->DisableTelemetryEvents(rt.env))
            rt.api->ReleaseStatus(t);
    });
    return rt;
}

class OrtError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void check(OrtStatus* st)
{
    if (!st)
        return;
    const std::string msg = runtime().api->GetErrorMessage(st);
    runtime().api->ReleaseStatus(st);
    throw OrtError(msg);
}

// A loaded model: one session, its input/output names.
struct Session {
    OrtSession* session = nullptr;
    std::string input, output;
};

Session loadSession(const QString& path)
{
    const OrtApi* api = runtime().api;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw OrtError(QObject::tr("The model %1 cannot be read.").arg(QFileInfo(path).fileName()).toStdString());
    const QByteArray bytes = f.readAll();
    OrtSessionOptions* opts = nullptr;
    check(api->CreateSessionOptions(&opts));
    api->SetIntraOpNumThreads(opts, std::clamp(QThread::idealThreadCount(), 1, 4));
    api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
    // Inputs change size with every text line; an arena would keep the largest allocations forever.
    api->DisableCpuMemArena(opts);
    Session s;
    OrtStatus* st = api->CreateSessionFromArray(runtime().env, bytes.constData(), size_t(bytes.size()), opts, &s.session);
    api->ReleaseSessionOptions(opts);
    check(st);
    OrtAllocator* alloc = nullptr;
    check(api->GetAllocatorWithDefaultOptions(&alloc));
    char* name = nullptr;
    check(api->SessionGetInputName(s.session, 0, alloc, &name));
    s.input = name;
    api->AllocatorFree(alloc, name);
    check(api->SessionGetOutputName(s.session, 0, alloc, &name));
    s.output = name;
    api->AllocatorFree(alloc, name);
    return s;
}

// Runs a model on one NCHW float tensor; returns the first output and its shape.
std::vector<float> run(const Session& s, std::vector<float>& input, const std::vector<int64_t>& shape, std::vector<int64_t>* outShape)
{
    const OrtApi* api = runtime().api;
    OrtMemoryInfo* mem = nullptr;
    check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem));
    OrtValue* in = nullptr;
    OrtStatus* st = api->CreateTensorWithDataAsOrtValue(mem, input.data(), input.size() * sizeof(float), shape.data(), shape.size(),
                                                        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in);
    api->ReleaseMemoryInfo(mem);
    check(st);
    const char* inNames[] = {s.input.c_str()};
    const char* outNames[] = {s.output.c_str()};
    OrtValue* out = nullptr;
    st = api->Run(s.session, nullptr, inNames, &in, 1, outNames, 1, &out);
    api->ReleaseValue(in);
    check(st);
    OrtTensorTypeAndShapeInfo* info = nullptr;
    check(api->GetTensorTypeAndShape(out, &info));
    size_t dims = 0;
    api->GetDimensionsCount(info, &dims);
    outShape->assign(dims, 0);
    api->GetDimensions(info, outShape->data(), dims);
    size_t count = 0;
    api->GetTensorShapeElementCount(info, &count);
    api->ReleaseTensorTypeAndShapeInfo(info);
    float* data = nullptr;
    st = api->GetTensorMutableData(out, reinterpret_cast<void**>(&data));
    std::vector<float> result;
    if (!st)
        result.assign(data, data + count);
    api->ReleaseValue(out);
    check(st);
    return result;
}

struct Models {
    Session det, rec;
    QStringList chars; // index 0 = CTC blank
    QString error;
    bool ok = false;
};

std::mutex g_modelsMutex;

Models& models()
{
    static Models m;
    return m;
}

// Loads (once) and returns the models; call with g_modelsMutex held.
Models& loadedModels()
{
    Models& m = models();
    if (m.ok)
        return m;
    m.error.clear();
    try {
        const QString dir = HandwritingOcr::modelDir();
        QFile yml(dir + "/rec.yml");
        if (!yml.open(QIODevice::ReadOnly))
            throw OrtError("rec.yml");
        const QStringList dict = HandwritingOcr::parseCharacterDict(yml.readAll());
        if (dict.size() < 100)
            throw OrtError(QObject::tr("The character list of the recognition model is damaged.").toStdString());
        m.chars = QStringList{QString()} + dict + QStringList{QStringLiteral(" ")};
        m.det = loadSession(dir + "/det.onnx");
        m.rec = loadSession(dir + "/rec.onnx");
        m.ok = true;
    } catch (const std::exception& e) {
        m.error = QObject::tr("The handwriting models could not be loaded: %1").arg(QString::fromStdString(e.what()));
    }
    return m;
}

// ---- detection (DB) ----------------------------------------------------------------------

cv::Mat toBgr(const QImage& image)
{
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    cv::Mat m(rgb.height(), rgb.width(), CV_8UC3, const_cast<uchar*>(rgb.constBits()), size_t(rgb.bytesPerLine()));
    cv::Mat bgr;
    cv::cvtColor(m, bgr, cv::COLOR_RGB2BGR);
    return bgr;
}

std::vector<std::array<cv::Point2f, 4>> detect(const Session& det, const cv::Mat& bgr)
{
    constexpr int kLimit = 1280; // longer side; finer gave no gain on handwriting (see docs)
    // Multiples of 32, rounded half to even like PaddleOCR (std::nearbyint, default rounding mode).
    const double s = std::min(1.0, double(kLimit) / std::max(bgr.cols, bgr.rows));
    const int nh = std::max(32, int(std::nearbyint(bgr.rows * s / 32.0)) * 32);
    const int nw = std::max(32, int(std::nearbyint(bgr.cols * s / 32.0)) * 32);
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(nw, nh));
    std::vector<float> input(size_t(3) * nh * nw);
    const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f};
    for (int y = 0; y < nh; ++y) {
        const uchar* row = resized.ptr<uchar>(y);
        for (int x = 0; x < nw; ++x)
            for (int c = 0; c < 3; ++c)
                input[size_t(c) * nh * nw + size_t(y) * nw + x] = (row[x * 3 + c] / 255.0f - mean[c]) / stdv[c];
    }
    std::vector<int64_t> outShape;
    std::vector<float> prob = run(det, input, {1, 3, nh, nw}, &outShape);
    if (outShape.size() != 4 || outShape[2] != nh || outShape[3] != nw)
        throw OrtError("unexpected detection output");
    cv::Mat probMap(nh, nw, CV_32F, prob.data());
    cv::Mat mask = probMap > 0.3f;
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    std::vector<std::array<cv::Point2f, 4>> boxes;
    for (size_t i = 0; i < contours.size() && i < 1000; ++i) {
        cv::RotatedRect r = cv::minAreaRect(contours[i]);
        if (std::min(r.size.width, r.size.height) < 3)
            continue;
        // Mean probability inside the contour.
        const cv::Rect bb = cv::boundingRect(contours[i]) & cv::Rect(0, 0, nw, nh);
        cv::Mat local = cv::Mat::zeros(bb.size(), CV_8U);
        std::vector<std::vector<cv::Point>> shifted{contours[i]};
        for (cv::Point& p : shifted[0])
            p -= bb.tl();
        cv::fillPoly(local, shifted, cv::Scalar(1));
        if (cv::mean(probMap(bb), local)[0] < 0.6)
            continue;
        // Unclip: grow by area * 1.5 / perimeter on every side.
        const double area = double(r.size.width) * r.size.height, peri = 2.0 * (r.size.width + r.size.height);
        const float d = float(area * 1.5 / peri);
        r.size.width += 2 * d;
        r.size.height += 2 * d;
        if (std::min(r.size.width, r.size.height) < 5)
            continue;
        std::array<cv::Point2f, 4> pts;
        r.points(pts.data());
        for (cv::Point2f& p : pts) {
            p.x = float(p.x * double(bgr.cols) / nw);
            p.y = float(p.y * double(bgr.rows) / nh);
        }
        boxes.push_back(pts);
    }
    return boxes;
}

// tl, tr, br, bl
std::array<cv::Point2f, 4> ordered(const std::array<cv::Point2f, 4>& p)
{
    std::array<cv::Point2f, 4> o;
    auto byKey = [&](auto key, bool max) {
        return *std::max_element(p.begin(), p.end(), [&](const cv::Point2f& a, const cv::Point2f& b) {
            return max ? key(a) < key(b) : key(a) > key(b);
        });
    };
    o[0] = byKey([](const cv::Point2f& q) { return q.x + q.y; }, false);
    o[2] = byKey([](const cv::Point2f& q) { return q.x + q.y; }, true);
    o[1] = byKey([](const cv::Point2f& q) { return q.y - q.x; }, false);
    o[3] = byKey([](const cv::Point2f& q) { return q.y - q.x; }, true);
    return o;
}

// ---- recognition (CTC) -------------------------------------------------------------------

RecognizedLine recognizeBox(const Models& m, const cv::Mat& bgr, const std::array<cv::Point2f, 4>& box)
{
    const std::array<cv::Point2f, 4> p = ordered(box);
    const int w = int(std::max(cv::norm(p[0] - p[1]), cv::norm(p[2] - p[3])));
    const int h = int(std::max(cv::norm(p[0] - p[3]), cv::norm(p[1] - p[2])));
    RecognizedLine line;
    for (const cv::Point2f& q : p)
        line.box << QPointF(q.x, q.y);
    if (w < 2 || h < 2)
        return line;
    const cv::Point2f dst[4] = {{0, 0}, {float(w), 0}, {float(w), float(h)}, {0, float(h)}};
    cv::Mat crop;
    cv::warpPerspective(bgr, crop, cv::getPerspectiveTransform(p.data(), dst), cv::Size(w, h), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    if (crop.rows >= 1.5 * crop.cols)
        cv::rotate(crop, crop, cv::ROTATE_90_COUNTERCLOCKWISE);
    constexpr int H = 48;
    const int W = std::clamp(int(std::ceil(H * double(crop.cols) / std::max(1, crop.rows))), 16, 3200);
    cv::Mat resized;
    cv::resize(crop, resized, cv::Size(W, H));
    std::vector<float> input(size_t(3) * H * W);
    for (int y = 0; y < H; ++y) {
        const uchar* row = resized.ptr<uchar>(y);
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 3; ++c)
                input[size_t(c) * H * W + size_t(y) * W + x] = (row[x * 3 + c] / 255.0f - 0.5f) / 0.5f;
    }
    std::vector<int64_t> shape;
    const std::vector<float> out = run(m.rec, input, {1, 3, H, W}, &shape);
    if (shape.size() != 3)
        throw OrtError("unexpected recognition output");
    const int64_t T = shape[1], C = shape[2];
    if (C != m.chars.size())
        throw OrtError(QObject::tr("The recognition model does not match its character list (%1 / %2).")
                           .arg(C)
                           .arg(m.chars.size())
                           .toStdString());
    QString text;
    double confSum = 0;
    int confN = 0;
    int64_t last = 0;
    for (int64_t t = 0; t < T; ++t) {
        const float* row = out.data() + t * C;
        const int64_t k = std::max_element(row, row + C) - row;
        if (k != 0 && k != last) {
            text += m.chars[int(k)];
            confSum += row[k];
            ++confN;
        }
        last = k;
    }
    line.text = text.trimmed();
    line.confidence = confN ? confSum / confN : 0.0;
    return line;
}

QString sha256Hex(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
}

} // namespace

// ---- public API ------------------------------------------------------------------------------

QString RecognitionResult::text() const
{
    QStringList l;
    for (const RecognizedLine& line : lines)
        l << line.text;
    return l.join('\n');
}

QList<HandwritingOcr::ModelFile> HandwritingOcr::modelFiles()
{
    QList<ModelFile> list;
    for (const Model& m : kModels)
        list << ModelFile{m.file, QString::fromLatin1(m.url), QByteArray(m.sha256), m.size};
    return list;
}

qint64 HandwritingOcr::downloadSize()
{
    qint64 n = 0;
    for (const Model& m : kModels)
        n += m.size;
    return n;
}

QString HandwritingOcr::modelDir()
{
    const QString override = qEnvironmentVariable("FOLVIO_HANDWRITING_MODELS");
    if (!override.isEmpty())
        return override;
    return PathUtils::appDataDir() + "/models/ppocrv6-small";
}

QString HandwritingOcr::sourceDescription()
{
    return QObject::tr("PP-OCRv6 small text detection and recognition by PaddlePaddle (Apache-2.0), downloaded from "
                       "huggingface.co/PaddlePaddle at fixed versions and checked with SHA-256.");
}

bool HandwritingOcr::runtimeAvailable(QString* reason)
{
    Runtime& rt = runtime();
    if (rt.api)
        return true;
    if (reason)
        *reason = rt.error;
    return false;
}

bool HandwritingOcr::modelsInstalled()
{
    for (const Model& m : kModels) {
        const QFileInfo fi(modelDir() + "/" + m.file);
        if (!fi.isFile() || fi.size() != m.size)
            return false;
    }
    return true;
}

bool HandwritingOcr::isAvailable(QString* reason)
{
    if (!runtimeAvailable(reason))
        return false;
    if (!modelsInstalled()) {
        if (reason)
            *reason = QObject::tr("The handwriting models are not installed.");
        return false;
    }
    return true;
}

bool HandwritingOcr::removeModels(QString* error)
{
    std::lock_guard lock(g_modelsMutex);
    Models& m = models();
    if (m.ok && runtime().api) {
        runtime().api->ReleaseSession(m.det.session);
        runtime().api->ReleaseSession(m.rec.session);
    }
    m = Models();
    QDir dir(modelDir());
    if (dir.exists() && !dir.removeRecursively()) {
        if (error)
            *error = QObject::tr("The folder %1 could not be removed.").arg(QDir::toNativeSeparators(dir.path()));
        return false;
    }
    return true;
}

QStringList HandwritingOcr::parseCharacterDict(const QByteArray& yml)
{
    // A small reader for the one list we need; PaddleOCR writes it with yaml.dump:
    //   PostProcess:
    //     character_dict:
    //     - a
    //     - '#'
    //     - ''''
    QStringList out;
    const QList<QByteArray> lines = yml.split('\n');
    bool inPost = false, inDict = false;
    int dictIndent = -1;
    for (QByteArray raw : lines) {
        if (raw.endsWith('\r'))
            raw.chop(1);
        const QString line = QString::fromUtf8(raw);
        const int indent = int(line.size() - QString(line).remove(QRegularExpression("^ +")).size());
        const QString t = line.trimmed();
        if (indent == 0 && !t.isEmpty()) {
            if (inDict)
                break;
            inPost = t == QLatin1String("PostProcess:");
            continue;
        }
        if (inPost && !inDict && t == QLatin1String("character_dict:")) {
            inDict = true;
            dictIndent = indent;
            continue;
        }
        if (!inDict)
            continue;
        if (!t.startsWith(QLatin1String("- ")) && t != QLatin1String("-")) {
            if (indent <= dictIndent && !t.isEmpty())
                break; // next key
            continue;
        }
        QString v = line.mid(line.indexOf('-') + 1);
        if (v.startsWith(' '))
            v.remove(0, 1);
        if (v.size() >= 2 && v.startsWith('\'') && v.endsWith('\'')) {
            v = v.mid(1, v.size() - 2).replace(QLatin1String("''"), QLatin1String("'"));
        } else if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"')) {
            const QString in = v.mid(1, v.size() - 2);
            QString o;
            for (int i = 0; i < in.size(); ++i) {
                if (in[i] != '\\' || i + 1 >= in.size()) {
                    o += in[i];
                    continue;
                }
                const QChar e = in[++i];
                if (e == 'n') o += '\n';
                else if (e == 't') o += '\t';
                else if (e == '0') o += QChar(0);
                else if (e == ' ') o += ' ';
                else if (e == 'x' || e == 'u' || e == 'U') {
                    const int len = e == 'x' ? 2 : e == 'u' ? 4 : 8;
                    const char32_t cp = in.mid(i + 1, len).toUInt(nullptr, 16);
                    o += QString::fromUcs4(&cp, 1);
                    i += len;
                } else
                    o += e;
            }
            v = o;
        } else {
            // YAML plain scalars lose only ASCII spaces and tabs (U+3000 is a valid entry).
            while (!v.isEmpty() && (v.back() == QLatin1Char(' ') || v.back() == QLatin1Char('\t')))
                v.chop(1);
        }
        out << v;
    }
    return out;
}

RecognitionResult HandwritingOcr::recognize(const QImage& image, const std::atomic_bool* cancel)
{
    RecognitionResult r;
    r.engine = QStringLiteral("handwriting");
    QString reason;
    if (!isAvailable(&reason)) {
        r.error = reason;
        return r;
    }
    if (image.isNull()) {
        r.error = QObject::tr("The selected area is empty.");
        return r;
    }
    std::lock_guard lock(g_modelsMutex);
    Models& m = loadedModels();
    if (!m.ok) {
        r.error = m.error;
        return r;
    }
    try {
        const cv::Mat bgr = toBgr(image);
        auto boxes = detect(m.det, bgr);
        struct Item {
            double cy, x, h;
            RecognizedLine line;
        };
        std::vector<Item> items;
        for (const auto& b : boxes) {
            if (cancel && cancel->load()) {
                r.error = QObject::tr("Cancelled.");
                return r;
            }
            RecognizedLine l = recognizeBox(m, bgr, b);
            if (l.text.isEmpty())
                continue;
            const QRectF br = l.box.boundingRect();
            items.push_back({br.center().y(), br.left(), br.height(), l});
        }
        // Reading order: rows top to bottom (boxes whose centres are within half a box height
        // belong to the same row), left to right inside a row.
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.cy < b.cy; });
        std::vector<std::vector<Item>> rows;
        for (const Item& it : items) {
            if (rows.empty() || std::abs(it.cy - rows.back().front().cy) > 0.5 * std::max(1.0, rows.back().front().h))
                rows.push_back({});
            rows.back().push_back(it);
        }
        double sum = 0;
        int n = 0;
        for (auto& row : rows) {
            std::sort(row.begin(), row.end(), [](const Item& a, const Item& b) { return a.x < b.x; });
            RecognizedLine merged;
            QStringList words;
            double c = 0;
            QPolygonF hull;
            for (const Item& it : row) {
                words << it.line.text;
                c += it.line.confidence;
                hull = hull.united(it.line.box.boundingRect());
                sum += it.line.confidence;
                ++n;
            }
            merged.text = words.join(' ');
            merged.confidence = c / row.size();
            merged.box = hull;
            r.lines << merged;
        }
        r.confidence = n ? sum / n : 0.0;
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = QObject::tr("Handwriting recognition failed: %1").arg(QString::fromStdString(e.what()));
    }
    return r;
}

// ---- model download --------------------------------------------------------------------------

HandwritingModelDownloader::HandwritingModelDownloader(QObject* parent) : QObject(parent)
{
    m_net = new QNetworkAccessManager(this);
}

HandwritingModelDownloader::~HandwritingModelDownloader()
{
    cancel();
}

void HandwritingModelDownloader::start()
{
    if (m_reply)
        return;
    m_cancelled = false;
    m_index = -1;
    m_doneBefore = 0;
    QDir().mkpath(HandwritingOcr::modelDir());
    next();
}

void HandwritingModelDownloader::cancel()
{
    m_cancelled = true;
    if (m_reply) {
        QNetworkReply* r = m_reply;
        m_reply = nullptr;
        r->abort();
        r->deleteLater();
    }
    if (m_file) {
        m_file->close();
        m_file->remove();
        m_file.reset();
    }
}

void HandwritingModelDownloader::fail(const QString& error)
{
    cancel();
    Q_EMIT finished(false, error);
}

void HandwritingModelDownloader::next()
{
    const QList<HandwritingOcr::ModelFile> files = HandwritingOcr::modelFiles();
    ++m_index;
    // Skip files that are already complete and correct.
    while (m_index < files.size()) {
        const QString path = HandwritingOcr::modelDir() + "/" + files[m_index].name;
        if (QFileInfo(path).size() == files[m_index].size && sha256Hex(path).toLatin1() == files[m_index].sha256) {
            m_doneBefore += files[m_index].size;
            ++m_index;
            continue;
        }
        break;
    }
    if (m_index >= files.size()) {
        Q_EMIT progress(HandwritingOcr::downloadSize(), HandwritingOcr::downloadSize());
        Q_EMIT finished(true, {});
        return;
    }
    const HandwritingOcr::ModelFile f = files[m_index];
    m_file = std::make_unique<QFile>(HandwritingOcr::modelDir() + "/" + f.name + ".part");
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QObject::tr("%1 cannot be written: %2").arg(QDir::toNativeSeparators(m_file->fileName()), m_file->errorString()));
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    QNetworkRequest req{QUrl(f.url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Folvio"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);
    m_reply = m_net->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (!m_reply)
            return;
        const QByteArray chunk = m_reply->readAll();
        m_hash->addData(chunk);
        if (m_file->write(chunk) != chunk.size())
            fail(QObject::tr("Not enough disk space for the handwriting models."));
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64) {
        Q_EMIT progress(m_doneBefore + received, HandwritingOcr::downloadSize());
    });
    connect(m_reply, &QNetworkReply::finished, this, [this, f] {
        QNetworkReply* reply = m_reply;
        if (!reply)
            return; // cancelled
        m_reply = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            fail(QObject::tr("Download failed: %1").arg(reply->errorString()));
            return;
        }
        m_file->write(reply->readAll());
        m_file->close();
        const QByteArray got = m_hash->result().toHex();
        if (got != f.sha256 || m_file->size() != f.size) {
            m_file->remove();
            m_file.reset();
            fail(QObject::tr("The downloaded file %1 failed the integrity check and was deleted.").arg(f.name));
            return;
        }
        const QString final = HandwritingOcr::modelDir() + "/" + f.name;
        QFile::remove(final);
        if (!m_file->rename(final)) {
            fail(QObject::tr("%1 could not be stored.").arg(QDir::toNativeSeparators(final)));
            return;
        }
        m_file.reset();
        m_doneBefore += f.size;
        next();
    });
}

// ---- Printed / Handwriting / Auto ----------------------------------------------------------

RecognitionResult TextRecognition::recognize(const QImage& image, Mode mode, const QString& printedLanguages,
                                             const std::atomic_bool* cancel)
{
    auto printed = [&]() {
        RecognitionResult r;
        r.engine = QStringLiteral("printed");
        QString reason;
        if (!TesseractEngine::isAvailable(&reason)) {
            r.error = reason;
            return r;
        }
        QTemporaryDir work;
        const OcrPageResult p = TesseractEngine::recognize(image, printedLanguages.isEmpty() ? TesseractEngine::defaultLanguages()
                                                                                               : printedLanguages,
                                                           work.path(), cancel);
        if (!p.ok) {
            r.error = p.error;
            return r;
        }
        QMap<int, RecognizedLine> byLine;
        QMap<int, int> counts;
        for (const OcrWord& w : p.words) {
            RecognizedLine& l = byLine[w.line];
            l.text += (l.text.isEmpty() ? QString() : QStringLiteral(" ")) + w.text;
            l.confidence += w.confidence / 100.0;
            l.box = l.box.united(QPolygonF(QRectF(w.box)));
            ++counts[w.line];
        }
        for (auto it = byLine.begin(); it != byLine.end(); ++it) {
            it->confidence /= std::max(1, counts[it.key()]);
            r.lines << *it;
        }
        r.confidence = p.meanConfidence / 100.0;
        r.ok = true;
        return r;
    };
    switch (mode) {
    case Mode::Printed: return printed();
    case Mode::Handwriting: return HandwritingOcr::recognize(image, cancel);
    case Mode::Auto: {
        // Printed text that Tesseract reads with high confidence stays with Tesseract (better layout
        // and punctuation); everything else goes to the handwriting recogniser when it is installed.
        RecognitionResult p = printed();
        if (p.ok && !p.lines.isEmpty() && p.confidence >= 0.80)
            return p;
        if (HandwritingOcr::isAvailable()) {
            RecognitionResult h = HandwritingOcr::recognize(image, cancel);
            if (h.ok && (!p.ok || h.confidence >= p.confidence || p.lines.isEmpty()))
                return h;
        }
        return p;
    }
    }
    return {};
}

} // namespace folvio
