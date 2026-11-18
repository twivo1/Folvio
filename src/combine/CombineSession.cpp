#include "combine/CombineSession.h"

#include "combine/ImageGeometry.h"
#include "core/deps/DependencyManager.h"
#include "core/files/FileFormat.h"
#include "core/files/PathUtils.h"
#include "core/files/SafeFileWriter.h"
#include "core/process/ProcessRunner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

namespace folvio::combine {

namespace {

QString ensure(const QString& dir)
{
    QDir().mkpath(dir);
    return dir;
}

// HEIC/AVIF: convert to a PNG copy with FFmpeg (source untouched).
QString convertWithFfmpeg(const QString& path, QString* error)
{
    const QString ffmpeg = DependencyManager::instance().path("ffmpeg");
    if (ffmpeg.isEmpty()) {
        *error = QObject::tr("%1: HEIC/AVIF images need FFmpeg, which is not installed.").arg(QFileInfo(path).fileName());
        return {};
    }
    const QString out = CombineSession::assetsDir() + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".png";
    ProcessSpec spec;
    spec.program = ffmpeg;
    spec.arguments = {"-hide_banner", "-loglevel", "error", "-nostdin", "-protocol_whitelist", "file",
                      "-i", "file:" + path, "-frames:v", "1", "-y", "file:" + out};
    spec.timeoutMs = 120000;
    const ProcessResult r = ProcessRunner::run(spec);
    if (!r.succeeded() || !QFileInfo::exists(out)) {
        *error = QObject::tr("%1: the image could not be decoded (%2).").arg(QFileInfo(path).fileName(), r.describe());
        return {};
    }
    return out;
}

} // namespace

QString CombineSession::sessionDir() { return ensure(PathUtils::appDataDir() + "/combine/session"); }
QString CombineSession::assetsDir() { return ensure(sessionDir() + "/assets"); }

bool CombineSession::isSupportedFile(const QString& path)
{
    return ImageProcessor::supportedSuffixes().contains(QFileInfo(path).suffix().toLower())
           || QStringList{"png", "jpg", "webp", "bmp", "tiff", "gif", "heic", "avif"}.contains(FormatDetector::detect(path).formatId);
}

ImportResult CombineSession::importFiles(const QStringList& paths)
{
    ImportResult r;
    for (const QString& p : paths) {
        const QFileInfo fi(p);
        if (fi.isDir()) {
            QStringList inner;
            for (const QFileInfo& f : QDir(p).entryInfoList(QDir::Files, QDir::Name))
                if (isSupportedFile(f.absoluteFilePath()))
                    inner << f.absoluteFilePath();
            const ImportResult sub = importFiles(inner);
            r.pages << sub.pages;
            r.errors << sub.errors;
            continue;
        }
        if (!fi.isFile()) {
            r.errors << QObject::tr("%1: file not found.").arg(fi.fileName());
            continue;
        }
        const QString format = FormatDetector::detect(p).formatId;
        if (!QStringList{"png", "jpg", "webp", "bmp", "tiff", "gif", "heic", "avif"}.contains(format)) {
            r.errors << QObject::tr("%1: not a supported image (%2).").arg(fi.fileName(), format.isEmpty() ? QObject::tr("unknown type") : format.toUpper());
            continue;
        }
        QString source = fi.absoluteFilePath();
        bool converted = false;
        if (format == "heic" || format == "avif") {
            QString err;
            source = convertWithFfmpeg(p, &err);
            if (source.isEmpty()) {
                r.errors << err;
                continue;
            }
            converted = true;
        }
        // Decode a small version to reject damaged files now rather than at export time.
        QString err;
        const QImage probe = ImageProcessor::loadOriented(source, 96, &err);
        if (probe.isNull()) {
            r.errors << QObject::tr("%1: %2").arg(fi.fileName(), err);
            continue;
        }
        CombinePage page;
        page.sourcePath = source;
        page.displayName = fi.fileName();
        page.fromClipboard = converted; // stored in the session assets
        page.sourceSize = ImageProcessor::orientedSize(source);
        if (!page.sourceSize.isValid()) {
            const QImage full = ImageProcessor::loadOriented(source, 0, &err);
            page.sourceSize = full.size();
        }
        page.sourceSha256 = sha256OfFile(source);
        r.pages << page;
    }
    return r;
}

bool CombineSession::mimeHasImportable(const QMimeData* mime)
{
    if (!mime)
        return false;
    if (mime->hasImage())
        return true;
    for (const QUrl& u : mime->urls())
        if (u.isLocalFile() && (QFileInfo(u.toLocalFile()).isDir() || isSupportedFile(u.toLocalFile())))
            return true;
    return false;
}

CombinePage CombineSession::pageFromImage(const QImage& image, const QString& name, QString* error)
{
    CombinePage page;
    if (image.isNull()) {
        if (error)
            *error = QObject::tr("The clipboard image is empty.");
        return page;
    }
    const QString path = assetsDir() + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".png";
    if (!image.save(path, "PNG")) {
        if (error)
            *error = QObject::tr("The clipboard image could not be stored.");
        return page;
    }
    page.sourcePath = path;
    page.displayName = name;
    page.fromClipboard = true;
    page.sourceSize = image.size();
    page.sourceSha256 = sha256OfFile(path);
    return page;
}

ImportResult CombineSession::importMime(const QMimeData* mime)
{
    ImportResult r;
    if (!mime)
        return r;
    QStringList files;
    for (const QUrl& u : mime->urls())
        if (u.isLocalFile())
            files << u.toLocalFile();
    if (!files.isEmpty()) {
        r = importFiles(files);
        if (!r.pages.isEmpty())
            return r;
    }
    if (mime->hasImage()) {
        // Screenshots (Win+Shift+S) arrive as a bitmap, not as a file.
        const QImage img = qvariant_cast<QImage>(mime->imageData());
        QString err;
        CombinePage p = pageFromImage(img, QObject::tr("Pasted image"), &err);
        if (p.sourcePath.isEmpty())
            r.errors << err;
        else
            r.pages << p;
    }
    return r;
}

// ---- autosave -----------------------------------------------------------------------

bool CombineSession::autosave(const CombineDocument& doc, QString* error)
{
    QSaveFile f(sessionDir() + "/session.json");
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(doc.toJson()).toJson(QJsonDocument::Compact));
    return f.commit();
}

