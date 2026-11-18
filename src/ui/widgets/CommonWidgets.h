#pragma once

#include <QFrame>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QToolButton>

class QHBoxLayout;

namespace folvio {

// Large file drop target with a "Choose files…" button.
class DropZone : public QFrame {
    Q_OBJECT
public:
    explicit DropZone(const QString& title, const QString& hint, QWidget* parent = nullptr);
    void setFileFilter(const QString& filter) { m_filter = filter; }
    void setCompact(bool compact);

Q_SIGNALS:
    void filesDropped(const QStringList& paths);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    void chooseFiles();
    bool m_hover = false;
    QString m_filter;
    QLabel* m_icon = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_hint = nullptr;
};

// Inline message bar: info / warning / error / success with optional actions.
class Banner : public QFrame {
    Q_OBJECT
public:
    enum class Kind { Info, Warning, Error, Success };
    explicit Banner(QWidget* parent = nullptr);
    void show(Kind kind, const QString& text, const QStringList& actions = {});
    void setDetails(const QString& text);
    Kind kind() const { return m_kind; }
    bool hasActions() const { return m_hasActions; }

Q_SIGNALS:
    void actionTriggered(int index);
    void dismissed();

private:
    QLabel* m_icon;
    QLabel* m_text;
    QHBoxLayout* m_actionsLayout;
    QList<QPushButton*> m_actionButtons;
    Kind m_kind = Kind::Info;
    bool m_hasActions = false;
};

QFrame* makeSeparator(QWidget* parent = nullptr);
QToolButton* makeToolButton(const QString& icon, const QString& tooltip, QWidget* parent = nullptr,
                            bool checkable = false);
QPushButton* makePrimaryButton(const QString& text, QWidget* parent = nullptr);
QString humanSize(qint64 bytes);
// Opens Explorer with the file selected (or the folder).
void revealInExplorer(const QString& path);

} // namespace folvio
