#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

class QWidget;

namespace folvio::test {

// Each page is a list of text lines drawn top-down (14 pt, 28 pt line pitch, first baseline at y=100).
bool makeTextPdf(const QString& path, const QList<QStringList>& pages, const QString& family = "Arial");
// PDF whose only content is a raster image of text (simulated scan).
bool makeScannedPdf(const QString& path, const QString& text);
// PDF with a ruled table and an embedded image.
bool makeTablePdf(const QString& path);
bool makePng(const QString& path, int w, int h, bool alpha);
bool runFfmpeg(const QStringList& args);
bool havePython();
bool runPython(const QString& script, const QStringList& args);
QString pdfText(const QString& path, const QByteArray& password = {});
// Saves a grab of the widget's window when FOLVIO_TEST_SCREENSHOTS names a folder (visual review).
void saveScreenshot(QWidget* w, const QString& name);
// Fraction of differing pixels between two images (same size), optionally ignoring a rect.
double pixelDifference(const QImage& a, const QImage& b, const QRect& ignore = {});

} // namespace folvio::test
