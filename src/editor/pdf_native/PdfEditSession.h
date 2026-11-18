#pragma once

#include "core/files/SafeFileWriter.h"
#include "editor/pdf_native/PdfTextLayout.h"
#include "engines/pdf/PdfDocument.h"

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QSet>

#include <memory>

namespace folvio {

struct TextStyle {
    QString family;          // installed family used for new text ("Arial", "Calibri", ...)
    bool bold = false;
    bool italic = false;
    double size = 11;        // visual points
    QColor color = Qt::black;
    enum Align { Left = 0, Center = 1, Right = 2 };
    int align = Left;

    bool operator==(const TextStyle& o) const
    {
        return family == o.family && bold == o.bold && italic == o.italic && std::abs(size - o.size) < 0.01
               && color == o.color && align == o.align;
    }
    bool operator!=(const TextStyle& o) const { return !(*this == o); }
};

struct OcrWordPdf {
    QString text;
    QRectF rect;   // PDF space
};

// One user edit. Edits are replayed on the pristine original bytes, so every
// state is reproducible and undo/redo never accumulates damage.
struct EditOp {
    enum class Kind { ReplaceBlock, AddText, AddImage, OcrLayer };
    Kind kind = Kind::ReplaceBlock;
    int page = 0;
    int blockId = -1;       // ReplaceBlock: id from PdfTextLayout on the original page
    int addedId = -1;       // AddText: stable id; later ops with the same id supersede earlier ones
    QString text;           // new text ('\n' separates lines); empty = delete
    TextStyle style;
    bool styleChanged = false;
    QPointF origin;         // AddText: top-left corner (PDF space)
    double width = 0;       // AddText: wrap width (0 = no wrapping)
    QRectF rect;            // AddImage: target rectangle (PDF space)
    QByteArray image;       // AddImage: PNG bytes
    QList<OcrWordPdf> words; // OcrLayer: invisible text layer
    int batch = 0;          // ops sharing a non-zero batch id are undone/redone together

    QJsonObject toJson() const;
    static EditOp fromJson(const QJsonObject& o);
};

struct BlockState {
    QString text;
    TextStyle style;
    QRectF bounds;          // current bounds (PDF space)
    bool edited = false;
    bool editable = true;
    bool mixedStyle = false;
};

struct AddedItem {
    int addedId = -1;
    int page = 0;
    QString text;
    TextStyle style;
    QRectF bounds;
    QPointF origin;
    double width = 0;
};

struct RecoveryInfo {
    QString recoveryFile;
    QString documentPath;
    QDateTime savedAt;
    int editCount = 0;
};

class PdfEditSession : public QObject {
    Q_OBJECT
public:
    explicit PdfEditSession(QObject* parent = nullptr);
    ~PdfEditSession() override;

    enum class OpenStatus { Ok, NeedsPassword, Failed };
    OpenStatus open(const QString& path, const QByteArray& password = {}, QString* error = nullptr);

    QString filePath() const { return m_path; }
    const PdfAnalysis& analysis() const { return m_analysis; }
    PdfDocument* original() const { return m_original.get(); }
    PdfDocument* current() const { return m_current.get(); }
    int pageCount() const;

    const QList<TextBlock>& blocks(int page) const; // layout of the original page
    BlockState blockState(int page, int blockId) const;
    QList<AddedItem> addedItems(int page) const;
    TextStyle styleForRun(const PdfTextRun& run) const;
    static int detectAlignment(const TextBlock& b);

    struct Hit {
        enum Type { None, Block, Added } type = None;
        int id = -1;
    };
    Hit hitTest(int page, const QPointF& pdfPoint) const;

    bool apply(const EditOp& op, QString* error = nullptr);
    // Applies several operations with a single rebuild (e.g. "Replace all", OCR on many pages).
    bool applyBatch(const QList<EditOp>& ops, QString* error = nullptr);
    bool canUndo() const { return m_cursor > 0; }
    bool canRedo() const { return m_cursor < m_ops.size(); }
    bool undo();
    bool redo();
    bool isModified() const { return m_cursor != m_savedCursor; }
    QList<EditOp> activeOps() const { return m_ops.mid(0, m_cursor); }
    QSet<int> changedPages() const;
    int nextAddedId() const;
    QStringList lastWarnings() const { return m_warnings; }

    // Saving --------------------------------------------------------------
    struct SaveOptions {
        bool createBackup = true;
        QString backupDir;
        bool ignoreExternalChange = false;
    };
    struct SaveResult {
        bool ok = false;
        SafeFileWriter::Status status = SafeFileWriter::Status::Ok;
        QString error;
        QString backupPath;
        QStringList warnings;
    };
    // Warnings the user must see before overwriting the original.
    QStringList preSaveWarnings() const;
    bool hasExternalChange() const;
    SaveResult save(const SaveOptions& options);
    SaveResult save() { return save(SaveOptions()); }
    SaveResult saveAs(const QString& path, bool overwrite);
    bool serialize(QByteArray* out, QString* error) const;
    // Re-opens the produced bytes and checks the result (page count, edited
    // text present, untouched content unchanged).
    bool verifyOutput(const QByteArray& bytes, QString* error) const;

    // Crash recovery ------------------------------------------------------
    void writeRecovery() const;
    void discardRecovery() const;
    static QList<RecoveryInfo> pendingRecoveries();
    bool restoreFromRecovery(const QString& recoveryFile, QString* error);
    static void deleteRecoveryFile(const QString& recoveryFile);

Q_SIGNALS:
    void documentChanged(const QSet<int>& pages);
    void modifiedChanged(bool modified);

private:
    bool rebuild(QString* error);
    QString recoveryPath() const;

    QString m_path;
    QByteArray m_originalBytes;
    QByteArray m_password;
    FileFingerprint m_fingerprint;
    PdfAnalysis m_analysis;
    std::unique_ptr<PdfDocument> m_original;
    std::unique_ptr<PdfDocument> m_current;
    mutable QHash<int, QList<TextBlock>> m_blocks;
    mutable QHash<int, QList<PdfTextRun>> m_runs;
    QHash<quint64, QRectF> m_currentBounds;     // (page << 32 | blockId) -> bounds after edits
    QHash<quint64, AddedItem> m_added;          // (page << 32 | addedId)
    QList<EditOp> m_ops;
    int m_cursor = 0;
    int m_savedCursor = 0;
    QStringList m_warnings;
};

} // namespace folvio
