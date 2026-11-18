#pragma once

#include <QMainWindow>
#include <QPointer>

class QButtonGroup;
class QLabel;
class QProgressBar;
class QStackedWidget;
class QToolButton;

namespace folvio {

class BatchPage;
class ConvertPage;
class DropZone;
class HomePage;
class JobQueue;
class PdfWorkspace;
class RecentPage;
class RichWorkspace;
class SettingsPage;
namespace combine {
class CombineWorkspace;
}

// AUTO CHANGE page: empty state, native PDF workspace or reflow document workspace.
class WorkspacePage : public QWidget {
    Q_OBJECT
public:
    explicit WorkspacePage(QWidget* parent = nullptr);
    bool openDocument(const QString& path, bool reflow = false);
    bool maybeClose();
    bool hasDocument() const;
    PdfWorkspace* pdf() const { return m_pdf; }
    RichWorkspace* rich() const { return m_rich; }
    QString statusFormat() const;
    int pageCount() const;
    int currentPage() const;
    double zoom() const;
    QString saveStatus() const;

Q_SIGNALS:
    void statusChanged();
    void busy(const QString& message, int percent);
    void documentOpened(const QString& path);
    void message(const QString& text);

private:
    QStackedWidget* m_stack = nullptr;
    DropZone* m_drop = nullptr;
    PdfWorkspace* m_pdf = nullptr;
    RichWorkspace* m_rich = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    enum Page { Home = 0, Convert, AutoChange, Batch, Recent, Settings, Combine };
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFiles(const QStringList& paths);
    void openForEditing(const QString& path);
    void openForConversion(const QString& path);
    void showPage(Page page);
    void demoEditFirstParagraph(); // diagnostics: opens the inline editor on the first paragraph
    combine::CombineWorkspace* combineWorkspace() const { return m_combine; }

protected:
    void closeEvent(QCloseEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    QWidget* buildSidebar();
    void buildStatusBar();
    void updateStatus();
    void restoreGeometryFromSettings();

    JobQueue* m_queue = nullptr;
    QStackedWidget* m_pages = nullptr;
    QButtonGroup* m_nav = nullptr;
    HomePage* m_home = nullptr;
    ConvertPage* m_convert = nullptr;
    WorkspacePage* m_workspace = nullptr;
    BatchPage* m_batch = nullptr;
    RecentPage* m_recent = nullptr;
    SettingsPage* m_settings = nullptr;
    combine::CombineWorkspace* m_combine = nullptr;
    QLabel* m_brand = nullptr;
    QLabel* m_statusFormat = nullptr;
    QLabel* m_statusPages = nullptr;
    QLabel* m_statusSave = nullptr;
    QLabel* m_statusZoom = nullptr;
    QLabel* m_statusJobs = nullptr;
    QLabel* m_statusMessage = nullptr;
    QProgressBar* m_statusProgress = nullptr;
};

} // namespace folvio
