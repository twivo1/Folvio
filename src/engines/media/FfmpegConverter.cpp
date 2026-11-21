#include "engines/media/FfmpegConverter.h"

#include "core/conversions/OutputValidator.h"
#include "core/deps/DependencyManager.h"
#include "core/files/FileFormat.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace folvio {

namespace {

const QStringList kVideoIn = {"mp4", "mkv", "mov", "avi", "webm"};
const QStringList kVideoOut = {"mp4", "mkv", "mov", "avi", "webm"};
const QStringList kAudio = {"mp3", "wav", "flac", "m4a", "ogg"};

bool isAudio(const QString& f) { return kAudio.contains(f); }

OptionSpec choice(const QString& key, const QString& label, const QStringList& values, const QStringList& labels,
                  const QString& def, const QString& tip = {})
{
    OptionSpec o;
    o.key = key;
    o.label = label;
    o.choiceValues = values;
    o.choiceLabels = labels;
    o.defaultValue = def;
    o.tooltip = tip;
    return o;
}

QStringList audioEncoderArgs(const QString& target, const QString& bitrate, const QString& sourceCodec, bool allowCopy)
{
    // Copy when the source codec already matches the target (no generation loss).
    if (allowCopy) {
        if ((target == "mp3" && sourceCodec == "mp3") || (target == "m4a" && sourceCodec == "aac")
            || (target == "flac" && sourceCodec == "flac") || (target == "ogg" && sourceCodec == "vorbis"))
            return {"-c:a", "copy"};
    }
    if (target == "mp3") return {"-c:a", "libmp3lame", "-b:a", bitrate};
    if (target == "m4a") return {"-c:a", "aac", "-b:a", bitrate};
    if (target == "ogg") return {"-c:a", "libvorbis", "-b:a", bitrate};
    if (target == "flac") return {"-c:a", "flac", "-compression_level", "8"};
    if (target == "wav") return {"-c:a", "pcm_s16le"};
    return {};
}

} // namespace

bool FfmpegConverter::containerAccepts(const QString& c, const QString& v, const QString& a)
{
    auto ok = [](const QStringList& list, const QString& codec) { return codec.isEmpty() || list.contains(codec); };
    if (c == "mkv")
        return true; // Matroska holds practically any codec
    if (c == "mp4")
        return ok({"h264", "hevc", "av1", "mpeg4", "vp9"}, v) && ok({"aac", "mp3", "ac3", "eac3", "alac", "opus", "flac"}, a);
    if (c == "mov")
        return ok({"h264", "hevc", "prores", "mpeg4", "mjpeg"}, v) && ok({"aac", "alac", "pcm_s16le", "pcm_s24le", "mp3"}, a);
    if (c == "webm")
        return ok({"vp8", "vp9", "av1"}, v) && ok({"opus", "vorbis"}, a);
    if (c == "avi")
        return ok({"mpeg4", "h264", "mjpeg", "msmpeg4v3"}, v) && ok({"mp3", "ac3", "pcm_s16le"}, a);
    return false;
}

QList<ConversionCapability> FfmpegConverter::capabilities() const
{
    QList<ConversionCapability> caps;
    for (const QString& in : kVideoIn) {
        for (const QString& out : kVideoOut)
            if (in != out)
                caps << ConversionCapability{in, out, 90, true,
                                             QObject::tr("Streams are copied without re-encoding when the codecs fit the target container.")};
        for (const QString& a : kAudio)
            caps << ConversionCapability{in, a, 90, a != "wav" && a != "flac", {}};
        caps << ConversionCapability{in, "gif", 70, true, QObject::tr("GIF is limited to 256 colours.")};
        caps << ConversionCapability{in, "png", 90, false, QObject::tr("Exports frames as numbered PNG images.")};
        caps << ConversionCapability{in, "jpg", 85, true, QObject::tr("Exports frames as numbered JPEG images.")};
    }
    for (const QString& in : kAudio)
        for (const QString& out : kAudio)
            if (in != out) {
                ConversionCapability c{in, out, 90, out != "wav" && out != "flac", {}};
                if ((in == "mp3" || in == "m4a" || in == "ogg") && (out == "flac" || out == "wav"))
                    c.note = QObject::tr("A lossless container does not restore quality lost in the lossy source.");
                caps << c;
            }
    for (const QString& out : {"mp4", "webm", "mkv"})
        caps << ConversionCapability{"gif", out, 90, true, {}};
    return caps;
}

