#pragma once

#include "editor/rich/RichDocumentIO.h"

#include <QFutureWatcher>
#include <QGraphicsView>
#include <QPointer>
#include <QTextCharFormat>
#include <QTextEdit>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QGraphicsProxyWidget;
class QGraphicsScene;
class QLabel;
class QLineEdit;
class QToolButton;
class QCheckBox;

namespace folvio {

class Banner;

// A QTextEdit shown as A4 pages inside a zoomable QGraphicsView.
class PagedDocumentView : public QGraphicsView {
    Q_OBJECT
public:
    static constexpr double kPageWidth = 793.7;   // A4 at 96 dpi
    static constexpr double kPageHeight = 1122.5;
    static constexpr double kMargin = 90.7;       // 24 mm

    explicit PagedDocumentView(QWidget* parent = nullptr);
    QTextEdit* editor() const { return m_edit; }
    void setZoom(double z);
    double zoom() const { return m_zoom; }
    int pageCount() const;
    int currentPage() const;
    void goToPage(int page);
    void applyPageSetup();
    void ensureCursorVisible();

Q_SIGNALS:
    void zoomChanged(double zoom);

protected:
    void wheelEvent(QWheelEvent* e) override;
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;

private:
    void updateHeight();
    QGraphicsScene* m_scene;
    QTextEdit* m_edit;
    QGraphicsProxyWidget* m_proxy;
    double m_zoom = 1.0;
};

class RichWorkspace : public QWidget {
    Q_OBJECT
public:
    explicit RichWorkspace(QWidget* parent = nullptr);
    ~RichWorkspace() override;

    // Opens asynchronously; emits opened() or failed().
    void openFile(const QString& path, const QString& format);
    bool hasDocument() const { return !m_path.isEmpty(); }
    QString filePath() const { return m_path; }
    QString format() const { return m_format; }
    bool isModified() const;
    bool maybeClose();
    void closeDocument();
    int pageCount() const;
    int currentPage() const;
    double zoom() const;
    QString saveStatus() const;

    bool save();
    bool saveAs();

Q_SIGNALS:
    void opened();
    void failed(const QString& error);
    void statusChanged();
    void documentClosed();
    void busy(const QString& message, int percent);
    void savedTo(const QString& path);

private:
    void buildUi();
    void onImported();
    void syncFormatControls();
    void mergeFormat(const QTextCharFormat& fmt);
    void setAlignment(Qt::Alignment a);
    void toggleList(bool numbered);
    void setLineSpacing(int percent);
    void insertImage();
    void insertTable();
    void tableOperation(const QString& op);
    void setParagraphStyle(int level);
    void showFind(bool replace);
    void find(bool backwards);
    void replaceOne();
    void replaceAll();
    bool writeTo(const QString& target, const QString& format, bool replaceOriginal);
    void exportAs(const QString& format);

    PagedDocumentView* m_view = nullptr;
    QTextEdit* m_edit = nullptr;
    Banner* m_banner = nullptr;
    QLabel* m_title = nullptr;
    QComboBox* m_styleBox = nullptr;
    QFontComboBox* m_fontBox = nullptr;
    QDoubleSpinBox* m_sizeBox = nullptr;
    QToolButton* m_bold = nullptr;
    QToolButton* m_italic = nullptr;
    QToolButton* m_underline = nullptr;
    QToolButton* m_color = nullptr;
    QToolButton* m_alignL = nullptr;
    QToolButton* m_alignC = nullptr;
    QToolButton* m_alignR = nullptr;
    QToolButton* m_alignJ = nullptr;
    QToolButton* m_bullets = nullptr;
    QToolButton* m_numbers = nullptr;
    QToolButton* m_undo = nullptr;
    QToolButton* m_redo = nullptr;
    QToolButton* m_saveBtn = nullptr;
    QWidget* m_findBar = nullptr;
    QLineEdit* m_findEdit = nullptr;
    QLineEdit* m_replaceEdit = nullptr;
    QCheckBox* m_findCase = nullptr;
    QLabel* m_findStatus = nullptr;
    QWidget* m_replaceRow = nullptr;

    QString m_path;
    QString m_format;
    QString m_workDir;
    bool m_confirmedOverwrite = false;
    bool m_syncing = false;
    QFutureWatcher<RichDocumentIO::Imported>* m_importWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QString m_pendingPath;
    QString m_pendingFormat;
};

} // namespace folvio
