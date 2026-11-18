#include "editor/pdf_native/PdfTextWriter.h"

#include <QFile>
#include <QMap>
#include <QMutex>
#include <QRawFont>
#include <QRegularExpression>

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <tuple>

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


// ---- unique PostScript name ---------------------------------------------------------
// PDFium's content generator shares one /Font resource between fonts with the same
// (BaseFont, Subtype). A system font embedded by Folvio gets its BaseFont from the font's
// PostScript name; if the PDF already embeds a subset of that very font without a subset
// tag (e.g. "LiberationSans" written by Qt on Linux), the untouched text would be switched to
// our font and become garbage. The copy we embed therefore gets a subset-style tag
// ("FVABCD+LiberationSans") by rewriting the 'name' table (appended as a new table; the old
// one stays unreferenced). Returns the input unchanged if the font cannot be parsed.

quint32 be32(const QByteArray& d, int o)
{
    const auto* u = reinterpret_cast<const uchar*>(d.constData());
    return (quint32(u[o]) << 24) | (quint32(u[o + 1]) << 16) | (quint32(u[o + 2]) << 8) | quint32(u[o + 3]);
}
quint16 be16(const QByteArray& d, int o)
{
    const auto* u = reinterpret_cast<const uchar*>(d.constData());
    return quint16((u[o] << 8) | u[o + 1]);
}
void put16(QByteArray& d, quint16 v)
{
    d.append(char(v >> 8));
    d.append(char(v & 0xff));
}
void set32(QByteArray& d, int o, quint32 v)
{
    d[o] = char(v >> 24);
    d[o + 1] = char((v >> 16) & 0xff);
    d[o + 2] = char((v >> 8) & 0xff);
    d[o + 3] = char(v & 0xff);
}

// Deterministic 4-letter tag per font file: "FV" + 4 letters.
QByteArray subsetTag(const QString& path)
{
    quint32 h = 2166136261u;
    for (QChar c : path) {
        h ^= c.unicode();
        h *= 16777619u;
    }
    QByteArray tag("FV");
    for (int i = 0; i < 4; ++i) {
        tag.append(char('A' + h % 26));
        h /= 26;
    }
    return tag;
}

