#pragma once

#include <QString>
#include <QStringList>

namespace folvio {

struct FontMatch {
    QString family;      // display family, e.g. "Calibri"
    QString filePath;    // TrueType file used for new text
    bool exact = false;  // same family as the PDF font
    bool bold = false;
    bool italic = false;
    bool isValid() const { return !filePath.isEmpty(); }
};

// Maps PDF font names (e.g. "ABCDEF+Calibri-Bold", "TimesNewRomanPSMT",
// "Helvetica-Oblique") to an installed TrueType font with full Unicode
// coverage, so edited text can contain any character (including Polish ones).
class FontMatcher {
public:
    static FontMatch match(const QString& pdfFontName, bool bold, bool italic, bool serif, bool monospace);
    static FontMatch byFamily(const QString& family, bool bold, bool italic);
    static QStringList availableFamilies(); // families offered in the editor's font box
    static QStringList fontDirectories();
};

} // namespace folvio
