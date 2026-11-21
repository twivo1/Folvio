#include "core/jobs/ConversionJob.h"

#include "core/conversions/OutputValidator.h"
#include "core/files/FileFormat.h"
#include "core/files/PathUtils.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>

namespace folvio {

namespace {

bool moveFile(const QString& from, const QString& to)
{
    if (QFile::rename(from, to))
        return true;
    // Cross-volume: copy, verify size, then remove the temp copy.
    if (!QFile::copy(from, to))
        return false;
    if (QFileInfo(from).size() != QFileInfo(to).size()) {
        QFile::remove(to);
        return false;
    }
    QFile::remove(from);
    return true;
}

} // namespace

ConversionResult ConversionExecutor::execute(const ConversionRequest& request, const ConversionPlan& plan,
                                             const std::atomic_bool* cancel,
                                             const ConversionContext::ProgressFn& progress)
{
    if (!plan.isValid())
        return ConversionResult::failure(QObject::tr("No conversion route from %1 to %2.")
                                             .arg(request.inputFormat.toUpper(), request.outputFormat.toUpper()));
    if (!QFileInfo::exists(request.inputPath))
        return ConversionResult::failure(QObject::tr("Input file no longer exists."));
    if (PathUtils::samePath(request.inputPath, request.outputPath))
        return ConversionResult::failure(QObject::tr("Output would overwrite the input file."));

    const QString tempDir = PathUtils::tempRoot() + "/job-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(tempDir);
    struct TempCleanup {
        QString dir;
        ~TempCleanup() { QDir(dir).removeRecursively(); }
    } cleanup{tempDir};

    ConversionContext ctx(tempDir, progress, cancel);
    const QString finalBase = PathUtils::sanitizeFileName(QFileInfo(request.outputPath).completeBaseName());
    QString currentInput = request.inputPath;
    QString currentFormat = request.inputFormat;
    ConversionResult last;
    QStringList warnings;

    const int n = int(plan.steps.size());
    for (int i = 0; i < n; ++i) {
        const PlanStep& step = plan.steps[i];
        const bool isLast = i == n - 1;
        const QString stepDir = tempDir + QStringLiteral("/step%1").arg(i);
        QDir().mkpath(stepDir);
        ConversionRequest sub;
        sub.inputPath = currentInput;
        sub.inputFormat = currentFormat;
        sub.outputFormat = step.capability.to;
        sub.outputPath = stepDir + "/" + (isLast ? finalBase : QStringLiteral("intermediate")) + "."
                         + FormatRegistry::preferredExtension(step.capability.to);
        sub.options = request.options;
        if (!isLast)
            sub.options.insert(QStringLiteral("intermediate"), true);
        ctx.setProgressRange(double(i) / n, double(i + 1) / n);
        ctx.stepProgress(0.0, QObject::tr("Converting with %1").arg(step.converter->displayName()));

        last = step.converter->convert(sub, ctx);
        if (ctx.isCancelled()) {
            ConversionResult r;
            r.cancelled = true;
            r.error = QObject::tr("Cancelled.");
            return r;
        }
        if (!last.ok) {
            if (last.error.isEmpty())
                last.error = QObject::tr("%1 failed.").arg(step.converter->displayName());
            return last;
        }
        if (last.outputs.isEmpty())
            last.outputs << sub.outputPath;
        warnings << last.warnings;
        if (!step.capability.note.isEmpty())
            warnings << step.capability.note;
        // Validate every produced file, not only the exit status.
        for (const QString& out : last.outputs) {
            const ValidationResult v = OutputValidator::validate(out, step.capability.to);
            if (!v.ok)
                return ConversionResult::failure(QObject::tr("Output verification failed: %1").arg(v.error));
        }
        if (!isLast) {
            if (last.outputs.size() != 1)
                return ConversionResult::failure(QObject::tr("Intermediate step produced multiple files."));
            currentInput = last.outputs.first();
            currentFormat = step.capability.to;
        }
    }

    // Move results into place without overwriting anything.
    ConversionResult final;
    final.ok = true;
    final.warnings = warnings;
    final.warnings.removeDuplicates();
    const QString outDir = QFileInfo(request.outputPath).absolutePath();
    if (!QDir().mkpath(outDir))
        return ConversionResult::failure(QObject::tr("Cannot create output folder %1.").arg(outDir));

    QString targetDir = outDir;
    if (last.outputs.size() > 1) {
        // Group multi-file output (pages, frames) into one new folder.
        targetDir = PathUtils::uniquePath(QDir(outDir).filePath(
            finalBase + QStringLiteral(" (") + request.outputFormat.toUpper() + QStringLiteral(")")));
        if (!QDir().mkpath(targetDir))
            return ConversionResult::failure(QObject::tr("Cannot create output folder %1.").arg(targetDir));
    }
    for (const QString& out : last.outputs) {
        const QString desired = last.outputs.size() > 1 ? QDir(targetDir).filePath(QFileInfo(out).fileName())
                                                        : request.outputPath;
        const QString dest = PathUtils::uniquePath(desired);
        if (!moveFile(out, dest))
            return ConversionResult::failure(QObject::tr("Could not write %1.").arg(dest));
        final.outputs << dest;
    }
    ctx.setProgressRange(0, 1);
    ctx.progress(1.0, QObject::tr("Done"));
    return final;
}

// ---------------------------------------------------------------------------

ConversionJob::ConversionJob(ConversionRequest request, ConversionPlan plan, QObject* parent)
    : QObject(parent), m_id(QUuid::createUuid().toString(QUuid::WithoutBraces)), m_request(std::move(request)),
      m_plan(std::move(plan))
{
}

QString ConversionJob::stateName(State s)
{
    switch (s) {
    case State::Queued: return tr("Queued");
    case State::Running: return tr("Running");
    case State::Succeeded: return tr("Done");
    case State::Failed: return tr("Failed");
    case State::Cancelled: return tr("Cancelled");
    }
    return {};
}

void ConversionJob::cancel()
{
    m_cancel->store(true);
    if (m_state == State::Queued) {
        ConversionResult r;
        r.cancelled = true;
        r.error = tr("Cancelled.");
        setFinished(r, 0);
    }
}

void ConversionJob::setRunning()
{
    m_state = State::Running;
    Q_EMIT stateChanged(m_state);
}

void ConversionJob::setProgress(double f, const QString& stage)
{
    if (isFinished())
        return;
    m_progress = f;
    if (!stage.isEmpty())
        m_stage = stage;
    Q_EMIT progressChanged(m_progress, m_stage);
}

void ConversionJob::setFinished(const ConversionResult& r, qint64 ms)
{
    if (isFinished())
        return;
    m_result = r;
    m_durationMs = ms;
    m_state = r.ok ? State::Succeeded : (r.cancelled || m_cancel->load() ? State::Cancelled : State::Failed);
    if (r.ok)
        m_progress = 1.0;
    m_stage = r.ok ? tr("Done") : r.error;
    Q_EMIT stateChanged(m_state);
    Q_EMIT finished();
}

void ConversionJob::resetForRetry()
{
    m_cancel = std::make_shared<std::atomic_bool>(false);
    m_state = State::Queued;
    m_progress = 0;
    m_stage.clear();
    m_result = {};
    Q_EMIT stateChanged(m_state);
    Q_EMIT progressChanged(0, {});
}

// ---------------------------------------------------------------------------

JobQueue::JobQueue(QObject* parent) : QObject(parent)
{
    const int ideal = std::max(1, QThread::idealThreadCount());
    m_pool.setMaxThreadCount(std::clamp(ideal / 2, 1, 3));
}

JobQueue::~JobQueue()
{
    shutdown(5000);
}

void JobQueue::setMaxConcurrent(int n)
{
    m_pool.setMaxThreadCount(std::clamp(n, 1, 8));
}

int JobQueue::activeCount() const
{
    return int(std::count_if(m_jobs.begin(), m_jobs.end(), [](ConversionJob* j) { return !j->isFinished(); }));
}

ConversionJob* JobQueue::submit(const ConversionRequest& request, const ConversionPlan& plan)
{
    auto* job = new ConversionJob(request, plan, this);
    m_jobs.append(job);
    Q_EMIT jobAdded(job);
    start(job);
    return job;
}

void JobQueue::retry(ConversionJob* job)
{
    if (!job || !job->isFinished())
        return;
    job->resetForRetry();
    start(job);
}

void JobQueue::start(ConversionJob* job)
{
    QPointer<ConversionJob> guard(job);
    const ConversionRequest request = job->request();
    const ConversionPlan plan = job->plan();
    std::shared_ptr<std::atomic_bool> cancel = job->m_cancel;

    m_pool.start([guard, request, plan, cancel, this] {
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(this, [guard] { if (guard) guard->setRunning(); }, Qt::QueuedConnection);
        QElapsedTimer t;
        t.start();
        auto progress = [guard, this](double f, const QString& stage) {
            QMetaObject::invokeMethod(this, [guard, f, stage] { if (guard) guard->setProgress(f, stage); },
                                      Qt::QueuedConnection);
        };
        ConversionResult r;
        try {
            r = ConversionExecutor::execute(request, plan, cancel.get(), progress);
        } catch (const std::exception& e) {
            r = ConversionResult::failure(QObject::tr("Internal error: %1").arg(QString::fromLocal8Bit(e.what())));
        } catch (...) {
            r = ConversionResult::failure(QObject::tr("Internal error."));
        }
        if (cancel->load()) {
            r.ok = false;
            r.cancelled = true;
            r.error = QObject::tr("Cancelled.");
        }
        const qint64 ms = t.elapsed();
        QMetaObject::invokeMethod(this, [guard, r, ms, this] {
            if (!guard)
                return;
            guard->setFinished(r, ms);
            Q_EMIT jobFinished(guard);
        }, Qt::QueuedConnection);
    });
}

void JobQueue::cancelAll()
{
    for (ConversionJob* j : m_jobs)
        if (!j->isFinished())
            j->cancel();
}

bool JobQueue::shutdown(int timeoutMs)
{
    cancelAll();
    m_pool.clear();
    return m_pool.waitForDone(timeoutMs);
}

void JobQueue::removeFinished()
{
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if ((*it)->isFinished()) {
            (*it)->deleteLater();
            it = m_jobs.erase(it);
        } else {
            ++it;
        }
    }
    Q_EMIT jobsRemoved();
}

} // namespace folvio
