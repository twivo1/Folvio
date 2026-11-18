#pragma once

// Low-level text writing into PDFium documents. All functions require the
// caller to hold pdfiumMutex().

#include <QColor>
#include <QHash>
#include <QList>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QTransform>

#include <map>
#include <memory>
#include <vector>

struct fpdf_document_t__;
struct fpdf_page_t__;
struct fpdf_pageobject_t__;
struct fpdf_font_t__;

namespace folvio::pdfedit {

using Doc = fpdf_document_t__*;
using Page = fpdf_page_t__*;
using Obj = fpdf_pageobject_t__*;
using Font = fpdf_font_t__*;

// Removes characters that must not be written into a PDF text layer and
// normalises look-alikes: NBSP -> space, soft hyphen / zero width -> removed,
// tabs -> space.
QString sanitizeText(const QString& text);

// Text with whitespace removed - used to compare what was written with what is read back.
QString compact(const QString& text);

// A font that can be used to write new text.
struct WriterFont {
    Font handle = nullptr;
    bool original = false;       // font embedded in the PDF (usually a subset)
    QString name;                // display name
    QString filePath;            // system TrueType file (when !original)
    QSet<char32_t> allowed;      // original fonts: characters known to have glyphs + ToUnicode entries
    bool hasSpace = true;

    bool supports(const QString& text) const;
    // Advance width of text in text-space units for the given font size.
    double advance(const QString& text, double size) const;
};

// Owns the fonts used during one document rebuild.
class FontProvider {
public:
    // charset: every character that may be written with system fonts in this rebuild.
    FontProvider(Doc doc, QSet<char32_t> charset);
    ~FontProvider();
    FontProvider(const FontProvider&) = delete;
    FontProvider& operator=(const FontProvider&) = delete;

    // System TrueType font embedded as a CID font with an exact, caller-generated ToUnicode map
    // (PDFium's auto-generated map resolves e.g. '-' to U+00AD and ' ' to U+00A0).
    WriterFont* system(const QString& filePath, QString* error);
    // A font already embedded in the document. A keep-alive object retains it even when all
    // original objects using it are removed.
    WriterFont* original(const QString& pdfFontName, Font handle, const QSet<char32_t>& allowed);

private:
    Doc m_doc;
    QSet<char32_t> m_charset;
    std::map<QString, std::unique_ptr<WriterFont>> m_system;
    std::map<QString, std::unique_ptr<WriterFont>> m_original;
    std::vector<Obj> m_keepAlive;
};

struct PlacedLine {
    QList<Obj> objects;
    QString expected;   // the text that must be read back from the objects
    QRectF bounds;
    bool originalFont = false;
};

// Writes one line at the text matrix m (origin = baseline start). One object for the whole line
// when the font has a space glyph, otherwise one object per word.
PlacedLine placeLine(Doc doc, Page page, const WriterFont& font, double rawSize, const QString& text,
                     const QTransform& m, const QColor& color, int renderMode, int* insertIndex);

// Reads back the text of freshly written objects. Returns indices of lines that differ.
QList<int> verifyReadBack(Page page, const QList<PlacedLine>& lines, QStringList* details = nullptr);

QRectF objectBounds(Obj obj);
int indexOfObject(Page page, Obj obj);

} // namespace folvio::pdfedit
