#pragma once

#include "core/files/FileFormat.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace folvio {

class Banner;
class ConversionJob;
class ConversionSetupPanel;
class DropZone;
class JobQueue;

// Focused single-file converter.
class ConvertPage : public QWidget {
    Q_OBJECT
public:
    ConvertPage(JobQueue* queue, QWidget* parent = nullptr);
    void setFile(const QString& path);

Q_SIGNALS:
    void multipleFilesDropped(const QStringList& paths);
    void openForEditing(const QString& path);
    void statusMessage(const QString& msg);

private:
    void start();
    void onJobProgress(double f, const QString& stage);
    void onJobFinished();

    JobQueue* m_queue;
    QStackedWidget* m_stack = nullptr;
    DropZone* m_drop = nullptr;
    QLabel* m_fileIcon = nullptr;
    QLabel* m_fileName = nullptr;
    QLabel* m_fileMeta = nullptr;
    Banner* m_banner = nullptr;
    ConversionSetupPanel* m_setup = nullptr;
    QPushButton* m_convert = nullptr;
    QPushButton* m_cancel = nullptr;
    QPushButton* m_edit = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_stage = nullptr;
    DetectedFile m_file;
    QPointer<ConversionJob> m_job;
};

// Many files, one target format, observable queue.
class BatchPage : public QWidget {
    Q_OBJECT
public:
    BatchPage(JobQueue* queue, QWidget* parent = nullptr);
    void addFiles(const QStringList& paths);

Q_SIGNALS:
    void statusMessage(const QString& msg);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void refreshTargets();
    void updateRow(QTreeWidgetItem* item);
    void startAll();
    void retryFailed();
    void showContextMenu(const QPoint& pos);
    QTreeWidgetItem* itemForJob(ConversionJob* job) const;
    void updateSummary();

    JobQueue* m_queue;
    QTreeWidget* m_table = nullptr;
    ConversionSetupPanel* m_setup = nullptr;
    QPushButton* m_start = nullptr;
    QPushButton* m_cancelAll = nullptr;
    QPushButton* m_retry = nullptr;
    QPushButton* m_openFolder = nullptr;
    QLabel* m_summary = nullptr;
    QString m_lastOutputDir;
};

} // namespace folvio
