#pragma once

#include <QImage>
#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

#include <atomic>

namespace folvio {

struct OcrWord {
    QString text;
    QRect box;          // pixels in the source image
    double confidence;  // 0..100 as reported by Tesseract
    int line = 0;       // line index for re-assembling text
};

struct OcrPageResult {
    bool ok = false;
    QString error;
    QList<OcrWord> words;
    double meanConfidence = 0;
    QSize imageSize;    // size of the recognized image (word boxes are relative to it)
    QString text() const;
};

// Runs the Tesseract CLI on page images. Recognition results are never assumed
// to be correct: every word carries Tesseract's confidence so the UI can flag
// doubtful words for review.
class TesseractEngine {
public:
    static bool isAvailable(QString* reason = nullptr);
    static QStringList availableLanguages();
    static QString defaultLanguages(); // "pol+eng" when both are installed

    static OcrPageResult recognize(const QImage& image, const QString& languages, const QString& workDir,
                                   const std::atomic_bool* cancel = nullptr);
    static QList<OcrWord> parseTsv(const QByteArray& tsv);
};

} // namespace folvio
