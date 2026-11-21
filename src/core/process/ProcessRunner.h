#pragma once

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>

namespace folvio {

struct ProcessSpec {
    QString program;            // absolute path to the executable
    QStringList arguments;      // passed as an argv array - never concatenated into a shell string
    QString workingDirectory;
    QByteArray stdinData;       // sent to the program's standard input, then the input is closed
    int timeoutMs = 10 * 60 * 1000;
    qint64 memoryLimitBytes = 4LL * 1024 * 1024 * 1024; // per process tree (Windows Job Object)
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    qint64 maxCapturedBytes = 8 * 1024 * 1024; // cap for stdout/stderr buffers
    std::function<void(const QByteArray&)> onStdout; // incremental chunks
    std::function<void(const QByteArray&)> onStderr;
};

struct ProcessResult {
    int exitCode = -1;
    bool started = false;
    bool crashed = false;
    bool timedOut = false;
    bool cancelled = false;
    QByteArray stdOut;
    QByteArray stdErr;
    QString errorString;

    bool succeeded() const { return started && !crashed && !timedOut && !cancelled && exitCode == 0; }
    QString describe() const;
};

// Runs an external process synchronously on the calling (worker) thread.
// On Windows the process is placed in a Job Object so that the whole process
// tree is terminated on cancel/timeout and a memory cap is enforced. No console
// windows are shown.
class ProcessRunner {
public:
    static ProcessResult run(const ProcessSpec& spec, const std::function<bool()>& isCancelled = {});
};

} // namespace folvio
