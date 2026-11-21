#include "core/conversions/OutputValidator.h"

#include "core/deps/DependencyManager.h"
#include "core/files/FileFormat.h"
#include "core/process/ProcessRunner.h"
#include "engines/pdf/PdfDocument.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>

#include <cstring>

namespace folvio {

namespace {
quint16 le16(const char* p) { return quint16(uchar(p[0]) | (uchar(p[1]) << 8)); }
quint32 le32(const char* p) { return quint32(le16(p)) | (quint32(le16(p + 2)) << 16); }
} // namespace

QStringList OutputValidator::zipEntries(const QString& path, QString* error)
{
    QStringList names;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return names;
    }
    const qint64 size = f.size();
    if (size < 22) {
        if (error)
            *error = QObject::tr("File too small to be a ZIP container.");
        return names;
    }
    // End of central directory record is within the last 64 KiB + 22 bytes.
    const qint64 tailSize = std::min<qint64>(size, 65557);
    f.seek(size - tailSize);
    const QByteArray tail = f.read(tailSize);
    int eocd = -1;
    for (int i = int(tail.size()) - 22; i >= 0; --i) {
        if (memcmp(tail.constData() + i, "PK\x05\x06", 4) == 0) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        if (error)
            *error = QObject::tr("ZIP end-of-directory record missing (file truncated?).");
        return names;
    }
    const char* e = tail.constData() + eocd;
    const quint16 entries = le16(e + 10);
    const quint32 cdSize = le32(e + 12);
    const quint32 cdOffset = le32(e + 16);
    if (qint64(cdOffset) + cdSize > size) {
        if (error)
            *error = QObject::tr("ZIP central directory is out of range.");
        return names;
    }
    f.seek(cdOffset);
    const QByteArray cd = f.read(cdSize);
    int pos = 0;
    for (int i = 0; i < entries && pos + 46 <= cd.size(); ++i) {
        const char* h = cd.constData() + pos;
        if (memcmp(h, "PK\x01\x02", 4) != 0)
            break;
        const quint16 nameLen = le16(h + 28), extraLen = le16(h + 30), commentLen = le16(h + 32);
        if (pos + 46 + nameLen > cd.size())
            break;
        names << QString::fromUtf8(h + 46, nameLen);
        pos += 46 + nameLen + extraLen + commentLen;
    }
    if (names.isEmpty() && error)
        *error = QObject::tr("ZIP container has no entries.");
    return names;
}

OutputValidator::MediaProbe OutputValidator::probeMedia(const QString& path)
{
    MediaProbe m;
    const QString ffprobe = DependencyManager::instance().path("ffprobe");
    if (ffprobe.isEmpty()) {
        m.error = QObject::tr("FFprobe is not available.");
        return m;
    }
    ProcessSpec spec;
    spec.program = ffprobe;
    spec.arguments = {"-v", "error", "-print_format", "json", "-show_format", "-show_streams", path};
    spec.timeoutMs = 60000;
    const ProcessResult r = ProcessRunner::run(spec);
    if (!r.succeeded()) {
        m.error = QString::fromUtf8(r.stdErr).trimmed();
        if (m.error.isEmpty())
            m.error = r.describe();
        return m;
    }
    const QJsonObject root = QJsonDocument::fromJson(r.stdOut).object();
    m.durationSec = root.value("format").toObject().value("duration").toString().toDouble();
    for (const QJsonValue& v : root.value("streams").toArray()) {
        const QJsonObject s = v.toObject();
        const QString type = s.value("codec_type").toString();
        // Cover art is reported as a video stream with attached_pic disposition.
        const bool attachedPic = s.value("disposition").toObject().value("attached_pic").toInt() == 1;
        if (type == "video" && !attachedPic && !m.hasVideo) {
            m.hasVideo = true;
            m.videoCodec = s.value("codec_name").toString();
            m.width = s.value("width").toInt();
            m.height = s.value("height").toInt();
            if (m.durationSec <= 0)
                m.durationSec = s.value("duration").toString().toDouble();
        } else if (type == "audio" && !m.hasAudio) {
            m.hasAudio = true;
            m.audioCodec = s.value("codec_name").toString();
        }
    }
    m.ok = m.hasVideo || m.hasAudio;
    if (!m.ok)
        m.error = QObject::tr("No audio or video streams found.");
    return m;
}

