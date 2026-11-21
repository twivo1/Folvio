#pragma once

#include "combine/CombineModel.h"
#include "engines/ocr/HandwritingOcr.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QWidget>

#include <atomic>
#include <memory>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace folvio {
class Banner;
}

namespace folvio::combine {

class PageCanvas;

// "Recognize Text" in Combine and in the Auto Change image editor: the user drags a rectangle on
// the page, the area is recognised at full resolution (printed: Tesseract, handwriting: PP-OCRv6,
// both offline), and the editable result can be copied, inserted as a text annotation, or used to
// replace the selection (always previewed before/after; the photo itself is never changed).
class RecognizePanel : public QWidget {
    Q_OBJECT
public:
    enum class Variant {
        Overlay,          // text over the original, which stays visible
        KeepAndAdd,       // original untouched, text added below the selection
        MatchBackground,  // cover the selection with its background colour, text on top
        CleanNote,        // a clean white note with the text over the selection
    };

    RecognizePanel(CombineDocument* doc, PageCanvas* canvas, Banner* banner, QWidget* parent = nullptr);
    ~RecognizePanel() override;

    // Recognises 'pageArea' (page units) of the current page.
    void recognizeArea(const QRectF& pageArea);
    bool isBusy() const;
    TextRecognition::Mode mode() const;
    void setMode(TextRecognition::Mode m);
    QPlainTextEdit* textEdit() const { return m_text; }
    const RecognitionResult& lastResult() const { return m_result; }
    QRectF area() const { return m_area; }

    void copyText();
    // Adds the (edited) text as a normal text annotation; returns its id.
    QUuid insertAsText();
    // Applies a replacement variant directly (the dialog calls this after the preview).
    bool applyVariant(Variant v);
    void openReplaceDialog();
    void cancel();

    // Rendering helpers shared with the replace preview (page with annotations, at most maxEdge px).
    static QImage renderPage(const CombinePage& page, int maxEdge);
    // Annotations a variant would add (without touching the document).
    QList<Annotation> annotationsFor(Variant v) const;
    // Background analysis of the selection: colour, and whether it is uniform (no grid/lines/texture).
    struct Background {
        QColor color = Qt::white;
        QColor ink = QColor(20, 20, 20);
        bool uniform = true;
    };
    Background analyzeBackground() const;
    // True when the text looks like formulas or a drawing (recognition unreliable there).
    static bool looksLikeMathOrDrawing(const RecognitionResult& r);

Q_SIGNALS:
    void recognitionFinished(bool ok);

private:
    void updateModuleState();
    bool ensureHandwritingModels(); // asks for consent and downloads; false if not (yet) available
    void onFinished();
    const CombinePage* currentPage() const;

    CombineDocument* m_doc;
    PageCanvas* m_canvas;
    Banner* m_banner;
    QComboBox* m_mode = nullptr;
    QLabel* m_moduleLabel = nullptr;
    QPushButton* m_download = nullptr;
    QPushButton* m_remove = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_status = nullptr;
    QPlainTextEdit* m_text = nullptr;
    QPushButton* m_copy = nullptr;
    QPushButton* m_insert = nullptr;
    QPushButton* m_replace = nullptr;
    QPushButton* m_cancel = nullptr;
    QPointer<HandwritingModelDownloader> m_downloader;
    QFutureWatcher<RecognitionResult>* m_watcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
    RecognitionResult m_result;
    QRectF m_area;
    QUuid m_pageId;
    QImage m_areaImage; // the recognised pixels (for background analysis)
};

} // namespace folvio::combine
