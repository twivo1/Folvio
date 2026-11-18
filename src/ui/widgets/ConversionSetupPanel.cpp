#include "ui/widgets/ConversionSetupPanel.h"

#include "core/files/FileFormat.h"
#include "core/persistence/Database.h"
#include "ui/widgets/Icons.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace folvio {

FlowLayout::FlowLayout(QWidget* parent, int spacing) : QLayout(parent), m_spacing(spacing)
{
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem* item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem* item)
{
    m_items.append(item);
}

QSize FlowLayout::minimumSize() const
{
    QSize s;
    for (QLayoutItem* i : m_items)
        s = s.expandedTo(i->minimumSize());
    return s;
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const
{
    int x = rect.x(), y = rect.y(), lineHeight = 0;
    for (QLayoutItem* item : m_items) {
        const QSize sz = item->sizeHint();
        if (x + sz.width() > rect.right() + 1 && lineHeight > 0) {
            x = rect.x();
            y += lineHeight + m_spacing;
            lineHeight = 0;
        }
        if (!testOnly)
            item->setGeometry(QRect(QPoint(x, y), sz));
        x += sz.width() + m_spacing;
        lineHeight = std::max(lineHeight, sz.height());
    }
    return y + lineHeight - rect.y();
}

// ---------------------------------------------------------------------------

ConversionSetupPanel::ConversionSetupPanel(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);
    auto* title = new QLabel(tr("Convert to"), this);
    title->setObjectName("SectionTitle");
    root->addWidget(title);
    m_chipsHost = new QWidget(this);
    m_chipsLayout = new QVBoxLayout(m_chipsHost);
    m_chipsLayout->setContentsMargins(0, 0, 0, 0);
    m_chipsLayout->setSpacing(8);
    root->addWidget(m_chipsHost);
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);

    m_route = new QLabel(this);
    m_route->setWordWrap(true);
    m_route->setProperty("muted", true);
    m_notes = new QLabel(this);
    m_notes->setWordWrap(true);
    root->addWidget(m_route);
    root->addWidget(m_notes);

    auto* optTitle = new QLabel(tr("Options"), this);
    optTitle->setObjectName("SectionTitle");
    m_optionsHost = new QWidget(this);
    m_optionsForm = new QFormLayout(m_optionsHost);
    m_optionsForm->setContentsMargins(0, 0, 0, 0);
    m_optionsForm->setLabelAlignment(Qt::AlignLeft);
    m_optionsForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    root->addWidget(optTitle);
    root->addWidget(m_optionsHost);

    auto* outTitle = new QLabel(tr("Save to"), this);
    outTitle->setObjectName("SectionTitle");
    root->addWidget(outTitle);
    m_sameFolder = new QRadioButton(tr("Same folder as the source file"), this);
    m_customFolder = new QRadioButton(tr("Folder:"), this);
    auto* row = new QHBoxLayout;
    m_folderLabel = new QLabel(this);
    m_folderLabel->setProperty("muted", true);
    m_folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_folderLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* choose = new QPushButton(tr("Choose…"), this);
    row->addWidget(m_customFolder);
    row->addWidget(m_folderLabel, 1);
    row->addWidget(choose);
    root->addWidget(m_sameFolder);
    root->addLayout(row);
    m_folder = Database::instance().setting("convert.outputDir").toString();
    m_sameFolder->setChecked(m_folder.isEmpty());
    m_customFolder->setChecked(!m_folder.isEmpty());
    m_folderLabel->setText(m_folder.isEmpty() ? tr("(not chosen)") : QDir::toNativeSeparators(m_folder));
    connect(choose, &QPushButton::clicked, this, &ConversionSetupPanel::chooseFolder);
    connect(m_customFolder, &QRadioButton::toggled, this, [this](bool on) {
        if (on && m_folder.isEmpty())
            chooseFolder();
    });
    connect(m_group, &QButtonGroup::idClicked, this, [this](int) {
        auto* b = m_group->checkedButton();
        m_target = b ? b->property("format").toString() : QString();
        rebuildOptions();
        Q_EMIT targetChanged(m_target);
    });
}

void ConversionSetupPanel::chooseFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Output folder"), m_folder);
    if (dir.isEmpty()) {
        if (m_folder.isEmpty())
            m_sameFolder->setChecked(true);
        return;
    }
    m_folder = dir;
    m_folderLabel->setText(QDir::toNativeSeparators(dir));
    m_customFolder->setChecked(true);
    Database::instance().setSetting("convert.outputDir", dir);
}

QString ConversionSetupPanel::outputDirectory() const
{
    return m_customFolder->isChecked() ? m_folder : QString();
}

void ConversionSetupPanel::setSourceFormats(const QStringList& formats)
{
    m_sources = formats;
    m_sources.removeDuplicates();
    m_targets.clear();
    ConversionPlanner planner;
    for (const QString& f : m_sources)
        for (const ConversionPlan& p : planner.availableTargets(f))
            m_targets[p.to] += int(formats.count(f));
    if (!m_targets.contains(m_target))
        m_target.clear();
    rebuildChips();
    rebuildOptions();
}

