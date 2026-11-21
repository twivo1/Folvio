#pragma once

#include "combine/CombineModel.h"
#include "combine/CombinePdfExporter.h"
#include "combine/CombineSession.h"
#include "combine/PageCanvas.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QWidget>

#include <atomic>
#include <memory>

class QAction;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListView;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QFontComboBox;
class QFormLayout;
class QLayout;

namespace folvio {
class Banner;
class DropZone;
}

namespace folvio::combine {

// COMBINE: images -> arranged, straightened, annotated, multi-page PDF.
class CombineWorkspace : public QWidget {
    Q_OBJECT
public:
    enum class Step { Arrange = 0, Edit = 1, Export = 2 };
    // Combine: the multi-image workflow. ImageEditor: Auto Change for a single image or screenshot
    // (Annotate, Recognize Text, Convert to PDF, Export Image) - same canvas, tools and exporter,
    // no Arrange step, no Combine session/recovery.
    enum class Mode { Combine, ImageEditor };

    explicit CombineWorkspace(QWidget* parent = nullptr, Mode mode = Mode::Combine);
    Mode mode() const { return m_mode; }
    // Image editor entry points.
    void openImageFile(const QString& path);
    void openImage(const QImage& image, const QString& name);
    bool hasImage() const { return !m_doc->isEmpty(); }
    // Writes the current page with its annotations (crop, perspective included) as PNG/JPG/WebP/...
    bool exportImageTo(const QString& path, QString* error = nullptr);
    void closeImage(); // without asking
    ~CombineWorkspace() override;

    CombineDocument* document() const { return m_doc; }
    PageCanvas* canvas() const { return m_canvas; }
    PageListModel* model() const { return m_model; }
    void setStep(Step s);
    // Selects an editor tool exactly like its toolbar button.
    void setTool(PageCanvas::Tool tool);
    // Diagnostics (--combine-demo): straighten page 1 and add sample annotations.
    void runDiagnosticDemo();
    Step step() const { return m_step; }

    // Import entry points (also used by tests).
    void importFiles(const QStringList& paths);
    void importFiles(const QStringList& paths, int position); // position < 0: append
    // Moves the given rows before 'insertIndex' (index in the current order); used by drag & drop,
    // keyboard and buttons alike.
    void reorderPages(const QList<int>& rows, int insertIndex);
    QListView* arrangeView() const;
    QListView* editPageList() const;
    class RecognizePanel* recognizePanel() const { return m_recognize; }
    bool pasteFromClipboard();
    bool importInProgress() const;
    bool exportInProgress() const;
    // Export without a file dialog (tests / automation).
    void exportToPath(const QString& path, bool overwrite);
    QString lastExportPath() const { return m_lastExport; }
    QString lastExportError() const { return m_lastExportError; }

    // Called by the main window before closing; returns false to keep the app open.
    bool prepareClose();
    void offerSessionRecovery();

Q_SIGNALS:
    void statusMessage(const QString& msg);
    void exportFinished(bool ok);
    void importFinished(int added);
    void imageClosed();                 // image editor: back to the Auto Change start page
    void recognizeTextRequested();      // image editor: the Recognize Text tool was chosen

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* buildHeader();
    QWidget* buildArrange();
    QWidget* buildEdit();
    QWidget* buildExport();
    QWidget* buildOptionsPanel();
    void connectPageView(class PageGridView* view);
    void updateActions();
    void requestThumbnail(const QUuid& id);
    void onImported(const ImportResult& r, int insertAt);
    void openEditor(int row);
    void selectPageInEdit(int row);
    QList<int> selectedRows(QListView* view) const;
    void syncOptionsPanel();
    void applyStyleFromPanel(const QString& description);
    void refreshExportSummary();
    void readExportSettingsFromUi();
    void startExport(const QString& path, bool overwrite);
    void onExportDone();
    void scheduleAutosave();
    void saveProject();
    void openProject();
    void newProject();
    void zoomBy(double factor);
    void fitPage();

