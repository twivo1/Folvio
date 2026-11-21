#include "combine/ImageGeometry.h"

#include <array>

#include <QImageReader>
#include <QPainter>
#include <QPolygonF>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace folvio::combine {

namespace {

double cross(const QPointF& o, const QPointF& a, const QPointF& b)
{
    return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
}

double dist(const QPointF& a, const QPointF& b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

bool segmentsIntersect(const QPointF& p1, const QPointF& p2, const QPointF& p3, const QPointF& p4)
{
    const double d1 = cross(p3, p4, p1), d2 = cross(p3, p4, p2), d3 = cross(p1, p2, p3), d4 = cross(p1, p2, p4);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

double quadArea(const Quad& q)
{
    double a = 0;
    for (int i = 0; i < 4; ++i) {
        const QPointF& p = q.pts[size_t(i)];
        const QPointF& n = q.pts[size_t((i + 1) % 4)];
        a += p.x() * n.y() - n.x() * p.y();
    }
    return a / 2.0; // positive for clockwise order in image coordinates (y down)
}

double ratioFor(const PerspectiveSettings& s, bool portrait)
{
    switch (s.aspect) {
    case AspectMode::A4: return portrait ? 297.0 / 210.0 : 210.0 / 297.0;
    case AspectMode::Letter: return portrait ? 11.0 / 8.5 : 8.5 / 11.0;
    case AspectMode::Custom: return std::max(0.05, s.customRatio);
    case AspectMode::Auto: break;
    }
    return 0;
}

// QImage <-> cv::Mat (copies; formats normalized to RGB/RGBA 8 bit).
cv::Mat toMat(const QImage& img, QImage* holder)
{
    *holder = img.hasAlphaChannel() ? img.convertToFormat(QImage::Format_RGBA8888)
                                    : img.convertToFormat(QImage::Format_RGB888);
    const int type = holder->format() == QImage::Format_RGBA8888 ? CV_8UC4 : CV_8UC3;
    return cv::Mat(holder->height(), holder->width(), type, const_cast<uchar*>(holder->constBits()),
                   size_t(holder->bytesPerLine()));
}

QImage fromMat(const cv::Mat& m)
{
    const QImage::Format f = m.type() == CV_8UC4 ? QImage::Format_RGBA8888
                             : m.type() == CV_8UC1 ? QImage::Format_Grayscale8 : QImage::Format_RGB888;
    return QImage(m.data, m.cols, m.rows, int(m.step), f).copy();
}

} // namespace

// ---- Geometry ---------------------------------------------------------------------

QuadCheck Geometry::validate(const Quad& q, const QSize& imageSize)
{
    QuadCheck r;
    const double w = imageSize.width(), h = imageSize.height();
    if (w <= 0 || h <= 0) {
        r.error = QObject::tr("The image size is unknown.");
        return r;
    }
    const double tolX = 0.02 * w, tolY = 0.02 * h;
    for (const QPointF& p : q.pts) {
        if (!std::isfinite(p.x()) || !std::isfinite(p.y())) {
            r.error = QObject::tr("A corner has an invalid position.");
            return r;
        }
        if (p.x() < -tolX || p.y() < -tolY || p.x() > w + tolX || p.y() > h + tolY) {
            r.error = QObject::tr("A corner lies outside the photo. Content that is not in the photo cannot be recovered.");
            return r;
        }
    }
    const auto& P = q.pts;
    if (segmentsIntersect(P[0], P[1], P[2], P[3]) || segmentsIntersect(P[1], P[2], P[3], P[0])) {
        r.error = QObject::tr("The page edges cross each other. Drag the corners so they form a quadrilateral.");
        return r;
    }
    const double area = quadArea(q);
    if (area < 0) {
        r.error = QObject::tr("The corners are mirrored (not in top-left, top-right, bottom-right, bottom-left order).");
        return r;
    }
    if (area < 0.005 * w * h) {
        r.error = QObject::tr("The selected area is too small.");
        return r;
    }
    // Convexity and angles.
    for (int i = 0; i < 4; ++i) {
        const QPointF& a = P[size_t((i + 3) % 4)];
        const QPointF& b = P[size_t(i)];
        const QPointF& c = P[size_t((i + 1) % 4)];
        if (dist(a, b) < 8 || dist(b, c) < 8) {
            r.error = QObject::tr("Two corners are too close together.");
            return r;
        }
        if (cross(b, c, a) <= 0) { // turn direction must be consistent (clockwise)
            r.error = QObject::tr("The selected shape is not convex.");
            return r;
        }
        const QPointF u = a - b, v = c - b;
        const double cosang = (u.x() * v.x() + u.y() * v.y()) / (std::hypot(u.x(), u.y()) * std::hypot(v.x(), v.y()));
        const double deg = std::acos(std::clamp(cosang, -1.0, 1.0)) * 180.0 / std::numbers::pi;
        if (deg < 12 || deg > 168) {
            r.error = QObject::tr("The shape is too distorted to be a page (corner angle %1°).").arg(int(deg));
            return r;
        }
    }
    r.ok = true;
    return r;
}

Quad Geometry::orderCorners(const std::array<QPointF, 4>& in)
{
    // Sort around the centroid by angle (clockwise in image coordinates), then start at the
    // corner with the smallest x + y (top-left).
    QPointF c(0, 0);
    for (const QPointF& p : in)
        c += p / 4.0;
    std::array<QPointF, 4> pts = in;
    std::sort(pts.begin(), pts.end(), [&](const QPointF& a, const QPointF& b) {
        return std::atan2(a.y() - c.y(), a.x() - c.x()) < std::atan2(b.y() - c.y(), b.x() - c.x());
    });
    int start = 0;
    for (int i = 1; i < 4; ++i)
        if (pts[size_t(i)].x() + pts[size_t(i)].y() < pts[size_t(start)].x() + pts[size_t(start)].y())
            start = i;
    Quad q;
    for (int i = 0; i < 4; ++i)
        q.pts[size_t(i)] = pts[size_t((start + i) % 4)];
    return q;
}

Quad Geometry::defaultQuad(const QSize& s, double inset)
{
    const double dx = s.width() * inset, dy = s.height() * inset;
    Quad q;
    q.pts = {QPointF(dx, dy), QPointF(s.width() - dx, dy), QPointF(s.width() - dx, s.height() - dy),
             QPointF(dx, s.height() - dy)};
    return q;
}

std::optional<double> Geometry::estimateAspect(const Quad& q, const QSizeF& imageSize)
{
    if (!imageSize.isValid() || imageSize.isEmpty())
        return std::nullopt;
    using V = std::array<double, 3>;
    auto cross = [](const V& a, const V& b) {
        return V{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    auto dot = [](const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    // Normalise coordinates by the photo diagonal for numerical stability; the principal point is the centre.
    const double scale = std::hypot(imageSize.width(), imageSize.height());
    const double u0 = imageSize.width() / 2 / scale, v0 = imageSize.height() / 2 / scale;
    auto hv = [&](const QPointF& p) { return V{p.x() / scale, p.y() / scale, 1.0}; };
    const V m1 = hv(q.pts[0]), m2 = hv(q.pts[1]), m3 = hv(q.pts[3]), m4 = hv(q.pts[2]); // TL, TR, BL, BR
    const double d2 = dot(cross(m2, m4), m3), d3 = dot(cross(m3, m4), m2);
    if (std::abs(d2) < 1e-12 || std::abs(d3) < 1e-12)
        return std::nullopt;
    const double k2 = dot(cross(m1, m4), m3) / d2;
    const double k3 = dot(cross(m1, m4), m2) / d3;
    V n2, n3;
    for (int i = 0; i < 3; ++i) {
        n2[size_t(i)] = k2 * m2[size_t(i)] - m1[size_t(i)];
        n3[size_t(i)] = k3 * m3[size_t(i)] - m1[size_t(i)];
    }
    // A vanishing point at infinity (that pair of edges parallel in the photo) makes the focal
    // length unobservable; then a typical phone camera focal length (0.7 x diagonal) is assumed.
    constexpr double kTypicalFocal = 0.7;
    const double rel2 = std::abs(n2[2]) / std::max(1e-12, std::hypot(n2[0], n2[1]));
    const double rel3 = std::abs(n3[2]) / std::max(1e-12, std::hypot(n3[0], n3[1]));
    constexpr double kParallel = 0.03;
    double f = kTypicalFocal;
    if (rel2 >= kParallel && rel3 >= kParallel) {
        const double nz = n2[2] * n3[2];
        const double f2 = -((n2[0] * n3[0] - (n2[0] * n3[2] + n2[2] * n3[0]) * u0 + nz * u0 * u0)
                            + (n2[1] * n3[1] - (n2[1] * n3[2] + n2[2] * n3[1]) * v0 + nz * v0 * v0))
                          / nz;
        // Plausible lenses only (very wide angle to long telephoto, in units of the diagonal);
        // otherwise the corners are not consistent with a centred camera and the default is used.
        if (f2 > 0 && std::sqrt(f2) > 0.3 && std::sqrt(f2) < 20)
            f = std::sqrt(f2);
    }
    auto norm2 = [&](const V& n) {
        const double x = (n[0] - u0 * n[2]) / f, y = (n[1] - v0 * n[2]) / f;
        return x * x + y * y + n[2] * n[2];
    };
    const double na = norm2(n2), nb = norm2(n3);
    if (na <= 0)
        return std::nullopt;
    const double ratio = std::sqrt(nb / na);
    if (!std::isfinite(ratio) || ratio < 0.1 || ratio > 10)
        return std::nullopt;
    return ratio;
}

QSize Geometry::rectifiedSize(const Quad& q, const PerspectiveSettings& s, const QSizeF& imageSize)
{
    const auto& P = q.pts;
    // Longest opposite edges keep the most detail.
    double w = std::max(dist(P[0], P[1]), dist(P[3], P[2]));
    double h = std::max(dist(P[0], P[3]), dist(P[1], P[2]));
    w = std::max(w, 1.0);
    h = std::max(h, 1.0);
    // The camera model also decides portrait vs landscape for fixed paper sizes: under strong
    // perspective the edge lengths alone can point the wrong way.
    const std::optional<double> estimated = estimateAspect(q, imageSize);
    double ratio = ratioFor(s, estimated ? *estimated >= 1.0 : h >= w);
    if (ratio <= 0 && s.aspect == AspectMode::Auto)
        ratio = estimated.value_or(0.0);
    if (ratio > 0) {
        // Same pixel count, target proportions.
        const double area = w * h;
        w = std::sqrt(area / ratio);
        h = w * ratio;
    }
    return QSize(std::max(1, int(std::lround(w))), std::max(1, int(std::lround(h))));
}

QSize Geometry::marginSize(const QSize& r, const PerspectiveSettings& s)
{
    const int m = int(std::lround(r.width() * std::max(0.0, s.marginPercent) / 100.0));
    return QSize(r.width() + 2 * m, r.height() + 2 * m);
}

QTransform Geometry::homography(const Quad& q, const PerspectiveSettings& s, const QSizeF& imageSize, bool* ok)
{
    const QSize r = rectifiedSize(q, s, imageSize);
    const double m = r.width() * std::max(0.0, s.marginPercent) / 100.0;
    const QPolygonF from({q.pts[0], q.pts[1], q.pts[2], q.pts[3]});
    const QPolygonF to({QPointF(m, m), QPointF(m + r.width(), m), QPointF(m + r.width(), m + r.height()),
                        QPointF(m, m + r.height())});
    QTransform t;
    const bool good = QTransform::quadToQuad(from, to, t);
    if (ok)
        *ok = good;
    return t;
}

Quad Geometry::scaled(const Quad& q, double f)
{
    Quad r = q;
    for (QPointF& p : r.pts)
        p *= f;
    return r;
}

QSize Geometry::rotatedSize(const QSize& s, int degrees)
{
    return (degrees % 180) ? s.transposed() : s;
}

Quad Geometry::rotated(const Quad& q, const QSize& before, int degrees)
{
    degrees = ((degrees % 360) + 360) % 360;
    Quad r = q;
    for (QPointF& p : r.pts) {
        const QPointF o = p;
        switch (degrees) {
        case 90: p = QPointF(before.height() - o.y(), o.x()); break;
        case 180: p = QPointF(before.width() - o.x(), before.height() - o.y()); break;
        case 270: p = QPointF(o.y(), before.width() - o.x()); break;
        default: break;
        }
    }
    // Keep the TL/TR/BR/BL meaning relative to the new orientation.
    return orderCorners(r.pts);
}

QSizeF Geometry::rectifiedPixelSize(const CombinePage& page)
{
    QSize px = rotatedSize(page.sourceSize, page.rotation);
    if (page.quad)
        px = marginSize(rectifiedSize(*page.quad, page.perspective, QSizeF(px)), page.perspective);
    return QSizeF(px);
}

void Geometry::remapAnnotationsForCrop(CombinePage& page, const QRectF& oldCrop, const QRectF& newCrop)
{
    const QSizeF r = rectifiedPixelSize(page);
    const double oldW = oldCrop.width() * r.width(), newW = newCrop.width() * r.width();
    if (page.annotations.isEmpty() || oldW <= 0 || newW <= 0)
        return;
    // u (old page units) -> rectified px -> u' (new page units)
    const double scale = oldW / newW;
    const QPointF offset((oldCrop.x() - newCrop.x()) * r.width() / newW, (oldCrop.y() - newCrop.y()) * r.height() / newW);
    for (Annotation& a : page.annotations)
        a.transform([&](const QPointF& u) { return u * scale + offset; }, scale);
}

void Geometry::remapAnnotationsForRotation(CombinePage& page, int degrees)
{
    degrees = ((degrees % 360) + 360) % 360;
    const QSizeF r = rectifiedPixelSize(page); // before the rotation
    if (page.annotations.isEmpty() || degrees == 0 || r.width() <= 0 || r.height() <= 0)
        return;
    const double k = r.width() / r.height(); // W/H of the page before rotating
    std::function<QPointF(const QPointF&)> map;
    double scale = 1.0;
    switch (degrees) {
    case 90: map = [k](const QPointF& u) { return QPointF(1.0 - u.y() * k, u.x() * k); }; scale = k; break;
    case 180: map = [k](const QPointF& u) { return QPointF(1.0 - u.x(), 1.0 / k - u.y()); }; break;
    case 270: map = [k](const QPointF& u) { return QPointF(u.y() * k, (1.0 - u.x()) * k); }; scale = k; break;
    default: return;
    }
    for (Annotation& a : page.annotations)
        a.transform(map, scale);
}

// ---- ImageProcessor ------------------------------------------------------------------

QStringList ImageProcessor::supportedSuffixes()
{
    return {"png", "jpg", "jpeg", "jpe", "webp", "bmp", "dib", "tif", "tiff", "gif", "heic", "heif", "avif"};
}

QImage ImageProcessor::loadOriented(const QString& path, int maxEdge, QString* error)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    reader.setAllocationLimit(2048); // MB
    if (maxEdge > 0) {
        QSize s = reader.size();
        if (s.isValid()) {
            // reader.size() is before EXIF rotation; scaling is proportional either way.
            if (std::max(s.width(), s.height()) > maxEdge) {
                s.scale(maxEdge, maxEdge, Qt::KeepAspectRatio);
                reader.setScaledSize(s);
            }
        }
    }
    QImage img = reader.read();
    if (img.isNull()) {
        if (error)
            *error = QObject::tr("The image cannot be decoded: %1").arg(reader.errorString());
        return {};
    }
    if (maxEdge > 0 && std::max(img.width(), img.height()) > maxEdge)
        img = img.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
}

QSize ImageProcessor::orientedSize(const QString& path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize s = reader.size();
    const QImageIOHandler::Transformations t = reader.transformation();
    if (t & QImageIOHandler::TransformationRotate90)
        s.transpose();
    return s;
}

QImage ImageProcessor::rotate(const QImage& img, int degrees)
{
    degrees = ((degrees % 360) + 360) % 360;
    if (degrees == 0)
        return img;
    QTransform t;
    t.rotate(degrees);
    return img.transformed(t); // exact for multiples of 90
}

QImage ImageProcessor::rectify(const QImage& src, const Quad& quad, const PerspectiveSettings& s, QString* error,
                               double maxMegapixels)
{
    const QuadCheck check = Geometry::validate(quad, src.size());
    if (!check.ok) {
        if (error)
            *error = check.error;
        return {};
    }
    QSize page = Geometry::rectifiedSize(quad, s, QSizeF(src.size()));
    QSize total = Geometry::marginSize(page, s);
    double scale = 1.0;
    const double mp = double(total.width()) * total.height() / 1e6;
    if (mp > maxMegapixels)
        scale = std::sqrt(maxMegapixels / mp);
    const double m = page.width() * std::max(0.0, s.marginPercent) / 100.0 * scale;
    const double pw = page.width() * scale, ph = page.height() * scale;
    const int outW = std::max(1, int(std::lround(pw + 2 * m))), outH = std::max(1, int(std::lround(ph + 2 * m)));

    std::vector<cv::Point2f> from, to;
    for (const QPointF& p : quad.pts)
        from.emplace_back(float(p.x()), float(p.y()));
    to = {{float(m), float(m)}, {float(m + pw), float(m)}, {float(m + pw), float(m + ph)}, {float(m), float(m + ph)}};
    const cv::Mat H = cv::getPerspectiveTransform(from, to);
    if (H.empty() || std::abs(cv::determinant(H)) < 1e-12) {
        if (error)
            *error = QObject::tr("The perspective transform is degenerate.");
        return {};
    }
    QImage holder;
    const cv::Mat in = toMat(src, &holder);
    cv::Mat out;
    // Cubic interpolation keeps text sharp; INTER_AREA-like quality for strong downscaling is not
    // needed because the output resolution follows the quad's edge lengths.
    const cv::Scalar white = in.channels() == 4 ? cv::Scalar(255, 255, 255, 255) : cv::Scalar(255, 255, 255);
    cv::warpPerspective(in, out, H, cv::Size(outW, outH), cv::INTER_CUBIC, cv::BORDER_CONSTANT, white);
    QImage result = fromMat(out);
    result.setDotsPerMeterX(src.dotsPerMeterX());
    result.setDotsPerMeterY(src.dotsPerMeterY());
    return result;
}

std::optional<Quad> ImageProcessor::detectPage(const QImage& img)
{
    if (img.isNull())
        return std::nullopt;
    // Work on a ~1000 px copy for speed and noise robustness.
    const double scale = std::min(1.0, 1000.0 / std::max(img.width(), img.height()));
    const QImage small = img.scaled(int(img.width() * scale), int(img.height() * scale), Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_Grayscale8);
    cv::Mat gray(small.height(), small.width(), CV_8UC1, const_cast<uchar*>(small.constBits()), size_t(small.bytesPerLine()));
    cv::Mat blurred, edges;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);
    const double imgArea = double(gray.cols) * gray.rows;
    std::optional<Quad> best;
    double bestArea = 0;
    auto consider = [&](const std::vector<cv::Point>& approx) {
        if (approx.size() != 4 || !cv::isContourConvex(approx))
            return;
        const double area = std::abs(cv::contourArea(approx));
        if (area < 0.15 * imgArea || area > 0.995 * imgArea || area <= bestArea)
            return;
        std::array<QPointF, 4> pts;
        for (int i = 0; i < 4; ++i)
            pts[size_t(i)] = QPointF(approx[size_t(i)].x / scale, approx[size_t(i)].y / scale);
        const Quad q = Geometry::orderCorners(pts);
        if (!Geometry::validate(q, img.size()).ok)
            return;
        bestArea = area;
        best = q;
    };
    // Try a few edge thresholds; documents photographed on a table usually give a strong outline.
    for (const auto& [lo, hi] : {std::pair{50, 150}, std::pair{25, 90}, std::pair{75, 200}}) {
        cv::Canny(blurred, edges, lo, hi);
        cv::dilate(edges, edges, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(edges, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
        for (const auto& c : contours) {
            const double peri = cv::arcLength(c, true);
            for (double eps : {0.02, 0.03, 0.05}) {
                std::vector<cv::Point> approx;
                cv::approxPolyDP(c, approx, eps * peri, true);
                consider(approx);
            }
        }
        if (best)
            break;
    }
    // Fallback: bright page on darker background via Otsu threshold.
    if (!best) {
        cv::Mat bin;
        cv::threshold(blurred, bin, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(bin, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        for (const auto& c : contours) {
            const double peri = cv::arcLength(c, true);
            std::vector<cv::Point> approx;
            cv::approxPolyDP(c, approx, 0.03 * peri, true);
            consider(approx);
        }
    }
    return best;
}

QImage ImageProcessor::applyCrop(const QImage& img, const QRectF& n)
{
    const QRectF unit(0, 0, 1, 1);
    const QRectF r = n.normalized().intersected(unit);
    if (r.width() <= 0 || r.height() <= 0)
        return img;
    const QRect px(int(std::floor(r.x() * img.width())), int(std::floor(r.y() * img.height())),
                   std::max(1, int(std::lround(r.width() * img.width()))),
                   std::max(1, int(std::lround(r.height() * img.height()))));
    return img.copy(px.intersected(img.rect()));
}

QImage ImageProcessor::render(const CombinePage& page, Stage stage, int maxEdge, QString* error, double* scaleOut)
{
    QImage img = loadOriented(page.sourcePath, maxEdge, error);
    if (img.isNull())
        return {};
    // Scale between the working copy and full-resolution oriented coordinates.
    const QSize full = page.sourceSize.isValid() ? page.sourceSize : img.size();
    const double scale = full.width() > 0 ? double(img.width()) / full.width() : 1.0;
    if (scaleOut)
        *scaleOut = scale;
    img = rotate(img, page.rotation);
    if (stage == Stage::Rotated)
        return img;
    if (page.quad) {
        QString err;
        const QImage rect = rectify(img, Geometry::scaled(*page.quad, scale), page.perspective, &err);
        if (rect.isNull()) {
            if (error)
                *error = err;
        } else {
            img = rect;
        }
    }
    if (stage == Stage::Rectified)
        return img;
    if (page.crop)
        img = applyCrop(img, *page.crop);
    return img;
}

} // namespace folvio::combine
