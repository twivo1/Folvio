#include "editor/pdf_native/PdfTextWriter.h"

#include <QFile>
#include <QMap>
#include <QMutex>
#include <QRawFont>
#include <QRegularExpression>

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <cmath>

namespace folvio::pdfedit {

namespace {

QMutex& cacheMutex()
{
    static QMutex m;
    return m;
}

QByteArray fontFileData(const QString& path)
{
    static QHash<QString, QByteArray> cache;
    QMutexLocker lock(&cacheMutex());
    auto it = cache.find(path);
    if (it != cache.end())
        return *it;
    QFile f(path);
    const QByteArray data = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    cache.insert(path, data);
    return data;
}

const QRawFont& rawFont(const QString& path)
{
    static QHash<QString, QRawFont> cache;
    QMutexLocker lock(&cacheMutex());
    auto it = cache.find(path);
    if (it == cache.end())
        it = cache.insert(path, QRawFont(path, 100.0));
    return *it;
}

QList<char32_t> codePoints(const QString& s)
{
    QList<char32_t> out;
    for (char32_t c : s.toUcs4())
        out << c;
    return out;
}

QString utf16Hex(char32_t c)
{
    const QString s = QString::fromUcs4(&c, 1);
    QString hex;
    for (QChar ch : s)
        hex += QStringLiteral("%1").arg(uint(ch.unicode()), 4, 16, QLatin1Char('0')).toUpper();
    return hex;
}

// ToUnicode CMap mapping each glyph id (= CID, identity CIDToGIDMap) to exactly one code point.
QByteArray buildToUnicode(const QRawFont& raw, const QSet<char32_t>& charset)
{
    // Prefer characters in ascending order so that '-' (U+002D) wins over U+00AD and
    // ' ' (U+0020) over U+00A0 when both share a glyph.
    QList<char32_t> chars(charset.begin(), charset.end());
    std::sort(chars.begin(), chars.end());
    QMap<quint32, char32_t> map;
    for (char32_t c : chars) {
        const QList<quint32> g = raw.glyphIndexesForString(QString::fromUcs4(&c, 1));
        if (g.size() != 1 || g[0] == 0 || map.contains(g[0]))
            continue;
        map.insert(g[0], c);
    }
    QByteArray out = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                     "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                     "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
                     "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    auto it = map.constBegin();
    while (it != map.constEnd()) {
        QByteArray chunk;
        int n = 0;
        for (; it != map.constEnd() && n < 100; ++it, ++n)
            chunk += QStringLiteral("<%1> <%2>\n").arg(it.key(), 4, 16, QLatin1Char('0')).arg(utf16Hex(it.value())).toUpper().toLatin1();
        out += QByteArray::number(n) + " beginbfchar\n" + chunk + "endbfchar\n";
    }
    out += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    return out;
}

QByteArray identityCidToGid(const QRawFont& raw)
{
    const QByteArray maxp = raw.fontTable("maxp");
    const int numGlyphs = maxp.size() >= 6 ? (uchar(maxp[4]) << 8) | uchar(maxp[5]) : 0;
    QByteArray map;
    map.reserve(numGlyphs * 2);
    for (int g = 0; g < numGlyphs; ++g) {
        map.append(char(g >> 8));
        map.append(char(g & 0xFF));
    }
    return map;
}

Obj createObject(Doc doc, Page page, Font font, double rawSize, const QString& text, const QTransform& m,
                 const QColor& color, int renderMode, int* insertIndex)
{
    Obj obj = FPDFPageObj_CreateTextObj(doc, font, float(rawSize));
    if (!obj)
        return nullptr;
    const std::u16string utf16 = text.toStdU16String();
    FPDFText_SetText(obj, reinterpret_cast<FPDF_WIDESTRING>(utf16.c_str()));
    FPDFPageObj_SetFillColor(obj, uint(color.red()), uint(color.green()), uint(color.blue()), uint(color.alpha()));
    FPDFTextObj_SetTextRenderMode(obj, FPDF_TEXT_RENDERMODE(renderMode));
    FS_MATRIX fm{float(m.m11()), float(m.m12()), float(m.m21()), float(m.m22()), float(m.dx()), float(m.dy())};
    FPDFPageObj_SetMatrix(obj, &fm);
    // Insert where the replaced text was, so the content (reading) order is preserved.
    if (insertIndex && *insertIndex >= 0 && FPDFPage_InsertObjectAtIndex(page, obj, size_t(*insertIndex)))
        ++*insertIndex;
    else
        FPDFPage_InsertObject(page, obj);
    return obj;
}

} // namespace

QString sanitizeText(const QString& text)
{
    QString s = text;
    s.replace(QChar(0x00A0), QLatin1Char(' '));
    s.replace(QChar(0x202F), QLatin1Char(' '));
    s.replace(QLatin1Char('\t'), QLatin1Char(' '));
    s.remove(QChar(0x00AD));
    s.remove(QChar(0x200B));
    s.remove(QChar(0xFEFF));
    s.remove(QChar(0x2029));
    return s;
}

QString compact(const QString& text)
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    QString s = text;
    s.remove(ws);
    return s;
}

