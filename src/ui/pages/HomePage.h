#pragma once

#include <QWidget>

class QTreeWidget;

namespace folvio {

class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(QWidget* parent = nullptr);
    void refresh();

Q_SIGNALS:
    void filesDropped(const QStringList& paths);
    void openForEditing(const QString& path);
    void openForConversion(const QString& path);
    void navigate(int page);

private:
    QTreeWidget* m_recent = nullptr;
};

} // namespace folvio
