#include "app/Application.h"

#include "core/conversions/ConversionRegistry.h"
#include "core/deps/DependencyManager.h"
#include "core/files/PathUtils.h"
#include "core/persistence/Database.h"
#include "combine/CombineWorkspace.h"
#include "ui/MainWindow.h"
#include "ui/themes/Theme.h"
#include "ui/widgets/Icons.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStyleHints>
#include <QTextStream>
#include <QTimer>

// Resources compiled into the static library must be registered explicitly.
static void initFolvioResources()
{
    Q_INIT_RESOURCE(resources);
}

namespace folvio {

namespace {

QFile* g_log = nullptr;

void messageHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    if (!g_log)
        return;
    static const char* names[] = {"debug", "warning", "critical", "fatal", "info"};
    QTextStream s(g_log);
    s << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << ' ' << names[std::min(int(type), 4)] << ' ' << msg << '\n';
    s.flush();
}

void openLog()
{
    // Diagnostics only: no document contents are ever logged.
    const QString path = PathUtils::logDir() + "/folvio.log";
    if (QFileInfo(path).size() > 2 * 1024 * 1024)
        QFile::remove(path);
    g_log = new QFile(path);
    if (g_log->open(QIODevice::Append | QIODevice::Text))
        qInstallMessageHandler(messageHandler);
}

} // namespace

int runApplication(int argc, char* argv[])
{
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    initFolvioResources();
    QApplication::setApplicationName(QStringLiteral("Folvio"));
    QApplication::setOrganizationName(QStringLiteral("Folvio"));
    QApplication::setApplicationVersion(QStringLiteral(FOLVIO_VERSION));
    QApplication::setWindowIcon(Icons::appIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Folvio - universal file workspace"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Files to open."), QStringLiteral("[files...]"));
    // Diagnostics for automated UI verification (not shown in the UI).
    QCommandLineOption shotOpt(QStringLiteral("screenshot"), QStringLiteral("Save a window screenshot and exit."), QStringLiteral("png"));
    QCommandLineOption pageOpt(QStringLiteral("page"), QStringLiteral("Initial page index."), QStringLiteral("index"));
    QCommandLineOption themeOpt(QStringLiteral("theme"), QStringLiteral("Force theme: light|dark."), QStringLiteral("mode"));
    QCommandLineOption delayOpt(QStringLiteral("screenshot-delay"), QStringLiteral("Delay in ms."), QStringLiteral("ms"), QStringLiteral("2500"));
    QCommandLineOption editOpt(QStringLiteral("demo-edit"), QStringLiteral("Click the first paragraph after opening (diagnostics)."));
    QCommandLineOption combineOpt(QStringLiteral("combine"), QStringLiteral("Import the given files into Combine (diagnostics)."));
    QCommandLineOption combineToolOpt(QStringLiteral("combine-tool"), QStringLiteral("Open Combine Edit with a tool index (diagnostics)."), QStringLiteral("tool"));
    QCommandLineOption combineDemoOpt(QStringLiteral("combine-demo"), QStringLiteral("Straighten and annotate the first Combine page (diagnostics)."));
    QCommandLineOption combineStepOpt(QStringLiteral("combine-step"), QStringLiteral("Combine step 0..2 (diagnostics)."), QStringLiteral("step"));
    for (const auto& o : {shotOpt, pageOpt, themeOpt, delayOpt, editOpt, combineOpt, combineToolOpt, combineDemoOpt, combineStepOpt})
        parser.addOption(o);
    parser.process(app);

    openLog();
    if (!Database::instance().open())
        QMessageBox::warning(nullptr, QStringLiteral("Folvio"),
                             QObject::tr("The settings database could not be opened. Settings and history will not be saved."));
    // User-chosen engine paths
    for (const char* id : {"ffmpeg", "ffprobe", "soffice", "tesseract"}) {
        const QString p = Database::instance().setting(QStringLiteral("engine.") + QLatin1String(id)).toString();
        if (!p.isEmpty())
            DependencyManager::instance().setOverride(QString::fromLatin1(id), p);
    }
    Theme::instance().apply(Theme::modeFromString(parser.isSet(themeOpt) ? parser.value(themeOpt)
                                                                          : Database::instance().setting("theme", "system").toString()));
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &app, [] {
        if (Theme::instance().mode() == Theme::Mode::System)
            Theme::instance().apply(Theme::Mode::System);
    });
    ConversionRegistry::registerBuiltins();
    PathUtils::cleanupStaleTemp(12);

    MainWindow window;
    window.show();
    const QStringList files = parser.positionalArguments();
    if (!files.isEmpty() && parser.isSet(combineOpt)) {
        QTimer::singleShot(0, &window, [&window, files] {
            window.showPage(MainWindow::Combine);
            window.combineWorkspace()->importFiles(files);
        });
    } else if (!files.isEmpty()) {
        QTimer::singleShot(0, &window, [&window, files] { window.openFiles(files); });
    }
    if (parser.isSet(combineToolOpt) || parser.isSet(combineDemoOpt) || parser.isSet(combineStepOpt)) {
        const int tool = parser.isSet(combineToolOpt) ? parser.value(combineToolOpt).toInt() : 0;
        const bool demo = parser.isSet(combineDemoOpt);
        const int step = parser.isSet(combineStepOpt) ? parser.value(combineStepOpt).toInt() : 1;
        QTimer::singleShot(1200, &window, [&window, tool, demo, step] {
            auto* ws = window.combineWorkspace();
            ws->setStep(combine::CombineWorkspace::Step::Edit);
            if (demo)
                ws->runDiagnosticDemo();
            ws->setTool(combine::PageCanvas::Tool(tool));
            if (step != 1)
                ws->setStep(combine::CombineWorkspace::Step(step));
        });
    }
    if (parser.isSet(pageOpt))
        QTimer::singleShot(10, &window, [&window, &parser, &pageOpt] { window.showPage(MainWindow::Page(parser.value(pageOpt).toInt())); });
    if (parser.isSet(editOpt))
        QTimer::singleShot(1500, &window, [&window] { window.demoEditFirstParagraph(); });
    if (parser.isSet(shotOpt)) {
        const QString shot = parser.value(shotOpt);
        QTimer::singleShot(parser.value(delayOpt).toInt(), &window, [&window, shot] {
            window.grab().save(shot);
            window.setProperty("skipCloseChecks", true);
            QApplication::exit(0);
        });
    }
    const int rc = app.exec();
    Database::instance().close();
    return rc;
}

} // namespace folvio
