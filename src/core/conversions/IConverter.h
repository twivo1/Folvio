#pragma once

#include "core/process/ProcessRunner.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <atomic>
#include <functional>

namespace folvio {

// A user-tunable option exposed by a converter for a given format pair.
struct OptionSpec {
    enum class Type { Choice, Integer, Boolean };
    QString key;
    QString label;
    Type type = Type::Choice;
    QStringList choiceValues;   // machine values
    QStringList choiceLabels;   // human labels (same length)
    QVariant defaultValue;
    int minimum = 0;
    int maximum = 100;
    QString tooltip;
};

struct ConversionCapability {
    QString from;
    QString to;
    int fidelity = 80;      // 0..100, used by the planner to choose engines
    bool lossy = false;     // the step loses information (re-encoding, flattening, ...)
    QString note;           // shown to the user, e.g. "Layout is approximated"
};

struct ConversionRequest {
    QString inputPath;
    QString inputFormat;
    QString outputFormat;
    QString outputPath;      // desired final path (may be adjusted to avoid conflicts)
    QVariantMap options;
};

struct ConversionResult {
    bool ok = false;
    bool cancelled = false;
    QString error;
    QStringList outputs;     // produced files (multi-page exports produce several)
    QStringList warnings;

    static ConversionResult failure(const QString& e) { ConversionResult r; r.error = e; return r; }
};

// Execution environment given to a converter. Converters run on worker threads.
class ConversionContext {
public:
    using ProgressFn = std::function<void(double fraction, const QString& stage)>;

    ConversionContext(QString tempDir, ProgressFn progress, const std::atomic_bool* cancelFlag)
        : m_tempDir(std::move(tempDir)), m_progress(std::move(progress)), m_cancel(cancelFlag) {}

    const QString& tempDir() const { return m_tempDir; }
    bool isCancelled() const { return m_cancel && m_cancel->load(); }
    void progress(double fraction, const QString& stage = {}) const
    {
        if (m_progress)
            m_progress(qBound(0.0, fraction, 1.0), stage);
    }
    // Maps a sub-step's 0..1 progress into a range of the overall progress.
    void setProgressRange(double from, double to) { m_rangeFrom = from; m_rangeTo = to; }
    void stepProgress(double fraction, const QString& stage = {}) const
    {
        progress(m_rangeFrom + (m_rangeTo - m_rangeFrom) * qBound(0.0, fraction, 1.0), stage);
    }
    ProcessResult run(ProcessSpec spec) const
    {
        return ProcessRunner::run(spec, [this] { return isCancelled(); });
    }

private:
    QString m_tempDir;
    ProgressFn m_progress;
    const std::atomic_bool* m_cancel = nullptr;
    double m_rangeFrom = 0.0;
    double m_rangeTo = 1.0;
};

class IConverter {
public:
    virtual ~IConverter() = default;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual QList<ConversionCapability> capabilities() const = 0;

    // External tools needed (ids understood by DependencyManager).
    virtual QStringList requiredTools() const { return {}; }
    virtual bool isAvailable(QString* reason = nullptr) const;

    virtual QList<OptionSpec> options(const QString& from, const QString& to) const
    {
        Q_UNUSED(from) Q_UNUSED(to) return {};
    }

    // Converts request.inputPath into request.outputPath. Must honour
    // ctx.isCancelled(), report progress, and never write anywhere except
    // request.outputPath (or files derived from it for multi-output) and ctx.tempDir().
    virtual ConversionResult convert(const ConversionRequest& request, ConversionContext& ctx) = 0;

    bool supports(const QString& from, const QString& to) const;
    ConversionCapability capability(const QString& from, const QString& to) const;
};

} // namespace folvio