bool WriterFont::supports(const QString& text) const
{
    if (original) {
        for (char32_t c : codePoints(text)) {
            if (c == U' ')
                continue;
            if (!allowed.contains(c))
                return false;
        }
        return true;
    }
    const QRawFont& raw = rawFont(filePath);
    if (!raw.isValid())
        return false;
    for (char32_t c : codePoints(text))
        if (c != U' ' && !raw.supportsCharacter(c))
            return false;
    return true;
}

double WriterFont::advance(const QString& text, double size) const
{
    if (original) {
        double w = 0;
        for (char32_t c : codePoints(text)) {
            float gw = 0;
            if (c == U' ' && !hasSpace)
                w += 0.27 * size;
            else if (FPDFFont_GetGlyphWidth(handle, uint32_t(c), float(size), &gw))
                w += gw;
            else
                w += 0.5 * size;
        }
        return w;
    }
    const QRawFont& raw = rawFont(filePath);
    if (!raw.isValid())
        return text.size() * size * 0.5;
    double w = 0;
    for (const QPointF& a : raw.advancesForGlyphIndexes(raw.glyphIndexesForString(text)))
        w += a.x();
    return w * size / 100.0;
}

FontProvider::FontProvider(Doc doc, QSet<char32_t> charset) : m_doc(doc), m_charset(std::move(charset)) {}

FontProvider::~FontProvider()
{
    for (Obj o : m_keepAlive)
        FPDFPageObj_Destroy(o);
    for (const auto& [path, f] : m_system)
        if (f->handle)
            FPDFFont_Close(f->handle);
}

WriterFont* FontProvider::system(const QString& filePath, QString* error)
{
    auto it = m_system.find(filePath);
    if (it != m_system.end())
        return it->second.get();
    const QByteArray data = fontFileData(filePath);
    const QRawFont& raw = rawFont(filePath);
    if (data.isEmpty() || !raw.isValid()) {
        if (error)
            *error = QObject::tr("The font file %1 cannot be read.").arg(filePath);
        return nullptr;
    }
    const QByteArray toUnicode = buildToUnicode(raw, m_charset);
    const QByteArray cidToGid = identityCidToGid(raw);
    Font handle = FPDFText_LoadCidType2Font(m_doc, reinterpret_cast<const uint8_t*>(data.constData()),
                                            uint32_t(data.size()), toUnicode.constData(),
                                            reinterpret_cast<const uint8_t*>(cidToGid.constData()),
                                            uint32_t(cidToGid.size()));
    if (!handle) {
        if (error)
            *error = QObject::tr("PDFium could not embed the font %1.").arg(filePath);
        return nullptr;
    }
    auto f = std::make_unique<WriterFont>();
    f->handle = handle;
    f->filePath = filePath;
    f->name = raw.familyName();
    WriterFont* ptr = f.get();
    m_system.emplace(filePath, std::move(f));
    return ptr;
}

