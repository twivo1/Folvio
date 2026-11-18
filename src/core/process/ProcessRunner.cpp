#include "core/process/ProcessRunner.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <csignal>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace folvio {

QString ProcessResult::describe() const
{
    if (!started)
        return QObject::tr("The program could not be started: %1").arg(errorString);
    if (cancelled)
        return QObject::tr("Cancelled.");
    if (timedOut)
        return QObject::tr("The operation took too long and was stopped.");
    if (crashed)
        return QObject::tr("The external program crashed.");
    if (exitCode != 0) {
        QString tail = QString::fromLocal8Bit(stdErr.right(600)).trimmed();
        if (tail.isEmpty())
            tail = QString::fromLocal8Bit(stdOut.right(600)).trimmed();
        return QObject::tr("The external program failed (exit code %1). %2").arg(exitCode).arg(tail);
    }
    return QObject::tr("OK");
}

namespace {

#ifndef Q_OS_WIN
// Inside an AppImage the runtime points LD_LIBRARY_PATH, QT_PLUGIN_PATH etc. into the image.
// External programs (LibreOffice, FFmpeg, Tesseract) come from the host system and must not
// load our bundled libraries, so those variables are restored to the host values.
QProcessEnvironment hostEnvironment(QProcessEnvironment env)
{
    if (!qEnvironmentVariableIsSet("APPIMAGE") && !qEnvironmentVariableIsSet("APPDIR"))
        return env;
    const char* vars[] = {"LD_LIBRARY_PATH", "LD_PRELOAD", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                          "QML2_IMPORT_PATH", "QTDIR", "PYTHONHOME", "PYTHONPATH", "GDK_PIXBUF_MODULEDIR",
                          "GDK_PIXBUF_MODULE_FILE", "GIO_MODULE_DIR", "GSETTINGS_SCHEMA_DIR", "XDG_DATA_DIRS"};
    for (const char* v : vars) {
        const QString name = QString::fromLatin1(v);
        const QString orig = env.value(name + QStringLiteral("_ORIG"));
        if (!orig.isEmpty())
            env.insert(name, orig);
        else if (name != QLatin1String("XDG_DATA_DIRS"))
            env.remove(name);
    }
    return env;
}
#endif

#ifdef Q_OS_WIN
class JobObject {
public:
    explicit JobObject(qint64 memoryLimit)
    {
        m_job = CreateJobObjectW(nullptr, nullptr);
        if (!m_job)
            return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (memoryLimit > 0) {
            info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
            info.JobMemoryLimit = SIZE_T(memoryLimit);
        }
        SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
    ~JobObject()
    {
        if (m_job)
            CloseHandle(m_job); // KILL_ON_JOB_CLOSE terminates any survivors
    }
    void assign(qint64 pid)
    {
        if (!m_job || pid <= 0)
            return;
        HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(pid));
        if (h) {
            AssignProcessToJobObject(m_job, h);
            CloseHandle(h);
        }
    }
    void terminate()
    {
        if (m_job)
            TerminateJobObject(m_job, 1);
    }

private:
    HANDLE m_job = nullptr;
};
#endif

void appendCapped(QByteArray& buf, const QByteArray& chunk, qint64 cap)
{
    buf.append(chunk);
    if (buf.size() > cap)
        buf = buf.right(cap); // keep the tail: errors are usually at the end
}

} // namespace

ProcessResult ProcessRunner::run(const ProcessSpec& spec, const std::function<bool()>& isCancelled)
{
    ProcessResult r;
    if (spec.program.isEmpty() || !QFileInfo(spec.program).isFile()) {
        r.errorString = QObject::tr("Executable not found: %1").arg(spec.program);
        return r;
    }

    QProcess p;
    p.setProgram(spec.program);
    p.setArguments(spec.arguments);
    p.setProcessEnvironment(spec.environment);
    if (!spec.workingDirectory.isEmpty())
        p.setWorkingDirectory(spec.workingDirectory);
    p.setInputChannelMode(QProcess::ManagedInputChannel);
#ifdef Q_OS_WIN
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
        a->flags |= CREATE_NO_WINDOW;
        a->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        a->startupInfo->wShowWindow = SW_HIDE;
    });
    JobObject job(spec.memoryLimitBytes);
#else
    p.setProcessEnvironment(hostEnvironment(spec.environment));
    // Own process group (so cancelling kills the whole tree) and die together with Folvio.
    p.setChildProcessModifier([] {
        ::setpgid(0, 0);
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    });
#endif

    p.start();
    if (!p.waitForStarted(30000)) {
        r.errorString = p.errorString();
        return r;
    }
    r.started = true;
#ifdef Q_OS_WIN
    job.assign(p.processId());
#endif
    p.closeWriteChannel();

    QElapsedTimer timer;
    timer.start();
    auto drain = [&] {
        const QByteArray out = p.readAllStandardOutput();
        if (!out.isEmpty()) {
            appendCapped(r.stdOut, out, spec.maxCapturedBytes);
            if (spec.onStdout)
                spec.onStdout(out);
        }
        const QByteArray err = p.readAllStandardError();
        if (!err.isEmpty()) {
            appendCapped(r.stdErr, err, spec.maxCapturedBytes);
            if (spec.onStderr)
                spec.onStderr(err);
        }
    };

    while (!p.waitForFinished(100)) {
        drain();
        if (p.state() == QProcess::NotRunning)
            break;
        const bool cancel = isCancelled && isCancelled();
        const bool timeout = spec.timeoutMs > 0 && timer.elapsed() > spec.timeoutMs;
        if (cancel || timeout) {
            r.cancelled = cancel;
            r.timedOut = timeout && !cancel;
#ifdef Q_OS_WIN
            job.terminate();
#else
            if (p.processId() > 0)
                ::kill(-static_cast<pid_t>(p.processId()), SIGKILL); // the whole process group
#endif
            p.kill();
            p.waitForFinished(5000);
            drain();
            r.exitCode = -1;
            return r;
        }
    }
    drain();
    r.crashed = p.exitStatus() == QProcess::CrashExit;
    r.exitCode = p.exitCode();
    return r;
}

} // namespace folvio
