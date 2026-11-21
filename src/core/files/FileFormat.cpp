#include "core/files/FileFormat.h"

#include <QFile>
#include <QFileInfo>

namespace folvio {

QString categoryName(FormatCategory c)
{
    switch (c) {
    case FormatCategory::Pdf: return QStringLiteral("PDF");
    case FormatCategory::Document: return QStringLiteral("Document");
    case FormatCategory::Spreadsheet: return QStringLiteral("Spreadsheet");
    case FormatCategory::Presentation: return QStringLiteral("Presentation");
    case FormatCategory::Text: return QStringLiteral("Text");
    case FormatCategory::Data: return QStringLiteral("Data");
    case FormatCategory::Image: return QStringLiteral("Image");
    case FormatCategory::Audio: return QStringLiteral("Audio");
    case FormatCategory::Video: return QStringLiteral("Video");
    case FormatCategory::Unknown: break;
    }
    return QStringLiteral("Unknown");
}

const QList<FormatInfo>& FormatRegistry::all()
{
    using C = FormatCategory;
    static const QList<FormatInfo> formats = {
        {"pdf", "PDF document", C::Pdf, {"pdf"}},
        {"docx", "Word document", C::Document, {"docx"}},
        {"doc", "Word 97-2003 document", C::Document, {"doc"}},
        {"odt", "OpenDocument text", C::Document, {"odt"}},
        {"rtf", "Rich Text Format", C::Document, {"rtf"}},
        {"txt", "Plain text", C::Text, {"txt", "text", "log"}},
        {"html", "HTML page", C::Text, {"html", "htm", "xhtml"}},
        {"md", "Markdown", C::Text, {"md", "markdown"}},
        {"pptx", "PowerPoint presentation", C::Presentation, {"pptx"}},
        {"ppt", "PowerPoint 97-2003", C::Presentation, {"ppt"}},
        {"odp", "OpenDocument presentation", C::Presentation, {"odp"}},
        {"xlsx", "Excel workbook", C::Spreadsheet, {"xlsx"}},
        {"xls", "Excel 97-2003 workbook", C::Spreadsheet, {"xls"}},
        {"ods", "OpenDocument spreadsheet", C::Spreadsheet, {"ods"}},
        {"csv", "CSV table", C::Data, {"csv"}},
        {"json", "JSON data", C::Data, {"json"}},
        {"png", "PNG image", C::Image, {"png"}},
        {"jpg", "JPEG image", C::Image, {"jpg", "jpeg", "jpe"}, true},
        {"webp", "WebP image", C::Image, {"webp"}, true},
        {"bmp", "Bitmap image", C::Image, {"bmp", "dib"}},
        {"tiff", "TIFF image", C::Image, {"tif", "tiff"}},
        {"gif", "GIF image", C::Image, {"gif"}},
        {"heic", "HEIC/HEIF image", C::Image, {"heic", "heif"}, true},
        {"avif", "AVIF image", C::Image, {"avif"}, true},
        {"svg", "SVG vector image", C::Image, {"svg"}},
        {"mp4", "MP4 video", C::Video, {"mp4", "m4v"}, true},
        {"mkv", "Matroska video", C::Video, {"mkv"}, true},
        {"mov", "QuickTime video", C::Video, {"mov", "qt"}, true},
        {"avi", "AVI video", C::Video, {"avi"}, true},
        {"webm", "WebM video", C::Video, {"webm"}, true},
        {"mp3", "MP3 audio", C::Audio, {"mp3"}, true},
        {"wav", "WAV audio", C::Audio, {"wav", "wave"}},
        {"flac", "FLAC audio", C::Audio, {"flac"}},
        {"m4a", "M4A/AAC audio", C::Audio, {"m4a", "aac"}, true},
        {"ogg", "Ogg Vorbis audio", C::Audio, {"ogg", "oga"}, true},
    };
    return formats;
}

const FormatInfo* FormatRegistry::byId(const QString& id)
{
    for (const auto& f : all())
        if (f.id == id)
            return &f;
    return nullptr;
}

QString FormatRegistry::idForExtension(const QString& extension)
{
    const QString ext = extension.toLower().remove(QLatin1Char('.'));
    for (const auto& f : all())
        if (f.extensions.contains(ext))
            return f.id;
    return {};
}

QString FormatRegistry::preferredExtension(const QString& id)
{
    const auto* f = byId(id);
    return f ? f->extensions.first() : id;
}

QString FormatRegistry::displayName(const QString& id)
{
    const auto* f = byId(id);
    return f ? f->displayName : id.toUpper();
}

FormatCategory FormatRegistry::category(const QString& id)
{
    const auto* f = byId(id);
    return f ? f->category : FormatCategory::Unknown;
}

namespace {

bool startsWith(const QByteArray& d, const char* magic, int offset = 0)
{
    const int n = int(qstrlen(magic));
    return d.size() >= offset + n && memcmp(d.constData() + offset, magic, n) == 0;
}

QString sniffZip(const QByteArray& head, const QString& extHint)
{
    // OOXML / ODF are ZIP containers. ODF stores "mimetype" uncompressed first.
    if (head.contains("application/vnd.oasis.opendocument.text"))
        return "odt";
    if (head.contains("application/vnd.oasis.opendocument.spreadsheet"))
        return "ods";
    if (head.contains("application/vnd.oasis.opendocument.presentation"))
        return "odp";
    if (head.contains("word/"))
        return "docx";
    if (head.contains("xl/"))
        return "xlsx";
    if (head.contains("ppt/"))
        return "pptx";
    // [Content_Types].xml first without part names visible in the header:
    if (extHint == "docx" || extHint == "xlsx" || extHint == "pptx" || extHint == "odt"
        || extHint == "ods" || extHint == "odp")
        return extHint;
    return {};
}

QString sniffFtyp(const QByteArray& head, const QString& extHint)
{
    if (!startsWith(head, "ftyp", 4))
        return {};
    const QByteArray brand = head.mid(8, 4);
    const QByteArray compat = head.mid(8, 64);
    if (brand == "avif" || brand == "avis")
        return "avif";
    if (brand == "heic" || brand == "heix" || brand == "hevc" || brand == "hevx" || brand == "heim"
        || brand == "heis" || brand == "mif1" || brand == "msf1") {
        if (compat.contains("avif"))
            return "avif";
        return "heic";
    }
    if (brand == "qt  ")
        return "mov";
    if (brand == "M4A " || brand == "M4B ")
        return "m4a";
    if (extHint == "m4a" || extHint == "mov")
        return extHint;
    return "mp4";
}

} // namespace

QString FormatDetector::sniff(const QByteArray& head, const QString& extHint)
{
    if (head.isEmpty())
        return {};
    if (startsWith(head, "%PDF-") || (head.size() > 5 && head.left(1024).contains("%PDF-")))
        return "pdf";
    if (startsWith(head, "PK\x03\x04"))
        return sniffZip(head, extHint);
    if (startsWith(head, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1")) {
        // OLE2 compound file: doc / xls / ppt. Trust the extension.
        if (extHint == "doc" || extHint == "xls" || extHint == "ppt")
            return extHint;
        return "doc";
    }
    if (startsWith(head, "{\\rtf"))
        return "rtf";
    if (startsWith(head, "\x89PNG\r\n\x1A\n"))
        return "png";
    if (startsWith(head, "\xFF\xD8\xFF"))
        return "jpg";
    if (startsWith(head, "GIF87a") || startsWith(head, "GIF89a"))
        return "gif";
    if (startsWith(head, "BM") && head.size() > 14)
        return "bmp";
    if (startsWith(head, "II*\0") || startsWith(head, "MM\0*"))
        return "tiff";
    if (startsWith(head, "RIFF")) {
        if (startsWith(head, "WEBP", 8))
            return "webp";
        if (startsWith(head, "WAVE", 8))
            return "wav";
        if (startsWith(head, "AVI ", 8))
            return "avi";
    }
    if (const QString f = sniffFtyp(head, extHint); !f.isEmpty())
        return f;
    if (startsWith(head, "\x1A\x45\xDF\xA3"))
        return head.left(64).contains("webm") ? "webm" : "mkv";
    if (startsWith(head, "fLaC"))
        return "flac";
    if (startsWith(head, "OggS"))
        return "ogg";
    if (startsWith(head, "ID3") || (uchar(head[0]) == 0xFF && head.size() > 1 && (uchar(head[1]) & 0xE0) == 0xE0))
        return "mp3";

    // Text based formats: rely on extension, but check SVG.
    const QByteArray lower = head.left(4096).toLower();
    if (lower.contains("<svg"))
        return "svg";
    if (extHint == "html" || extHint == "md" || extHint == "txt" || extHint == "csv" || extHint == "json")
        return extHint;
    if (lower.contains("<!doctype html") || lower.contains("<html"))
        return "html";
    return {};
}

DetectedFile FormatDetector::detect(const QString& path)
{
    DetectedFile r;
    r.path = path;
    QFileInfo fi(path);
    if (!fi.exists()) {
        r.error = QObject::tr("File does not exist.");
        return r;
    }
    if (!fi.isFile()) {
        r.error = QObject::tr("Not a regular file.");
        return r;
    }
    r.size = fi.size();
    r.extensionFormatId = FormatRegistry::idForExtension(fi.suffix());

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = QObject::tr("Cannot read file: %1").arg(f.errorString());
        return r;
    }
    const QByteArray head = f.read(64 * 1024);
    const QString sniffed = sniff(head, r.extensionFormatId);
    if (!sniffed.isEmpty()) {
        r.formatId = sniffed;
        // jpg/jpeg etc. map to the same id; mismatch only if ids differ.
        r.contentMatchesExtension = r.extensionFormatId.isEmpty() || r.extensionFormatId == sniffed;
    } else {
        r.formatId = r.extensionFormatId;
    }
    if (r.size == 0)
        r.error = QObject::tr("File is empty.");
    return r;
}

} // namespace folvio