WriterFont* FontProvider::original(const QString& pdfFontName, Font handle, const QSet<char32_t>& allowed)
{
    auto it = m_original.find(pdfFontName);
    if (it != m_original.end())
        return it->second.get();
    if (!handle)
        return nullptr;
    // Text objects hold a reference to their font; this unattached object keeps the font alive.
    if (Obj keep = FPDFPageObj_CreateTextObj(m_doc, handle, 1.0f))
        m_keepAlive.push_back(keep);
    auto f = std::make_unique<WriterFont>();
    f->handle = handle;
    f->original = true;
    f->name = pdfFontName;
    f->allowed = allowed;
    f->hasSpace = allowed.contains(U' ');
    WriterFont* ptr = f.get();
    m_original.emplace(pdfFontName, std::move(f));
    return ptr;
}

QRectF objectBounds(Obj obj)
{
    float l = 0, b = 0, r = 0, t = 0;
    if (!obj || !FPDFPageObj_GetBounds(obj, &l, &b, &r, &t))
        return {};
    return QRectF(QPointF(l, b), QPointF(r, t)).normalized();
}

int indexOfObject(Page page, Obj obj)
{
    const int n = FPDFPage_CountObjects(page);
    for (int i = 0; i < n; ++i)
        if (FPDFPage_GetObject(page, i) == obj)
            return i;
    return -1;
}

PlacedLine placeLine(Doc doc, Page page, const WriterFont& font, double rawSize, const QString& text,
                     const QTransform& m, const QColor& color, int renderMode, int* insertIndex)
{
    PlacedLine line;
    line.expected = text;
    line.originalFont = font.original;
    auto add = [&](Obj o) {
        if (!o)
            return;
        line.objects << o;
        const QRectF b = objectBounds(o);
        if (!b.isNull())
            line.bounds = line.bounds.isNull() ? b : line.bounds.united(b);
    };
    if (font.hasSpace) {
        add(createObject(doc, page, font.handle, rawSize, text, m, color, renderMode, insertIndex));
        return line;
    }
    // No space glyph in this (subset) font: position words individually.
    double adv = 0;
    const QStringList words = text.split(QLatin1Char(' '));
    for (int i = 0; i < words.size(); ++i) {
        if (!words[i].isEmpty()) {
            const QTransform wm(m.m11(), m.m12(), m.m21(), m.m22(), m.dx() + adv * m.m11(), m.dy() + adv * m.m12());
            add(createObject(doc, page, font.handle, rawSize, words[i], wm, color, renderMode, insertIndex));
            adv += font.advance(words[i], rawSize);
        }
        if (i + 1 < words.size())
            adv += font.advance(QStringLiteral(" "), rawSize);
    }
    return line;
}

QList<int> verifyReadBack(Page page, const QList<PlacedLine>& lines, QStringList* details)
{
    QList<int> bad;
    if (lines.isEmpty())
        return bad;
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
    if (!tp) {
        for (int i = 0; i < lines.size(); ++i)
            bad << i;
        return bad;
    }
    for (int i = 0; i < lines.size(); ++i) {
        QString got;
        for (Obj o : lines[i].objects) {
            const unsigned long bytes = FPDFTextObj_GetText(o, tp, nullptr, 0);
            if (bytes > 2) {
                std::vector<unsigned short> buf(bytes / 2 + 1, 0);
                FPDFTextObj_GetText(o, tp, buf.data(), bytes);
                got += QString::fromUtf16(reinterpret_cast<const char16_t*>(buf.data()), int(bytes / 2) - 1);
            }
        }
        if (compact(got) != compact(lines[i].expected)) {
            bad << i;
            if (details)
                *details << QStringLiteral("expected \"%1\", read back \"%2\"").arg(lines[i].expected.left(60), got.left(60));
        }
    }
    FPDFText_ClosePage(tp);
    return bad;
}

} // namespace folvio::pdfedit
