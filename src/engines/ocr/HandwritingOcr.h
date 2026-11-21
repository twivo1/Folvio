#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QPolygonF>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QCryptographicHash;

namespace folvio {

// One recognised line (or word group) of a region.
struct RecognizedLine {
    QString text;
    double confidence = 0;   // 0..1
    QPolygonF box;           // pixels of the recognised image
};

struct RecognitionResult {
    bool ok = false;
    QString error;
    QString engine;          // "handwriting" (PP-OCRv6) or "printed" (Tesseract)
    QList<RecognizedLine> lines;
    double confidence = 0;   // mean, 0..1
    QString text() const;    // lines joined with '\n'
};

// Handwriting recognition, fully offline (experimental):
//   PP-OCRv6 small text detection (DB) + recognition (CTC, 50 languages incl. Polish and English),
//   Apache-2.0 models by PaddlePaddle, run with ONNX Runtime (MIT) through its C API.
// The runtime ships with Folvio; the models (~31 MB) are downloaded only after the user agrees,
// from pinned commits of PaddlePaddle's Hugging Face repositories, and verified with SHA-256.
// Nothing is ever uploaded; recognition runs on this computer.
class HandwritingOcr {
public:
    struct ModelFile {
        QString name;        // stored file name
        QString url;
        QByteArray sha256;   // hex
        qint64 size = 0;
    };
    static QList<ModelFile> modelFiles();
    static qint64 downloadSize();
    static QString modelDir();          // <app data>/models/ppocrv6-small
    static QString sourceDescription(); // for the consent dialog

    // The ONNX Runtime library could be loaded (it is part of the installation).
    static bool runtimeAvailable(QString* reason = nullptr);
    static bool modelsInstalled();
    static bool isAvailable(QString* reason = nullptr); // runtime and models
    static bool removeModels(QString* error = nullptr);

    // Recognises text in 'image' (a selected region at full resolution). Thread-safe.
    static RecognitionResult recognize(const QImage& image, const std::atomic_bool* cancel = nullptr);

    // Parses the character list of a PaddleOCR inference.yml (PostProcess.character_dict).
    static QStringList parseCharacterDict(const QByteArray& yml);
};

// Downloads and verifies the model files; emits finished(true) when everything is in place.
class HandwritingModelDownloader : public QObject {
    Q_OBJECT
public:
    explicit HandwritingModelDownloader(QObject* parent = nullptr);
    ~HandwritingModelDownloader() override;
    void start();
    void cancel();
    bool isRunning() const { return m_reply != nullptr; }

Q_SIGNALS:
    void progress(qint64 done, qint64 total);
    void finished(bool ok, const QString& error);

private:
    void next();
    void fail(const QString& error);
    QNetworkAccessManager* m_net = nullptr;
    QNetworkReply* m_reply = nullptr;
    std::unique_ptr<QFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    int m_index = -1;
    qint64 m_doneBefore = 0;
    bool m_cancelled = false;
};

// Printed / Handwriting / Auto for the Recognize Text tool.
class TextRecognition {
public:
    enum class Mode { Printed, Handwriting, Auto };
    static RecognitionResult recognize(const QImage& image, Mode mode, const QString& printedLanguages,
                                       const std::atomic_bool* cancel = nullptr);
};

} // namespace folvio
