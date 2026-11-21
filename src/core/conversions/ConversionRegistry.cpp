#include "core/conversions/ConversionRegistry.h"

#include "core/deps/DependencyManager.h"
#include "core/files/FileFormat.h"

#include <QHash>
#include <QSet>

#include <algorithm>

namespace folvio {

bool IConverter::isAvailable(QString* reason) const
{
    for (const QString& t : requiredTools()) {
        if (!DependencyManager::instance().available(t)) {
            if (reason)
                *reason = QObject::tr("%1 is not installed.").arg(DependencyManager::instance().tool(t).displayName);
            return false;
        }
    }
    return true;
}

bool IConverter::supports(const QString& from, const QString& to) const
{
    const auto caps = capabilities();
    return std::any_of(caps.begin(), caps.end(), [&](const auto& c) { return c.from == from && c.to == to; });
}

ConversionCapability IConverter::capability(const QString& from, const QString& to) const
{
    for (const auto& c : capabilities())
        if (c.from == from && c.to == to)
            return c;
    return {};
}

bool ConversionPlan::isLossy() const
{
    return std::any_of(steps.begin(), steps.end(), [](const PlanStep& s) { return s.capability.lossy; });
}

QStringList ConversionPlan::notes() const
{
    QStringList n;
    for (const auto& s : steps)
        if (!s.capability.note.isEmpty())
            n << s.capability.note;
    return n;
}

QString ConversionPlan::describe() const
{
    QStringList parts;
    for (const auto& s : steps)
        parts << QStringLiteral("%1 → %2 (%3)").arg(s.capability.from.toUpper(), s.capability.to.toUpper(),
                                                     s.converter->displayName());
    return parts.join(QStringLiteral(", "));
}

ConversionRegistry& ConversionRegistry::instance()
{
    static ConversionRegistry reg;
    return reg;
}

void ConversionRegistry::add(std::unique_ptr<IConverter> converter)
{
    QMutexLocker lock(&m_mutex);
    for (const auto& c : m_converters)
        if (c->id() == converter->id())
            return;
    m_converters.push_back(std::move(converter));
}

QList<IConverter*> ConversionRegistry::converters() const
{
    QMutexLocker lock(&m_mutex);
    QList<IConverter*> list;
    for (const auto& c : m_converters)
        list << c.get();
    return list;
}

IConverter* ConversionRegistry::byId(const QString& id) const
{
    QMutexLocker lock(&m_mutex);
    for (const auto& c : m_converters)
        if (c->id() == id)
            return c.get();
    return nullptr;
}

QList<PlanStep> ConversionRegistry::directSteps(const QString& from) const
{
    QList<PlanStep> steps;
    for (IConverter* c : converters()) {
        if (!c->isAvailable())
            continue;
        for (const auto& cap : c->capabilities())
            if (cap.from == from && cap.to != from)
                steps.append({c, cap});
    }
    return steps;
}

namespace {

// Formats allowed as intermediate hubs, per hub: which categories may pass through.
bool isAllowedHub(const QString& hub, const QString& from, const QString& to)
{
    const FormatCategory fc = FormatRegistry::category(from);
    const FormatCategory tc = FormatRegistry::category(to);
    if (hub == "pdf") // office -> pdf -> raster images
        return (fc == FormatCategory::Presentation || fc == FormatCategory::Document || fc == FormatCategory::Spreadsheet)
               && tc == FormatCategory::Image;
    if (hub == "png") // image format bridges, e.g. HEIC -> PNG -> PDF
        return fc == FormatCategory::Image && (tc == FormatCategory::Pdf || tc == FormatCategory::Image);
    if (hub == "html") // markdown <-> office documents
        return (from == "md" && (tc == FormatCategory::Document)) || (fc == FormatCategory::Document && to == "md");
    if (hub == "csv") // spreadsheet <-> json
        return (fc == FormatCategory::Spreadsheet && to == "json") || (from == "json" && tc == FormatCategory::Spreadsheet);
    if (hub == "docx") // legacy/odf documents -> pdf-like targets handled directly; allow doc->docx->md? no.
        return false;
    return false;
}

int stepScore(const ConversionCapability& c)
{
    return c.fidelity - (c.lossy ? 10 : 0);
}

} // namespace

ConversionPlan ConversionPlanner::plan(const QString& from, const QString& to) const
{
    ConversionPlan best;
    best.from = from;
    best.to = to;
    if (from.isEmpty() || to.isEmpty() || from == to)
        return best;

    const QList<PlanStep> first = m_registry.directSteps(from);
    // Direct route.
    for (const PlanStep& s : first) {
        if (s.capability.to != to)
            continue;
        const int score = 1000 + stepScore(s.capability);
        if (score > best.score) {
            best.steps = {s};
            best.score = score;
        }
    }
    if (best.isValid())
        return best;

    // One intermediate hop.
    for (const PlanStep& s1 : first) {
        const QString hub = s1.capability.to;
        if (!isAllowedHub(hub, from, to))
            continue;
        for (const PlanStep& s2 : m_registry.directSteps(hub)) {
            if (s2.capability.to != to)
                continue;
            // Never decode lossy -> re-encode lossy -> re-encode again.
            if (s1.capability.lossy && s2.capability.lossy)
                continue;
            const int score = 500 + std::min(stepScore(s1.capability), stepScore(s2.capability));
            if (score > best.score) {
                best.steps = {s1, s2};
                best.score = score;
            }
        }
    }
    return best;
}

QList<ConversionPlan> ConversionPlanner::availableTargets(const QString& from) const
{
    QSet<QString> targets;
    for (const PlanStep& s : m_registry.directSteps(from)) {
        targets.insert(s.capability.to);
        for (const PlanStep& s2 : m_registry.directSteps(s.capability.to))
            if (isAllowedHub(s.capability.to, from, s2.capability.to))
                targets.insert(s2.capability.to);
    }
    targets.remove(from);
    QList<ConversionPlan> plans;
    for (const QString& t : targets) {
        ConversionPlan p = plan(from, t);
        if (p.isValid())
            plans << p;
    }
    // Stable, predictable ordering: by category of target, then by id.
    std::sort(plans.begin(), plans.end(), [](const ConversionPlan& a, const ConversionPlan& b) {
        const int ca = int(FormatRegistry::category(a.to)), cb = int(FormatRegistry::category(b.to));
        return ca != cb ? ca < cb : a.to < b.to;
    });
    return plans;
}

} // namespace folvio
