#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

class QMimeData;

namespace folvio {

// What a paste (Ctrl+V) or a drop carries, decided in one place for every workspace.
//
// Order of preference (see docs/AUTO_CHANGE.md):
//  1. local image files (Explorer / Files "Copy" of a PNG/JPG) - the file, at full quality;
//  2. a bitmap (Win+Shift+S, Print Screen, "Copy image" in a browser, X11/Wayland image/png);
//     except when the bitmap is only a picture of copied text (Word/LibreOffice put both on the
//     clipboard; their HTML has no <img>) - then the text wins;
//  3. encoded image data in an image/* format Qt did not convert itself;
//  4. plain text.
struct ClipboardContent {
    enum class Kind { None, ImageFiles, Image, Text };
    Kind kind = Kind::None;
    QStringList files;   // ImageFiles
    QImage image;        // Image (and the first file's image for ImageFiles, loaded on demand)
    QString text;        // Text

    static ClipboardContent read(const QMimeData* mime);
    static ClipboardContent fromClipboard();
    static bool isImageFile(const QString& path);
    // Image of the content: the bitmap, or the first image file read with EXIF orientation applied.
    QImage loadImage(QString* error = nullptr) const;
    bool hasImage() const { return kind == Kind::Image || kind == Kind::ImageFiles; }
};

} // namespace folvio
