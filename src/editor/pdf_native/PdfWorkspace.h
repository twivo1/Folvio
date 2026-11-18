#pragma once

#include "editor/pdf_native/PdfEditSession.h"
#include "engines/ocr/TesseractEngine.h"

#include <QFutureWatcher>
#include <QHash>
#include <QPlainTextEdit>
#include <QPointer>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QScrollArea;
class QSplitter;
class QStackedWidget;
class QTableWidget;
class QTimer;
class QToolButton;
class QCheckBox;
class QFontComboBox;
class QActionGroup;

namespace folvio {

class Banner;
class PdfCanvas;

// Text editor placed over a PDF paragraph while it is being edited.
class InlineTextEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit InlineTextEditor(QWidget* parent = nullptr);
Q_SIGNALS:
    void commitRequested();
    void cancelRequested();
protected:
    void keyPressEvent(QKeyEvent* e) override;
};

class PdfWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit PdfWorkspace(QWidget* parent = nullptr);
    ~PdfWorkspace() override;

    bool openFile(const QString& path, QString* error);
    QString filePath() const;
    bool isModified() const;
    bool hasDocument() const;
    // Asks to save unsaved changes. Returns false if the user cancelled.
    bool maybeClose();
    void closeDocument();

    int pageCount() const;
    int currentPage() const;
    double zoom() const;
    QString saveStatus() const;

    bool save();
    bool saveAs();
    void undo();
    void redo();
    void showFind(bool replace);
    void demoEditFirstParagraph();

Q_SIGNALS:
    void statusChanged();
    void documentClosed();
    void busy(const QString& message, int percent); // percent < 0: indeterminate, 100: done
    void reflowRequested(const QString& pdfPath);    // switch to reconstructed (Word-like) editing
    void savedTo(const QString& path);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Tool { EditText, AddText, AddImage };
    enum class ViewMode { Edit, Original, SideBySide, Differences };

    void buildUi();
    void buildToolbar(QWidget* bar, QWidget* formatBar);
    void updateActions();
    void updateFormatControls(const TextStyle& s, bool enabled);
    void setTool(Tool t);
    void setViewMode(ViewMode m);
    void setZoom(double z, bool keepCenter = true);
    void fitWidth();
    void goToPage(int page);
    void refreshThumbnails(const QSet<int>& pages);
    void onDocumentChanged(const QSet<int>& pages);

    void onCanvasClicked(int page, const QPointF& pdf, PdfEditSession::Hit hit, Qt::MouseButton button, const QPoint& global);
    void startEditBlock(int page, int blockId);
    void startEditAdded(int page, int addedId);
    void startNewText(int page, const QPointF& pdfTopLeft);
    void positionEditor();
    void commitEdit();
    void cancelEdit();
    TextStyle currentEditStyle() const;
    void applyStyleToEditor();
    void insertImageAt(int page, const QPointF& pdfPoint, const QString& file);

    // find & replace
    struct Match {
        int page;
        int blockId;
        bool added;
        QRectF rect;
    };
    void runFind();
    void findNext(bool backwards);
    void replaceCurrent();
    void replaceAll();

    // OCR
    void startOcr();
    void onOcrFinished();
    void populateOcrTable();
    void applyOcrLayer();

    void exportAs(const QString& format);
    void showWarnings(const QStringList& warnings);
    void scheduleRecovery();
    void offerRecovery();

    std::unique_ptr<PdfEditSession> m_session;
    // UI
    QLabel* m_title = nullptr;
    QLabel* m_analysis = nullptr;
    Banner* m_banner = nullptr;
    QWidget* m_findBar = nullptr;
    QLineEdit* m_findEdit = nullptr;
    QLineEdit* m_replaceEdit = nullptr;
    QCheckBox* m_caseSensitive = nullptr;
    QLabel* m_findStatus = nullptr;
    QWidget* m_replaceRow = nullptr;
    QListWidget* m_thumbs = nullptr;
    QSplitter* m_viewSplitter = nullptr;
    QScrollArea* m_scroll = nullptr;
    QScrollArea* m_scrollOriginal = nullptr;
    PdfCanvas* m_canvas = nullptr;
    PdfCanvas* m_canvasOriginal = nullptr;
    QWidget* m_sidePanel = nullptr;
    QTableWidget* m_ocrTable = nullptr;
    QLabel* m_ocrSummary = nullptr;
    QCheckBox* m_ocrOnlyDoubtful = nullptr;
    QWidget* m_ocrBox = nullptr;
    QLabel* m_warningsLabel = nullptr;

    QToolButton* m_undoBtn = nullptr;
    QToolButton* m_redoBtn = nullptr;
    QToolButton* m_saveBtn = nullptr;
    QToolButton* m_saveAsBtn = nullptr;
    QToolButton* m_exportBtn = nullptr;
    QToolButton* m_toolEdit = nullptr;
    QToolButton* m_toolText = nullptr;
    QToolButton* m_toolImage = nullptr;
    QToolButton* m_ocrBtn = nullptr;
    QToolButton* m_reflowBtn = nullptr;
    QComboBox* m_viewCombo = nullptr;
    QFontComboBox* m_fontBox = nullptr;
    QDoubleSpinBox* m_sizeBox = nullptr;
    QToolButton* m_boldBtn = nullptr;
    QToolButton* m_italicBtn = nullptr;
    QToolButton* m_colorBtn = nullptr;
    QToolButton* m_alignLeft = nullptr;
    QToolButton* m_alignCenter = nullptr;
    QToolButton* m_alignRight = nullptr;
    QWidget* m_formatBar = nullptr;

    InlineTextEditor* m_editor = nullptr;
    QTimer* m_recoveryTimer = nullptr;

    Tool m_tool = Tool::EditText;
    ViewMode m_viewMode = ViewMode::Edit;
    bool m_confirmedOverwrite = false;
    QString m_pendingImage;

    // editing state
    enum class EditKind { None, Block, Added, NewText } m_editKind = EditKind::None;
    int m_editPage = -1;
    int m_editId = -1;
    QPointF m_editOrigin;
    double m_editWidth = 0;
    TextStyle m_editStyle;
    TextStyle m_editOriginalStyle;
    bool m_syncingControls = false;

    QList<Match> m_matches;
    int m_matchIndex = -1;

    // OCR results per page: words in PDF space + confidence
    struct OcrWordState {
        QString text;
        QRectF rect;
        double confidence;
    };
    QHash<int, QList<OcrWordState>> m_ocr;
    QFutureWatcher<QHash<int, OcrPageResult>>* m_ocrWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_ocrCancel;
    QHash<int, QTransform> m_ocrImageToPdf;
};

} // namespace folvio
