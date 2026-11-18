#pragma once

#include "core/conversions/ConversionRegistry.h"

#include <QLayout>
#include <QMap>
#include <QStyle>
#include <QWidget>

class QButtonGroup;
class QFormLayout;
class QLabel;
class QRadioButton;
class QVBoxLayout;

namespace folvio {

// Simple left-to-right wrapping layout (format chips).
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int spacing = 6);
    ~FlowLayout() override;
    void addItem(QLayoutItem* item) override;
    int count() const override { return int(m_items.size()); }
    QLayoutItem* itemAt(int i) const override { return m_items.value(i); }
    QLayoutItem* takeAt(int i) override { return i >= 0 && i < m_items.size() ? m_items.takeAt(i) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return doLayout(QRect(0, 0, w, 0), true); }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override;
    void setGeometry(const QRect& r) override
    {
        QLayout::setGeometry(r);
        doLayout(r, false);
    }

private:
    int doLayout(const QRect& rect, bool testOnly) const;
    QList<QLayoutItem*> m_items;
    int m_spacing;
};

// Target format chooser + options + output location. Used by Convert and Batch.
class ConversionSetupPanel : public QWidget {
    Q_OBJECT
public:
    explicit ConversionSetupPanel(QWidget* parent = nullptr);

    // Source formats of the files to convert. Targets reachable from at least one
    // source are offered; count shows how many files support each target.
    void setSourceFormats(const QStringList& formats);
    QString targetFormat() const { return m_target; }
    QVariantMap options() const;
    QString outputDirectory() const; // empty = next to each source file
    ConversionPlan planFor(const QString& sourceFormat) const;
    bool hasTargets() const { return !m_targets.isEmpty(); }

Q_SIGNALS:
    void targetChanged(const QString& format);

private:
    void rebuildChips();
    void rebuildOptions();
    void chooseFolder();

    QStringList m_sources;
    QMap<QString, int> m_targets; // target -> number of compatible sources
    QString m_target;
    QWidget* m_chipsHost = nullptr;
    QVBoxLayout* m_chipsLayout = nullptr;
    QButtonGroup* m_group = nullptr;
    QLabel* m_route = nullptr;
    QLabel* m_notes = nullptr;
    QWidget* m_optionsHost = nullptr;
    QFormLayout* m_optionsForm = nullptr;
    QMap<QString, QWidget*> m_optionWidgets;
    QMap<QString, OptionSpec> m_optionSpecs;
    QRadioButton* m_sameFolder = nullptr;
    QRadioButton* m_customFolder = nullptr;
    QLabel* m_folderLabel = nullptr;
    QString m_folder;
};

} // namespace folvio
