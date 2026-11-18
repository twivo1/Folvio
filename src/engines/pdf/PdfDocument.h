#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QList>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTransform>

#include <memory>
#include <mutex>

// Opaque PDFium handle types (avoid leaking fpdfview.h into every header).
struct fpdf_document_t__;
struct fpdf_page_t__;

namespace folvio {

// PDFium is not thread safe. Every call into PDFium must hold this lock.
std::recursive_mutex& pdfiumMutex();
void ensurePdfiumInitialized();

struct PdfTextRun {
    int objectIndex = -1;       // index of the page object in the page's object list
    QString text;
    QRectF bounds;              // PDF user space (points, origin bottom-left)
    QTransform matrix;          // text object matrix (a b c d e f)
    double fontSize = 0;        // raw font size (Tf)
    double effectiveSize = 0;   // fontSize scaled by the matrix (visual points)
    QString fontName;           // base font name, subset prefix removed
    bool bold = false;
    bool italic = false;
    bool serif = false;
    bool monospace = false;
    bool embedded = false;
    QColor color = Qt::black;
    int renderMode = 0;
    bool axisAligned = true;
    bool spaceAfter = false;    // PDFium's text page places a word boundary after this object
};

struct PdfPageInfo {
    QSizeF size;                // points
    int textObjects = 0;
    int imageObjects = 0;
    int pathObjects = 0;
    int formObjects = 0;
    int charCount = 0;
    double imageCoverage = 0;   // fraction of the page area covered by images
    bool looksScanned = false;
};

struct PdfAnalysis {
    int pageCount = 0;
    int fileVersion = 0;        // e.g. 17 for PDF 1.7
    bool encrypted = false;
    bool hasTextLayer = false;
    bool isScanned = false;     // mostly image pages without text
    int scannedPages = 0;
    int signatureCount = 0;
    bool likelyHasTables = false;
    int totalImages = 0;
    bool canModify = true;      // permissions allow modification
    QList<PdfPageInfo> pages;
    QString summary() const;
};

class PdfDocument {
public:
    enum class OpenError { None, File, Format, Password, Security, Unknown };

    ~PdfDocument();
    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;

    static std::unique_ptr<PdfDocument> openFile(const QString& path, const QByteArray& password = {},
                                                 OpenError* error = nullptr, QString* message = nullptr);
    static std::unique_ptr<PdfDocument> openData(QByteArray data, const QByteArray& password = {},
                                                 OpenError* error = nullptr, QString* message = nullptr);
    static std::unique_ptr<PdfDocument> createEmpty();

    static QString describe(OpenError e);

    int pageCount() const;
    QSizeF pageSize(int page) const; // points
    // Renders a page. scale = pixels per point. Background is white.
    QImage render(int page, double scale, bool withAnnotations = true) const;
    QString pageText(int page) const;
    QString allText() const;
    QList<PdfTextRun> textRuns(int page) const;
    // Thin horizontal path objects (table rules, underlines), PDF space.
    QList<QRectF> horizontalRules(int page) const;
    PdfAnalysis analyze() const;

    bool isEncrypted() const;
    int signatureCount() const;
    const QByteArray& sourceData() const { return m_data; }
    const QByteArray& password() const { return m_password; }

    enum class SaveMode { Full, Incremental };
    bool save(QByteArray* out, SaveMode mode = SaveMode::Full, QString* error = nullptr) const;

    fpdf_document_t__* handle() const { return m_doc; }

private:
    PdfDocument() = default;
    QByteArray m_data;      // must outlive m_doc (PDFium reads from it lazily)
    QByteArray m_password;
    fpdf_document_t__* m_doc = nullptr;
};

// RAII page handle; caller must hold pdfiumMutex().
class PdfPageHandle {
public:
    PdfPageHandle(fpdf_document_t__* doc, int index);
    ~PdfPageHandle();
    PdfPageHandle(const PdfPageHandle&) = delete;
    PdfPageHandle& operator=(const PdfPageHandle&) = delete;
    fpdf_page_t__* get() const { return m_page; }
    explicit operator bool() const { return m_page != nullptr; }

private:
    fpdf_page_t__* m_page = nullptr;
};

QString stripSubsetPrefix(const QString& fontName);

} // namespace folvio