QList<OptionSpec> FfmpegConverter::options(const QString& from, const QString& to) const
{
    QList<OptionSpec> o;
    const QStringList bitrates = {"320k", "256k", "192k", "160k", "128k", "96k"};
    if (kVideoOut.contains(to)) {
        o << choice("videoCodec", QObject::tr("Video codec"), {"auto", "h264", "h265", "vp9", "av1"},
                    {QObject::tr("Automatic (copy if possible)"), "H.264", "H.265 / HEVC", "VP9", "AV1"}, "auto",
                    QObject::tr("Automatic keeps the original streams when the target container supports them."));
        o << choice("quality", QObject::tr("Quality"), {"high", "medium", "small"},
                    {QObject::tr("High"), QObject::tr("Balanced"), QObject::tr("Smaller file")}, "medium");
        o << choice("height", QObject::tr("Resolution"), {"0", "2160", "1440", "1080", "720", "480"},
                    {QObject::tr("Original"), "2160p", "1440p", "1080p", "720p", "480p"}, "0");
        o << choice("fps", QObject::tr("Frame rate"), {"0", "60", "30", "25", "24"},
                    {QObject::tr("Original"), "60", "30", "25", "24"}, "0");
        o << choice("audioBitrate", QObject::tr("Audio bitrate"), bitrates, bitrates, "192k");
    } else if (isAudio(to)) {
        if (to != "wav" && to != "flac")
            o << choice("audioBitrate", QObject::tr("Bitrate"), bitrates, bitrates, "192k");
        o << choice("sampleRate", QObject::tr("Sample rate"), {"0", "48000", "44100"},
                    {QObject::tr("Original"), "48 kHz", "44.1 kHz"}, "0");
        o << choice("channels", QObject::tr("Channels"), {"0", "2", "1"},
                    {QObject::tr("Original"), QObject::tr("Stereo"), QObject::tr("Mono")}, "0");
    } else if (to == "gif") {
        o << choice("gifFps", QObject::tr("Frame rate"), {"10", "15", "24"}, {"10", "15", "24"}, "15");
        o << choice("gifWidth", QObject::tr("Width"), {"320", "480", "640", "800", "0"},
                    {"320 px", "480 px", "640 px", "800 px", QObject::tr("Original")}, "480");
        o << choice("maxSeconds", QObject::tr("Length"), {"10", "30", "60", "0"},
                    {QObject::tr("First 10 s"), QObject::tr("First 30 s"), QObject::tr("First 60 s"), QObject::tr("Whole video")}, "30");
    } else if (to == "png" || to == "jpg") {
        o << choice("frameRate", QObject::tr("Frames"), {"1", "0.2", "5", "all"},
                    {QObject::tr("1 per second"), QObject::tr("1 every 5 seconds"), QObject::tr("5 per second"),
                     QObject::tr("Every frame")}, "1");
    }
    Q_UNUSED(from)
    return o;
}

