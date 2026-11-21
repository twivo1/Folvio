#pragma once

#include <QString>
#include <QStringList>

namespace folvio {

// "folvio --selftest [--selftest-require ffmpeg,soffice,tesseract,onnxruntime,handwriting-download] [--selftest-report file]"
// handwriting-download: downloads the handwriting models into the self-test's temporary data folder and uses them.
// Runs real operations of the installed application (no test framework, no UI):
// Combine 3 PNG -> PDF, Auto Change edit + save, image and Markdown conversions, and - when the
// engines are present - MP4 -> MP3, HTML -> DOCX -> PDF -> DOCX and OCR. Used to verify release
// packages on clean machines. Returns the process exit code (0 = no failures).
int runSelfTest(const QStringList& requiredEngines, const QString& reportPath);

} // namespace folvio