ValidationResult OutputValidator::validate(const QString& path, const QString& formatId)
{
    ValidationResult v;
    QFileInfo fi(path);
    if (!fi.exists() || fi.size() == 0) {
        v.error = QObject::tr("Output file is missing or empty.");
        return v;
    }
    const FormatCategory cat = FormatRegistry::category(formatId);

    if (formatId == "pdf") {
        PdfDocument::OpenError err;
        QString msg;
        auto doc = PdfDocument::openFile(path, {}, &err, &msg);
        // A password-protected output is still a structurally valid PDF.
        if (!doc && err != PdfDocument::OpenError::Password) {
            v.error = msg;
            return v;
        }
        v.ok = true;
        v.details = doc ? QObject::tr("%n page(s)", nullptr, doc->pageCount()) : QObject::tr("encrypted");
        return v;
    }
    if (formatId == "docx" || formatId == "xlsx" || formatId == "pptx" || formatId == "odt" || formatId == "ods"
        || formatId == "odp") {
        QString err;
        const QStringList entries = zipEntries(path, &err);
        if (entries.isEmpty()) {
            v.error = err;
            return v;
        }
        static const QHash<QString, QString> required = {{"docx", "word/document.xml"}, {"xlsx", "xl/workbook.xml"},
                                                         {"pptx", "ppt/presentation.xml"}, {"odt", "content.xml"},
                                                         {"ods", "content.xml"}, {"odp", "content.xml"}};
        if (!entries.contains(required.value(formatId))) {
            v.error = QObject::tr("Container is missing %1.").arg(required.value(formatId));
            return v;
        }
        v.ok = true;
        v.details = QObject::tr("%n part(s)", nullptr, int(entries.size()));
        return v;
    }
    if (formatId == "doc" || formatId == "xls" || formatId == "ppt") {
        QFile f(path);
        f.open(QIODevice::ReadOnly);
        v.ok = f.read(8) == QByteArray("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
        if (!v.ok)
            v.error = QObject::tr("Not a valid OLE2 compound document.");
        return v;
    }
    if (formatId == "rtf") {
        QFile f(path);
        f.open(QIODevice::ReadOnly);
        v.ok = f.read(5) == "{\\rtf";
        if (!v.ok)
            v.error = QObject::tr("Not a valid RTF document.");
        return v;
    }
    if (formatId == "json") {
        QFile f(path);
        f.open(QIODevice::ReadOnly);
        QJsonParseError pe;
        QJsonDocument::fromJson(f.readAll(), &pe);
        v.ok = pe.error == QJsonParseError::NoError;
        if (!v.ok)
            v.error = pe.errorString();
        return v;
    }
    if (formatId == "txt" || formatId == "csv" || formatId == "html" || formatId == "md" || formatId == "svg") {
        QFile f(path);
        f.open(QIODevice::ReadOnly);
        const QByteArray data = f.read(4 * 1024 * 1024);
        auto dec = QStringDecoder(QStringDecoder::Utf8);
        const QString text = dec.decode(data);
        if (dec.hasError() && formatId != "txt" && formatId != "csv") {
            v.error = QObject::tr("Output is not valid UTF-8 text.");
            return v;
        }
        v.ok = !text.trimmed().isEmpty() || data.size() > 0;
        if (!v.ok)
            v.error = QObject::tr("Output contains no text.");
        return v;
    }
    if (cat == FormatCategory::Image) {
        if (formatId == "heic" || formatId == "avif") {
            const MediaProbe m = probeMedia(path);
            v.ok = m.ok && m.width > 0;
            v.error = m.error;
            v.details = QStringLiteral("%1x%2").arg(m.width).arg(m.height);
            return v;
        }
        QImageReader reader(path);
        reader.setDecideFormatFromContent(true);
        const QImage img = reader.read();
        if (img.isNull()) {
            v.error = QObject::tr("Image cannot be decoded: %1").arg(reader.errorString());
            return v;
        }
        v.ok = true;
        v.details = QStringLiteral("%1x%2").arg(img.width()).arg(img.height());
        return v;
    }
    if (cat == FormatCategory::Audio || cat == FormatCategory::Video) {
        const MediaProbe m = probeMedia(path);
        if (!m.ok) {
            v.error = m.error;
            return v;
        }
        if (cat == FormatCategory::Audio && !m.hasAudio) {
            v.error = QObject::tr("Audio output contains no audio stream.");
            return v;
        }
        if (cat == FormatCategory::Video && formatId != "gif" && !m.hasVideo) {
            v.error = QObject::tr("Video output contains no video stream.");
            return v;
        }
        if (m.durationSec <= 0.0) {
            v.error = QObject::tr("Output has zero duration.");
            return v;
        }
        v.ok = true;
        v.details = QObject::tr("duration %1 s").arg(m.durationSec, 0, 'f', 1);
        return v;
    }
    v.ok = true; // unknown format: existence + non-empty already checked
    return v;
}

} // namespace folvio