bool CombineSession::hasRecoverableSession()
{
    QFile f(sessionDir() + "/session.json");
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return !QJsonDocument::fromJson(f.readAll()).object().value("pages").toArray().isEmpty();
}

bool CombineSession::restoreSession(CombineDocument& doc, QString* error)
{
    QFile f(sessionDir() + "/session.json");
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return doc.fromJson(QJsonDocument::fromJson(f.readAll()).object(), error);
}

void CombineSession::discardSession(const CombineDocument* keep)
{
    QFile::remove(sessionDir() + "/session.json");
    // Remove stored clipboard/converted images not used by the document that stays open.
    QSet<QString> used;
    if (keep)
        for (const CombinePage& p : keep->pages())
            used.insert(QFileInfo(p.sourcePath).absoluteFilePath());
    for (const QFileInfo& fi : QDir(assetsDir()).entryInfoList(QDir::Files))
        if (!used.contains(fi.absoluteFilePath()))
            QFile::remove(fi.absoluteFilePath());
}

// ---- project files ----------------------------------------------------------------

bool CombineSession::saveProject(const CombineDocument& doc, const QString& path, bool embedAll, QString* error)
{
    QJsonObject root{{"format", "folvio-combine-project"}, {"version", 1}, {"document", doc.toJson()}};
    QJsonObject embedded;
    for (const CombinePage& p : doc.pages()) {
        if (!(p.fromClipboard || embedAll) || embedded.contains(p.id.toString()))
            continue;
        QFile f(p.sourcePath);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QObject::tr("The image of page \"%1\" cannot be read.").arg(p.displayName);
            return false;
        }
        embedded.insert(p.id.toString(QUuid::WithoutBraces),
                        QJsonObject{{"suffix", QFileInfo(p.sourcePath).suffix()},
                                    {"data", QString::fromLatin1(f.readAll().toBase64())}});
    }
    root.insert("embedded", embedded);
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Compact);
    const auto res = SafeFileWriter::writeNew(
        path,
        [&data](const QString& tmp, QString* e) {
            QFile f(tmp);
            if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
                *e = f.errorString();
                return false;
            }
            return true;
        },
        [](const QString& tmp, QString* e) {
            QFile f(tmp);
            if (!f.open(QIODevice::ReadOnly))
                return false;
            QJsonParseError pe;
            QJsonDocument::fromJson(f.readAll(), &pe);
            if (pe.error != QJsonParseError::NoError)
                *e = pe.errorString();
            return pe.error == QJsonParseError::NoError;
        },
        true);
    if (!res.ok() && error)
        *error = res.error;
    return res.ok();
}

bool CombineSession::openProject(CombineDocument& doc, const QString& path, QStringList* warnings, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QJsonParseError pe;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll(), &pe).object();
    if (pe.error != QJsonParseError::NoError || root.value("format").toString() != "folvio-combine-project") {
        if (error)
            *error = QObject::tr("This file is not a Folvio Combine project.");
        return false;
    }
    QJsonObject docJson = root.value("document").toObject();
    const QJsonObject embedded = root.value("embedded").toObject();
    QJsonArray pages = docJson.value("pages").toArray();
    for (int i = 0; i < pages.size(); ++i) {
        QJsonObject p = pages[i].toObject();
        const QJsonObject e = embedded.value(p.value("id").toString()).toObject();
        if (!e.isEmpty()) {
            const QString out = assetsDir() + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + "." + e.value("suffix").toString("png");
            QFile o(out);
            if (!o.open(QIODevice::WriteOnly) || o.write(QByteArray::fromBase64(e.value("data").toString().toLatin1())) < 0) {
                if (error)
                    *error = QObject::tr("Embedded images could not be extracted.");
                return false;
            }
            p.insert("source", out);
            p.insert("clipboard", true);
            pages[i] = p;
        }
    }
    docJson.insert("pages", pages);
    if (!doc.fromJson(docJson, error))
        return false;
    if (warnings)
        *warnings = sourceProblems(doc);
    return true;
}

QStringList CombineSession::sourceProblems(const CombineDocument& doc)
{
    QStringList problems;
    for (const CombinePage& p : doc.pages()) {
        if (!QFileInfo::exists(p.sourcePath))
            problems << QObject::tr("\"%1\": the source image is missing (%2).").arg(p.displayName, QDir::toNativeSeparators(p.sourcePath));
        else if (!p.sourceSha256.isEmpty() && sha256OfFile(p.sourcePath) != p.sourceSha256)
            problems << QObject::tr("\"%1\": the source image changed since it was added.").arg(p.displayName);
    }
    return problems;
}

} // namespace folvio::combine