    void exportImageInteractive();
    Mode m_mode = Mode::Combine;
    QString m_assetsDir; // empty: Combine session assets
    QLabel* m_title = nullptr;
    class RecognizePanel* m_recognize = nullptr;
    CombineDocument* m_doc = nullptr;
    PageListModel* m_model = nullptr;
    Step m_step = Step::Arrange;
    QStackedWidget* m_steps = nullptr;
    QButtonGroup* m_stepButtons = nullptr;
    Banner* m_banner = nullptr;
    QToolButton* m_undo = nullptr;
    QToolButton* m_redo = nullptr;
    QTimer* m_autosave = nullptr;

    // Arrange
    QStackedWidget* m_arrangeStack = nullptr;
    DropZone* m_drop = nullptr;
    class PageGridView* m_grid = nullptr;
    QPushButton* m_btnRemove = nullptr;
    QPushButton* m_btnDuplicate = nullptr;
    QPushButton* m_btnUp = nullptr;
    QPushButton* m_btnDown = nullptr;
    QPushButton* m_btnEdit = nullptr;
    QPushButton* m_btnSort = nullptr;
    QPushButton* m_btnNextEdit = nullptr;
    QAction* m_saveProjectAction = nullptr;
    QLabel* m_arrangeInfo = nullptr;

    // Edit
    class PageGridView* m_pageList = nullptr;
    QScrollArea* m_scroll = nullptr;
    PageCanvas* m_canvas = nullptr;
    QButtonGroup* m_tools = nullptr;
    QToolButton* m_shapeButton = nullptr;
    QStackedWidget* m_options = nullptr;
    QLabel* m_editInfo = nullptr;
    // perspective panel
    QButtonGroup* m_cornerButtons = nullptr;
    QDoubleSpinBox* m_cornerX = nullptr;
    QDoubleSpinBox* m_cornerY = nullptr;
    QComboBox* m_aspect = nullptr;
    QDoubleSpinBox* m_customRatio = nullptr;
    QDoubleSpinBox* m_marginPct = nullptr;
    QButtonGroup* m_compare = nullptr;
    QLabel* m_quadStatus = nullptr;
    QPushButton* m_straighten = nullptr;
    QPushButton* m_removeCorrection = nullptr;
    // style panel
    QWidget* m_stylePanel = nullptr;
    QFormLayout* m_styleForm = nullptr;
    QLayout* m_widthRow = nullptr;
    QLayout* m_opacityRow = nullptr;
    QLayout* m_fillRow = nullptr;
    QLabel* m_styleTitle = nullptr;
    QToolButton* m_strokeColor = nullptr;
    QSlider* m_width = nullptr;
    QLabel* m_widthLabel = nullptr;
    QSlider* m_opacity = nullptr;
    QLabel* m_opacityLabel = nullptr;
    QCheckBox* m_smooth = nullptr;
    QCheckBox* m_fillEnabled = nullptr;
    QToolButton* m_fillColor = nullptr;
    QWidget* m_textOptions = nullptr;
    QComboBox* m_fontFamily = nullptr;
    QDoubleSpinBox* m_fontSize = nullptr;
    QToolButton* m_bold = nullptr;
    QToolButton* m_italic = nullptr;
    QToolButton* m_underline = nullptr;
    QButtonGroup* m_align = nullptr;
    QWidget* m_selectionActions = nullptr;
    QLabel* m_styleHint = nullptr;
    bool m_syncing = false;
    ToolStyle m_penStyle, m_highlighterStyle, m_shapeStyle, m_textStyle;

    // Export
    QTreeWidget* m_summary = nullptr;
    QLabel* m_estimate = nullptr;
    QComboBox* m_pageSize = nullptr;
    QDoubleSpinBox* m_customW = nullptr;
    QDoubleSpinBox* m_customH = nullptr;
    QComboBox* m_orientation = nullptr;
    QComboBox* m_margins = nullptr;
    QDoubleSpinBox* m_customMargin = nullptr;
    QComboBox* m_quality = nullptr;
    QPushButton* m_exportButton = nullptr;
    QPushButton* m_cancelExport = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_progressLabel = nullptr;
    Banner* m_exportBanner = nullptr;

    QFutureWatcher<ExportResult>* m_exportWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_exportCancel;
    QString m_lastExport;
    QString m_lastExportError;
    int m_importsRunning = 0;
    bool m_sessionOffered = false;
};

} // namespace folvio::combine
