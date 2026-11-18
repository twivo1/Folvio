#include "core/conversions/ConversionRegistry.h"

#include <QApplication>
#include <QTemporaryDir>

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    // Tests never show windows. The native platform plugin is kept on purpose:
    // the "offscreen" plugin on Windows has no font database, so generated PDFs
    // would contain no text.
#ifndef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    // Isolate app data (backups, temp, recovery) from the real user profile.
    QTemporaryDir dataDir;
    qputenv("FOLVIO_DATA_DIR", dataDir.path().toUtf8());
    QApplication app(argc, argv);
    folvio::ConversionRegistry::registerBuiltins();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