ConversionPlan ConversionSetupPanel::planFor(const QString& sourceFormat) const
{
    return ConversionPlanner().plan(sourceFormat, m_target);
}

void ConversionSetupPanel::rebuildChips()
{
    for (QAbstractButton* b : m_group->buttons())
        m_group->removeButton(b);
    while (QLayoutItem* item = m_chipsLayout->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    if (m_targets.isEmpty()) {
        auto* l = new QLabel(m_sources.isEmpty() ? tr("Add a file to see the formats it can be converted to.")
                                                 : tr("No conversion is available for this file type with the installed engines. "
                                                      "See Settings → Engines."),
                             m_chipsHost);
        l->setWordWrap(true);
        l->setProperty("muted", true);
        m_chipsLayout->addWidget(l);
        return;
    }
    // Group targets by category for scanability.
    QMap<int, QStringList> byCategory;
    for (auto it = m_targets.cbegin(); it != m_targets.cend(); ++it)
        byCategory[int(FormatRegistry::category(it.key()))] << it.key();
    int id = 0;
    for (auto it = byCategory.cbegin(); it != byCategory.cend(); ++it) {
        auto* label = new QLabel(categoryName(FormatCategory(it.key())), m_chipsHost);
        label->setObjectName("FormatGroupLabel");
        m_chipsLayout->addWidget(label);
        auto* host = new QWidget(m_chipsHost);
        auto* flow = new FlowLayout(host, 6);
        for (const QString& f : it.value()) {
            auto* chip = new QToolButton(host);
            chip->setObjectName("FormatChip");
            chip->setCheckable(true);
            chip->setText(f.toUpper());
            chip->setProperty("format", f);
            QString tip = FormatRegistry::displayName(f);
            if (m_sources.size() > 1)
                tip += "\n" + tr("%n file(s) can be converted to this format", nullptr, m_targets.value(f));
            chip->setToolTip(tip);
            chip->setCursor(Qt::PointingHandCursor);
            chip->setChecked(f == m_target);
            m_group->addButton(chip, id++);
            flow->addWidget(chip);
        }
        m_chipsLayout->addWidget(host);
    }
}

void ConversionSetupPanel::rebuildOptions()
{
    while (QLayoutItem* item = m_optionsForm->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    m_optionWidgets.clear();
    m_optionSpecs.clear();
    m_route->clear();
    m_notes->clear();
    if (m_target.isEmpty()) {
        m_optionsHost->parentWidget()->update();
        return;
    }
    // Describe the route for the first compatible source and collect options from all steps.
    QStringList notes;
    for (const QString& src : m_sources) {
        const ConversionPlan plan = ConversionPlanner().plan(src, m_target);
        if (!plan.isValid())
            continue;
        if (m_route->text().isEmpty())
            m_route->setText(tr("Route: %1").arg(plan.describe()));
        notes << plan.notes();
        if (plan.isLossy())
            notes << tr("This conversion re-encodes data and may reduce quality.");
        for (const PlanStep& step : plan.steps)
            for (const OptionSpec& o : step.converter->options(step.capability.from, step.capability.to))
                if (!m_optionSpecs.contains(o.key))
                    m_optionSpecs.insert(o.key, o);
    }
    notes.removeDuplicates();
    if (!notes.isEmpty())
        m_notes->setText("⚠ " + notes.join("\n⚠ "));
    for (const OptionSpec& o : std::as_const(m_optionSpecs)) {
        QWidget* w = nullptr;
        switch (o.type) {
        case OptionSpec::Type::Choice: {
            auto* c = new QComboBox(m_optionsHost);
            for (int i = 0; i < o.choiceValues.size(); ++i)
                c->addItem(o.choiceLabels.value(i, o.choiceValues[i]), o.choiceValues[i]);
            c->setCurrentIndex(std::max(0, c->findData(o.defaultValue)));
            w = c;
            break;
        }
        case OptionSpec::Type::Integer: {
            auto* s = new QSpinBox(m_optionsHost);
            s->setRange(o.minimum, o.maximum);
            s->setValue(o.defaultValue.toInt());
            w = s;
            break;
        }
        case OptionSpec::Type::Boolean: {
            auto* c = new QCheckBox(m_optionsHost);
            c->setChecked(o.defaultValue.toBool());
            w = c;
            break;
        }
        }
        w->setToolTip(o.tooltip);
        m_optionsForm->addRow(o.label, w);
        m_optionWidgets.insert(o.key, w);
    }
    if (m_optionSpecs.isEmpty()) {
        auto* l = new QLabel(tr("No options for this conversion."), m_optionsHost);
        l->setProperty("muted", true);
        m_optionsForm->addRow(l);
    }
}

QVariantMap ConversionSetupPanel::options() const
{
    QVariantMap m;
    for (auto it = m_optionWidgets.cbegin(); it != m_optionWidgets.cend(); ++it) {
        if (auto* c = qobject_cast<QComboBox*>(it.value()))
            m.insert(it.key(), c->currentData());
        else if (auto* s = qobject_cast<QSpinBox*>(it.value()))
            m.insert(it.key(), s->value());
        else if (auto* b = qobject_cast<QCheckBox*>(it.value()))
            m.insert(it.key(), b->isChecked());
    }
    return m;
}

} // namespace folvio
