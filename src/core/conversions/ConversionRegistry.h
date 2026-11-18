#pragma once

#include "core/conversions/IConverter.h"

#include <QList>
#include <QMutex>

#include <memory>
#include <vector>

namespace folvio {

struct PlanStep {
    IConverter* converter = nullptr;
    ConversionCapability capability;
};

struct ConversionPlan {
    QString from;
    QString to;
    QList<PlanStep> steps;
    int score = 0;

    bool isValid() const { return !steps.isEmpty(); }
    bool isDirect() const { return steps.size() == 1; }
    bool isLossy() const;
    QStringList notes() const;
    QString describe() const; // "PDF → DOCX (Microsoft Word)"
};

class ConversionRegistry {
public:
    static ConversionRegistry& instance();
    // Registers the built-in converters (idempotent).
    static void registerBuiltins();

    void add(std::unique_ptr<IConverter> converter);
    QList<IConverter*> converters() const;
    IConverter* byId(const QString& id) const;

    // Direct capabilities whose converter is currently available.
    QList<PlanStep> directSteps(const QString& from) const;

private:
    mutable QMutex m_mutex;
    std::vector<std::unique_ptr<IConverter>> m_converters;
};

// Chooses the best route between two formats. Direct routes are preferred; a
// single intermediate format is allowed only through a small whitelist of
// "hub" formats and never when it would re-encode lossy data twice or upsample
// lossy data into a lossless container pointlessly.
class ConversionPlanner {
public:
    explicit ConversionPlanner(const ConversionRegistry& registry = ConversionRegistry::instance())
        : m_registry(registry) {}

    ConversionPlan plan(const QString& from, const QString& to) const;
    QList<ConversionPlan> availableTargets(const QString& from) const; // one best plan per target

private:
    const ConversionRegistry& m_registry;
};

} // namespace folvio
