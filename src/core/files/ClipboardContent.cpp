#include "core/files/ClipboardContent.h"

#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeData>
#include <QUrl>

namespace folvio {

bool ClipboardContent::isImageFile(const QString& path)
{
    static const QStringList ext = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff", "heic", "heif", "avif"};
    const QFileInfo fi(path);
    return fi.isFile() && ext.contains(fi.suffix().toLower());
}

ClipboardContent ClipboardContent::read(const QMimeData* mime)
{
    ClipboardContent c;
    if (!mime)
        return c;
    // 1. Image files copied in a file manager.
    for (const QUrl& u : mime->urls())
        if (u.isLocalFile() && isImageFile(u.toLocalFile()))
            c.files << u.toLocalFile();
    if (!c.files.isEmpty()) {
        c.kind = Kind::ImageFiles;
        return c;
    }
    const QString text = mime->hasText() ? mime->text() : QString();
    const bool textOnlyPicture = !text.trimmed().isEmpty() && mime->hasHtml()
                                 && !mime->html().contains(QLatin1String("<img"), Qt::CaseInsensitive);
    // 2. A bitmap (screenshots, browsers).
    if (mime->hasImage() && !textOnlyPicture) {
        const QImage img = qvariant_cast<QImage>(mime->imageData());
        if (!img.isNull()) {
            c.kind = Kind::Image;
            c.image = img;
            return c;
        }
    }
    // 3. Encoded image data Qt did not turn into a QImage (some X11/Wayland clipboards).
    if (!textOnlyPicture) {
        for (const char* fmt : {"image/png", "image/jpeg", "image/bmp", "image/webp", "image/gif", "image/tiff", "PNG"}) {
            const QByteArray data = mime->data(QString::fromLatin1(fmt));
            if (data.isEmpty())
                continue;
            const QImage img = QImage::fromData(data);
            if (!img.isNull()) {
                c.kind = Kind::Image;
                c.image = img;
                return c;
            }
        }
    }
    // 4. Text (file URLs of non-images are not text to paste).
    if (!text.isEmpty() && !(mime->hasUrls() && text.startsWith(QLatin1String("file:")))) {
        c.kind = Kind::Text;
        c.text = text;
    }
    return c;
}

ClipboardContent ClipboardContent::fromClipboard()
{
    return read(QApplication::clipboard()->mimeData());
}

QImage ClipboardContent::loadImage(QString* error) const
{
    if (kind == Kind::Image)
        return image;
    if (kind != Kind::ImageFiles || files.isEmpty()) {
        if (error)
            *error = QObject::tr("The clipboard does not contain an image.");
        return {};
    }
    QImageReader reader(files.first());
    reader.setAutoTransform(true);
    reader.setAllocationLimit(1024); // MB; very large photos are fine, absurd sizes are refused
    QImage img = reader.read();
    if (img.isNull() && error)
        *error = QObject::tr("The image \"%1\" could not be read: %2").arg(QFileInfo(files.first()).fileName(), reader.errorString());
    return img;
}

} // namespace folvio
