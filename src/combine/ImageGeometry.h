#pragma once

#include "combine/CombineModel.h"

#include <QImage>
#include <QSize>
#include <QString>
#include <QTransform>

#include <optional>

namespace folvio::combine {

struct QuadCheck {
    bool ok = false;
    QString error;
};

// Pure geometry helpers (no image data).
class Geometry {
public:
    // Corners must be finite, inside the photo (2 % tolerance), convex, non-crossing,
    // clockwise TL->TR->BR->BL, with sensible area and angles.
    static QuadCheck validate(const Quad& q, const QSize& imageSize);
    // Assigns TL/TR/BR/BL to four unordered points (used by detection and "auto order").
    static Quad orderCorners(const std::array<QPointF, 4>& pts);
    static Quad defaultQuad(const QSize& imageSize, double insetFraction = 0.06);
    // Size of the rectified page (without margin) for a quad, honouring the aspect mode.
    // Output size of the straightened page. With AspectMode::Auto and a known photo size the
    // proportions are estimated from a pinhole camera model (principal point at the photo centre,
    // square pixels: Zhang & He, "Whiteboard scanning and image enhancement"); if the corners are not
    // consistent with such a camera, the longest opposite edges are used.
    static QSize rectifiedSize(const Quad& q, const PerspectiveSettings& s, const QSizeF& imageSize = {});
    // Estimated real height/width ratio of the photographed rectangle, if the geometry allows it.
    static std::optional<double> estimateAspect(const Quad& q, const QSizeF& imageSize);
    static QSize marginSize(const QSize& rectified, const PerspectiveSettings& s); // total size incl. margin
    // Projective transform mapping the quad onto the rectified page (incl. margin offset).
    static QTransform homography(const Quad& q, const PerspectiveSettings& s, const QSizeF& imageSize = {}, bool* ok = nullptr);
    static Quad scaled(const Quad& q, double factor);
    // Rotates quad coordinates when the image is rotated by 'degrees' (clockwise multiples of 90).
    static Quad rotated(const Quad& q, const QSize& imageSizeBefore, int degrees);
    static QSize rotatedSize(const QSize& s, int degrees);
};

class ImageProcessor {
public:
    // Image formats importable by Combine (decoded by Qt; HEIC/AVIF via FFmpeg conversion).
    static QStringList supportedSuffixes();

    // Loads an image with EXIF orientation applied. maxEdge > 0 decodes a downscaled copy.
    static QImage loadOriented(const QString& path, int maxEdge = 0, QString* error = nullptr);
    static QSize orientedSize(const QString& path);
    static QImage rotate(const QImage& img, int degrees);

    // Perspective rectification (homography + cubic interpolation). 'quad' is in pixel
    // coordinates of 'src'. Areas outside the photo are filled white.
    static QImage rectify(const QImage& src, const Quad& quad, const PerspectiveSettings& s, QString* error = nullptr,
                          double maxMegapixels = 90.0);
    // Tries to find the page outline. Returns corners in pixel coordinates of 'img'.
    static std::optional<Quad> detectPage(const QImage& img);
    static QImage applyCrop(const QImage& img, const QRectF& normalized);

    enum class Stage { Rotated, Rectified, Final };
    // Applies the page pipeline up to 'stage'. maxEdge limits the working resolution
    // (0 = full resolution, used for export).
    static QImage render(const CombinePage& page, Stage stage, int maxEdge, QString* error = nullptr,
                         double* scaleOut = nullptr);
};

} // namespace folvio::combine