const QRawFont& rawFont(const QString& path)
{
    // QRawFont objects must not be shared between threads (GUI editing vs. export worker).
    thread_local QHash<QString, QRawFont> cache;
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

QString fontPostScriptName(const QByteArray& font)
{
    // Reuse the rewriting code: tag with an empty marker and read the name back is overkill;
    // parse directly instead.
    if (font.size() < 12)
        return {};
    const int numTables = be16(font, 4);
    for (int i = 0; i < numTables; ++i) {
        const int r = 12 + 16 * i;
        if (r + 16 > font.size() || font.mid(r, 4) != "name")
            continue;
        const int off = int(be32(font, r + 8)), len = int(be32(font, r + 12));
        if (off < 0 || len < 6 || off + len > font.size())
            return {};
        const QByteArray name = font.mid(off, len);
        const int count = be16(name, 2), storage = be16(name, 4);
        QString mac;
        for (int k = 0; k < count && 6 + 12 * k + 12 <= name.size(); ++k) {
            const int rec = 6 + 12 * k;
            const int platform = be16(name, rec), nameId = be16(name, rec + 6);
            const int l = be16(name, rec + 8), o = be16(name, rec + 10);
            if (nameId != 6 || storage + o + l > name.size())
                continue;
            const QByteArray text = name.mid(storage + o, l);
            if (platform == 3 || platform == 0) {
                QString out;
                for (int j = 0; j + 1 < text.size(); j += 2)
                    out += QChar(be16(text, j));
                return out;
            }
            if (mac.isEmpty())
                mac = QString::fromLatin1(text);
        }
        return mac;
    }
    return {};
}

QByteArray withTaggedPostScriptName(const QByteArray& font, const QByteArray& tag)
{
    if (font.size() < 12)
        return font;
    const quint32 version = be32(font, 0);
    if (version != 0x00010000 && version != 0x4F54544F /*OTTO*/ && version != 0x74727565 /*true*/)
        return font;
    const int numTables = be16(font, 4);
    int nameRecord = -1;
    for (int i = 0; i < numTables; ++i) {
        const int r = 12 + 16 * i;
        if (r + 16 > font.size())
            return font;
        if (font.mid(r, 4) == "name")
            nameRecord = r;
    }
    if (nameRecord < 0)
        return font;
    const int off = int(be32(font, nameRecord + 8));
    const int len = int(be32(font, nameRecord + 12));
    if (off < 0 || len < 6 || off + len > font.size())
        return font;
    const QByteArray name = font.mid(off, len);
    const int count = be16(name, 2);
    const int storage = be16(name, 4);
    if (6 + 12 * count > name.size())
        return font;

    struct Rec {
        quint16 platform, encoding, language, nameId;
        QByteArray text;
    };
    QList<Rec> recs;
    QString psName;
    for (int i = 0; i < count; ++i) {
        const int r = 6 + 12 * i;
        Rec rec{be16(name, r), be16(name, r + 2), be16(name, r + 4), be16(name, r + 6), {}};
        const int l = be16(name, r + 8), o = be16(name, r + 10);
        if (storage + o + l > name.size())
            return font;
        rec.text = name.mid(storage + o, l);
        if (rec.language >= 0x8000)
            return font; // language-tag records (format 1) are not rewritten
        if (rec.nameId == 6) {
            if (psName.isEmpty()) {
                if (rec.platform == 3 || rec.platform == 0) {
                    QString s;
                    for (int k = 0; k + 1 < rec.text.size(); k += 2)
                        s += QChar(be16(rec.text, k));
                    psName = s;
                } else {
                    psName = QString::fromLatin1(rec.text);
                }
            }
            continue;
        }
        recs << rec;
    }
    if (psName.isEmpty())
        return font;
    const QString tagged = QString::fromLatin1(tag) + QLatin1Char('+') + psName;
    QByteArray utf16;
    for (QChar c : tagged)
        put16(utf16, c.unicode());
    recs << Rec{3, 1, 0x409, 6, utf16} << Rec{1, 0, 0, 6, tagged.toLatin1()};
    std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) {
        return std::tie(a.platform, a.encoding, a.language, a.nameId) < std::tie(b.platform, b.encoding, b.language, b.nameId);
    });

    QByteArray table, strings;
    put16(table, 0);
    put16(table, quint16(recs.size()));
    put16(table, quint16(6 + 12 * recs.size()));
    for (const Rec& r : recs) {
        put16(table, r.platform);
        put16(table, r.encoding);
        put16(table, r.language);
        put16(table, r.nameId);
        put16(table, quint16(r.text.size()));
        put16(table, quint16(strings.size()));
        strings += r.text;
    }
    table += strings;
    const int tableLength = int(table.size());
    while (table.size() % 4)
        table.append('\0');

    QByteArray out = font;
    while (out.size() % 4)
        out.append('\0');
    const int newOffset = int(out.size());
    out += table;
    quint32 sum = 0;
    for (int i = 0; i < table.size(); i += 4)
        sum += be32(table, i);
    set32(out, nameRecord + 4, sum);
    set32(out, nameRecord + 8, quint32(newOffset));
    set32(out, nameRecord + 12, quint32(tableLength));
    return out;
}



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
    // Never share a name with a font the document already embeds (see withTaggedPostScriptName).
    const QByteArray tagged = withTaggedPostScriptName(data, subsetTag(filePath));
    Font handle = FPDFText_LoadCidType2Font(m_doc, reinterpret_cast<const uint8_t*>(tagged.constData()),
                                            uint32_t(tagged.size()), toUnicode.constData(),
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
