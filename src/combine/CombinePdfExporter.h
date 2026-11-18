#pragma once

#include "combine/CombineModel.h"

#include <QList>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <atomic>
#include <functional>

namespace folvio::combine {

// Layout of one exported page (PDF points, y-up as in PDF).
struct PagePlan {
    QSizeF pageSize;      // points
    QRectF imageRect;     // where the page image is drawn (points, PDF coordinates)
    QSize imagePixels;    // pixels of the image that will be embedded
    bool landscape = false;
    bool passthroughJpeg = false; // original JPEG bytes embedded without re-encoding
    qint64 estimatedBytes = 0;
};

struct ExportResult {
    bool ok = false;
    bool cancelled = false;
    QString error;
    QString path;
    qint64 bytes = 0;
    int pages = 0;
};

// Builds a multi-page PDF with PDFium:
//  * images: original JPEG passed through when the page has no geometry edits; otherwise the
//    rendered full-resolution page is embedded lossless (High, sources without lossy
//    compression) or as JPEG at the selected quality,
//  * annotations: vector paths with transparency; text as real, searchable text with an
//    exact ToUnicode map (Polish characters included).
class CombinePdfExporter {
public:
    using Progress = std::function<void(int done, int total, const QString& stage)>;

    static PagePlan plan(const CombinePage& page, const ExportSettings& s);
    static qint64 estimateTotalBytes(const QList<CombinePage>& pages, const ExportSettings& s);

    static bool build(const QList<CombinePage>& pages, const ExportSettings& s, QByteArray* out,
                      const Progress& progress, const std::atomic_bool* cancel, QString* error);
    // build + verified, crash-safe write (temp file, re-open check, atomic move).
    static ExportResult exportTo(const QString& path, const QList<CombinePage>& pages, const ExportSettings& s,
                                 bool overwrite, const Progress& progress, const std::atomic_bool* cancel);
    // Re-opens a produced PDF and checks page count and sizes against the plan.
    static bool verify(const QByteArray& pdf, const QList<CombinePage>& pages, const ExportSettings& s, QString* error);
};

} // namespace folvio::combine
