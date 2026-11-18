#pragma once

#include <QString>
#include <QStringList>

namespace folvio {

// "folvio --selftest [--selftest-require ffmpeg,soffice,tesseract] [--selftest-report file]"
// Runs real operations of the installed application (no test framework, no UI):
// Combine 3 PNG -> PDF, Auto Change edit + save, image and Markdown conversions, and - when the
// engines are present - MP4 -> MP3, HTML -> DOCX -> PDF -> DOCX and OCR. Used to verify release
// packages on clean machines. Returns the process exit code (0 = no failures).
int runSelfTest(const QStringList& requiredEngines, const QString& reportPath);

} // namespace folvio
