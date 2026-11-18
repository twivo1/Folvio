#pragma once

#include <QString>

namespace folvio {

class PathUtils {
public:
    // Per-user application data directory (%LOCALAPPDATA%\Folvio). Created on demand.
    static QString appDataDir();
    static QString backupDir();
    static QString tempRoot();     // %LOCALAPPDATA%\Folvio\temp
    static QString recoveryDir();
    static QString logDir();

    // Returns a path that does not exist yet: "name.ext", "name (1).ext", ...
    static QString uniquePath(const QString& desiredPath);

    // Builds "<dir>/<baseName>.<ext>" with a sanitized base name.
    static QString outputPathFor(const QString& inputPath, const QString& outputDir, const QString& extension);

    // Removes characters that are illegal in Windows file names, reserved device
    // names and trailing dots/spaces. Never returns an empty string.
    static QString sanitizeFileName(const QString& name);

    // True if 'candidate' resolves to the same file as 'other' (case-insensitive on Windows).
    static bool samePath(const QString& a, const QString& b);

    // Converts to a path usable by Win32 APIs beyond MAX_PATH ("\\?\C:\...").
    static QString toLongPathSafe(const QString& path);

    // Deletes temp sub-directories older than maxAgeHours (crash leftovers).
    static int cleanupStaleTemp(int maxAgeHours = 12);
};

} // namespace folvio
