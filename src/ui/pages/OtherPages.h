#pragma once

#include <QWidget>

class QTreeWidget;
class QTableWidget;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QButtonGroup;

namespace folvio {

class RecentPage : public QWidget {
    Q_OBJECT
public:
    explicit RecentPage(QWidget* parent = nullptr);
    void refresh();

Q_SIGNALS:
    void openForEditing(const QString& path);
    void openForConversion(const QString& path);

private:
    QTreeWidget* m_recent = nullptr;
    QTreeWidget* m_history = nullptr;
};

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(QWidget* parent = nullptr);
    void refreshEngines();

Q_SIGNALS:
    void themeChanged();

private:
    QTableWidget* m_engines = nullptr;
    QButtonGroup* m_theme = nullptr;
};

} // namespace folvio
