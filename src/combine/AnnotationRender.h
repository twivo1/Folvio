#pragma once

#include "combine/CombineModel.h"

#include <QPainterPath>

class QPainter;

namespace folvio::combine {

// One laid-out line of a text annotation (page units).
struct TextLineLayout {
    QString text;
    double x = 0;        // left of the line
    double baseline = 0;
    double width = 0;
};

// Geometry and painting of annotations, shared by the editor canvas and the PDF exporter
// so the exported page matches what the user saw.
class AnnotationRender {
public:
    // Centre-line path in page units for stroke-like annotations (pen, highlighter, line,
    // arrow shaft, polyline). Smoothing uses quadratic curves through segment midpoints.
    static QPainterPath centerLine(const Annotation& a);
    static QPainterPath arrowHead(const Annotation& a);  // filled triangle (Arrow only)
    static QPainterPath outline(const Annotation& a);    // Rect / Ellipse
    static double arrowHeadLength(const Annotation& a);

    // Paints in page units: the painter must already be scaled so that 1.0 == page width.
    static void paint(QPainter& p, const Annotation& a);
    static void paintAll(QPainter& p, const QList<Annotation>& list);

    static bool hitTest(const Annotation& a, const QPointF& pt, double tolerance);
    // Eraser: strokes are split where the eraser passes; other objects are removed whole.
    // Returns the replacement list for 'a' (empty = removed, {a} = untouched).
    static QList<Annotation> erase(const Annotation& a, const QPointF& pt, double radius, bool* changed);

    // Text layout (wrapping at the box width, explicit line breaks kept). Uses the metrics of
    // the TrueType file that the PDF export embeds.
    static QList<TextLineLayout> layoutText(const Annotation& a);
    // Height (page units) the text needs at the box's current width.
    static double textHeight(const Annotation& a);
    // Auto size: height = text height; fixed size: at least the text height (text is never clipped).
    static void fitTextBox(Annotation& a);
    // Resizes a text box with a handle (0..7 as in the selection frame): the width re-wraps the text,
    // the font size never changes; changing the height switches Auto size off.
    static void resizeTextBox(Annotation& a, const QRectF& startRect, const QRectF& newRect, int handle);
    static QString fontFile(const Annotation& a);
    static double textAdvance(const Annotation& a, const QString& text); // page units
};

} // namespace folvio::combine
