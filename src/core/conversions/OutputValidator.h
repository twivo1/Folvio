#pragma once

#include <QString>
#include <QStringList>

namespace folvio {

struct ValidationResult {
    bool ok = false;
    QString error;
    QString details; // e.g. "3 pages", "1920x1080", "duration 12.4 s"
};

// Verifies that a produced file is a real, readable instance of the expected
// format - not just that the converter exited with code 0.
class OutputValidator {
public:
    static ValidationResult validate(const QString& path, const QString& formatId);

    // Lists entry names of a ZIP container by reading its central directory.
    static QStringList zipEntries(const QString& path, QString* error = nullptr);

    struct MediaProbe {
        bool ok = false;
        double durationSec = 0;
        bool hasVideo = false;
        bool hasAudio = false;
        QString videoCodec;
        QString audioCodec;
        int width = 0;
        int height = 0;
        QString error;
    };
    static MediaProbe probeMedia(const QString& path);
};

} // namespace folvio