ConversionResult FfmpegConverter::convert(const ConversionRequest& request, ConversionContext& ctx)
{
    const QString ffmpeg = DependencyManager::instance().path("ffmpeg");
    const OutputValidator::MediaProbe probe = OutputValidator::probeMedia(request.inputPath);
    if (!probe.ok)
        return ConversionResult::failure(QObject::tr("The media file cannot be read: %1").arg(probe.error));

    const QString to = request.outputFormat;
    const QVariantMap& opt = request.options;
    const bool wantAudioOnly = isAudio(to);
    if (wantAudioOnly && !probe.hasAudio)
        return ConversionResult::failure(QObject::tr("This file has no audio track to extract."));
    if (!wantAudioOnly && !probe.hasVideo && request.inputFormat != "gif")
        return ConversionResult::failure(QObject::tr("This file has no video stream."));

    QStringList args = {"-hide_banner", "-nostdin", "-loglevel", "error", "-progress", "pipe:1", "-nostats",
                        "-protocol_whitelist", "file,pipe"};
    QString outputPattern = request.outputPath;
    QString framesDir;
    double expectedDuration = probe.durationSec;

    if (to == "gif") {
        const int maxSeconds = opt.value("maxSeconds", "30").toInt();
        if (maxSeconds > 0) {
            args << "-t" << QString::number(maxSeconds);
            expectedDuration = std::min(expectedDuration > 0 ? expectedDuration : maxSeconds, double(maxSeconds));
        }
    }
    args << "-i" << "file:" + request.inputPath;

    const QString audioBitrate = opt.value("audioBitrate", "192k").toString();
    if (wantAudioOnly) {
        args << "-map" << "0:a:0" << "-vn" << "-sn" << "-dn";
        const bool resample = opt.value("sampleRate", "0").toInt() > 0 || opt.value("channels", "0").toInt() > 0;
        args << audioEncoderArgs(to, audioBitrate, probe.audioCodec, !resample);
        if (const int sr = opt.value("sampleRate", "0").toInt(); sr > 0)
            args << "-ar" << QString::number(sr);
        if (const int ch = opt.value("channels", "0").toInt(); ch > 0)
            args << "-ac" << QString::number(ch);
        if (to == "mp3")
            args << "-id3v2_version" << "3";
    } else if (to == "gif") {
        const QString fps = opt.value("gifFps", "15").toString();
        const int width = opt.value("gifWidth", "480").toInt();
        const QString scale = width > 0 ? QStringLiteral("scale=%1:-1:flags=lanczos,").arg(width) : QString();
        args << "-filter_complex"
             << QStringLiteral("[0:v:0]fps=%1,%2split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4")
                    .arg(fps, scale)
             << "-loop" << "0";
    } else if (to == "png" || to == "jpg") {
        framesDir = QFileInfo(request.outputPath).absolutePath() + "/frames";
        QDir().mkpath(framesDir);
        const QString rate = opt.value("frameRate", "1").toString();
        args << "-map" << "0:V:0";
        if (rate != "all")
            args << "-vf" << "fps=" + rate;
        args << "-frames:v" << "20000" << "-fps_mode" << "passthrough";
        if (to == "jpg")
            args << "-q:v" << "2";
        outputPattern = framesDir + "/" + QFileInfo(request.outputPath).completeBaseName() + "_%05d." + to;
    } else {
        // Video container target.
        const QString codec = opt.value("videoCodec", "auto").toString();
        const int height = opt.value("height", "0").toInt();
        const int fps = opt.value("fps", "0").toInt();
        const bool filtersRequested = height > 0 || fps > 0;
        const bool canCopy = codec == "auto" && !filtersRequested && request.inputFormat != "gif"
                             && containerAccepts(to, probe.videoCodec, probe.audioCodec);
        args << "-map" << "0:V:0?" << "-map" << "0:a?";
        if (canCopy) {
            args << "-c" << "copy";
        } else {
            const QString q = opt.value("quality", "medium").toString();
            const int crfX264 = q == "high" ? 18 : q == "small" ? 28 : 23;
            QString vcodec = codec;
            if (vcodec == "auto")
                vcodec = to == "webm" ? "vp9" : (to == "avi" ? "mpeg4" : "h264");
            if (to == "webm" && vcodec != "vp9" && vcodec != "av1")
                vcodec = "vp9";
            if (vcodec == "h264")
                args << "-c:v" << "libx264" << "-preset" << "medium" << "-crf" << QString::number(crfX264);
            else if (vcodec == "h265")
                args << "-c:v" << "libx265" << "-preset" << "medium" << "-crf" << QString::number(crfX264 + 5)
                     << "-tag:v" << "hvc1";
            else if (vcodec == "vp9")
                args << "-c:v" << "libvpx-vp9" << "-b:v" << "0" << "-crf" << QString::number(crfX264 + 10)
                     << "-row-mt" << "1" << "-deadline" << "good" << "-cpu-used" << "4";
            else if (vcodec == "av1")
                args << "-c:v" << "libsvtav1" << "-crf" << QString::number(crfX264 + 12) << "-preset" << "8";
            else if (vcodec == "mpeg4")
                args << "-c:v" << "mpeg4" << "-q:v" << (q == "high" ? "2" : q == "small" ? "8" : "4");
            QStringList filters;
            if (height > 0)
                filters << QStringLiteral("scale=-2:'min(%1,ih)'").arg(height);
            else
                filters << "scale=trunc(iw/2)*2:trunc(ih/2)*2"; // encoders need even dimensions
            if (fps > 0)
                filters << QStringLiteral("fps=%1").arg(fps);
            args << "-vf" << filters.join(',') << "-pix_fmt" << "yuv420p";
            // Audio: keep compatible audio, otherwise encode for the container.
            if (probe.hasAudio) {
                if (containerAccepts(to, {}, probe.audioCodec))
                    args << "-c:a" << "copy";
                else if (to == "webm")
                    args << "-c:a" << "libopus" << "-b:a" << audioBitrate;
                else if (to == "avi")
                    args << "-c:a" << "libmp3lame" << "-b:a" << audioBitrate;
                else
                    args << "-c:a" << "aac" << "-b:a" << audioBitrate;
            }
        }
        if (to == "mp4" || to == "mov")
            args << "-movflags" << "+faststart";
    }
    args << "-y" << "file:" + outputPattern;

    ProcessSpec spec;
    spec.program = ffmpeg;
    spec.arguments = args;
    spec.timeoutMs = 6 * 60 * 60 * 1000; // long videos; cancel stays available
    const double total = std::max(0.001, expectedDuration);
    spec.onStdout = [&ctx, total](const QByteArray& chunk) {
        static const QRegularExpression re(QStringLiteral("out_time_us=(\\d+)"));
        auto it = re.globalMatch(QString::fromLatin1(chunk));
        qint64 us = -1;
        while (it.hasNext())
            us = it.next().captured(1).toLongLong();
        if (us >= 0)
            ctx.stepProgress(std::min(0.99, (us / 1e6) / total), QObject::tr("Encoding"));
    };
    const ProcessResult pr = ctx.run(spec);
    if (pr.cancelled) {
        ConversionResult r;
        r.cancelled = true;
        return r;
    }
    if (!pr.succeeded())
        return ConversionResult::failure(pr.describe());

    ConversionResult r;
    r.ok = true;
    if (!framesDir.isEmpty()) {
        for (const QFileInfo& fi : QDir(framesDir).entryInfoList({"*." + to}, QDir::Files, QDir::Name))
            r.outputs << fi.absoluteFilePath();
        if (r.outputs.isEmpty())
            return ConversionResult::failure(QObject::tr("No frames were extracted."));
    } else {
        r.outputs << request.outputPath;
    }
    return r;
}

} // namespace folvio
