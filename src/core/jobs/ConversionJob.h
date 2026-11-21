#pragma once

#include "core/conversions/ConversionRegistry.h"
#include "core/conversions/IConverter.h"

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QUuid>

#include <atomic>
#include <memory>

namespace folvio {

// Runs a plan synchronously (used by jobs on worker threads and by tests).
class ConversionExecutor {
public:
    static ConversionResult execute(const ConversionRequest& request, const ConversionPlan& plan,
                                    const std::atomic_bool* cancel, const ConversionContext::ProgressFn& progress);
};

class ConversionJob : public QObject {
    Q_OBJECT
public:
    enum class State { Queued, Running, Succeeded, Failed, Cancelled };
    Q_ENUM(State)

    ConversionJob(ConversionRequest request, ConversionPlan plan, QObject* parent = nullptr);

    QString id() const { return m_id; }
    const ConversionRequest& request() const { return m_request; }
    const ConversionPlan& plan() const { return m_plan; }
    State state() const { return m_state; }
    double progress() const { return m_progress; }
    QString stage() const { return m_stage; }
    const ConversionResult& result() const { return m_result; }
    qint64 durationMs() const { return m_durationMs; }
    bool isFinished() const { return m_state == State::Succeeded || m_state == State::Failed || m_state == State::Cancelled; }

    void cancel();

    static QString stateName(State s);

Q_SIGNALS:
    void progressChanged(double fraction, const QString& stage);
    void stateChanged(folvio::ConversionJob::State state);
    void finished();

private:
    friend class JobQueue;
    void setRunning();
    void setProgress(double f, const QString& stage);
    void setFinished(const ConversionResult& r, qint64 ms);
    void resetForRetry();

    QString m_id;
    ConversionRequest m_request;
    ConversionPlan m_plan;
    State m_state = State::Queued;
    double m_progress = 0;
    QString m_stage;
    ConversionResult m_result;
    qint64 m_durationMs = 0;
    std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);
};

// Owns jobs and runs them on a bounded thread pool. Never blocks the GUI thread.
class JobQueue : public QObject {
    Q_OBJECT
public:
    explicit JobQueue(QObject* parent = nullptr);
    ~JobQueue() override;

    ConversionJob* submit(const ConversionRequest& request, const ConversionPlan& plan);
    void retry(ConversionJob* job);
    void cancelAll();
    // Cancels everything and waits for workers to stop (used on application exit).
    bool shutdown(int timeoutMs = 15000);

    void setMaxConcurrent(int n);
    int maxConcurrent() const { return m_pool.maxThreadCount(); }
    QList<ConversionJob*> jobs() const { return m_jobs; }
    void removeFinished();
    int activeCount() const;

Q_SIGNALS:
    void jobAdded(folvio::ConversionJob* job);
    void jobFinished(folvio::ConversionJob* job);
    void jobsRemoved();

private:
    void start(ConversionJob* job);
    QThreadPool m_pool;
    QList<ConversionJob*> m_jobs;
};

} // namespace folvio
