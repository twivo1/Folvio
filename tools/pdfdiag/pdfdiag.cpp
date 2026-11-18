// Folvio PDF editing diagnostics (developer tool, not shipped).
//
//   pdfdiag dump     <file.pdf>            runs, lines and blocks per page
//   pdfdiag baseline <file.pdf>            rebuild each page with a no-op edit and verify
//   pdfdiag sweep    <file.pdf> <outdir>   edit every block once, verify each result,
//                                          store failing outputs in <outdir>
//   pdfdiag cidtest  <out.pdf>             CID font + caller ToUnicode experiment
//
// Prints structure and statistics; never writes next to the input file.
#include "core/conversions/ConversionRegistry.h"
#include "editor/pdf_native/PdfEditSession.h"
#include "editor/pdf_native/FontMatcher.h"
#include "engines/pdf/PdfDocument.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QPainter>
#include <QPdfWriter>
#include <QRawFont>
#include <QRegularExpression>

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cstdio>

using namespace folvio;

static QString norm(QString s)
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    return s.remove(ws);
}

static double pixelDiff(const QImage& a, const QImage& b, const QRect& ignore)
{
    if (a.size() != b.size())
        return 1.0;
    const QImage x = a.convertToFormat(QImage::Format_RGB32), y = b.convertToFormat(QImage::Format_RGB32);
    qint64 diff = 0, total = 0;
    for (int j = 0; j < x.height(); ++j)
        for (int i = 0; i < x.width(); ++i) {
            if (ignore.contains(i, j))
                continue;
            ++total;
            const QRgb p = x.pixel(i, j), q = y.pixel(i, j);
            if (std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q)) > 60)
                ++diff;
        }
    return total ? double(diff) / total : 0;
}

// Multiset of non-whitespace characters of a page (order independent, catches real loss).
static QString charBag(const QString& s)
{
    QString n = norm(s);
    std::sort(n.begin(), n.end());
    return n;
}

static void dump(const QString& path)
{
    PdfEditSession s;
    QString err;
    if (s.open(path, {}, &err) != PdfEditSession::OpenStatus::Ok) {
        std::printf("open failed: %s\n", qPrintable(err));
        return;
    }
    for (int p = 0; p < s.pageCount(); ++p) {
        const auto runs = s.original()->textRuns(p);
        std::printf("== page %d: %d text objects\n", p + 1, int(runs.size()));
        for (const PdfTextRun& r : runs)
            std::printf("  run obj=%d font=%s size=%.1f x=%.1f y=%.1f w=%.1f space=%d len=%d '%s'\n", r.objectIndex,
                        qPrintable(r.fontName), r.effectiveSize, r.matrix.dx(), r.matrix.dy(), r.bounds.width(),
                        int(r.spaceAfter), int(r.text.size()), qPrintable(r.text.left(50)));
        for (const TextBlock& b : s.blocks(p)) {
            std::printf("  BLOCK %d lines=%d editable=%d mixed=%d x=%.1f..%.1f\n", b.id, b.lineCount(), int(b.editable),
                        int(b.mixedStyle), b.bounds.left(), b.bounds.right());
            for (const TextLine& l : b.lines)
                std::printf("     | runs=%d '%s'\n", int(l.runs.size()), qPrintable(l.text.left(90)));
        }
    }
}

static void baseline(const QString& path)
{
    PdfEditSession s;
    s.open(path);
    for (int p = 0; p < s.pageCount(); ++p) {
        if (s.blocks(p).isEmpty())
            continue;
        const TextBlock& b = s.blocks(p).first();
        EditOp op;
        op.page = p;
        op.blockId = b.id;
        op.text = s.blockState(p, b.id).text; // identical text: nothing should change
        QString err;
        if (!s.apply(op, &err)) {
            std::printf("page %d: apply failed: %s\n", p + 1, qPrintable(err));
            continue;
        }
        QByteArray out;
        s.serialize(&out, nullptr);
        auto doc = PdfDocument::openData(out);
        const QString before = s.original()->pageText(p), after = doc->pageText(p);
        const double d = pixelDiff(s.original()->render(p, 1.5), doc->render(p, 1.5), {});
        std::printf("page %d: text identical=%d, pixel diff=%.5f\n", p + 1, int(before == after), d);
        std::printf("  verifyOutput: %s %s\n", s.verifyOutput(out, &err) ? "OK" : "FAIL", qPrintable(err));
        s.undo();
    }
}

static int sweep(const QString& path, const QString& outDir)
{
    QDir().mkpath(outDir);
    int failures = 0, refused = 0, total = 0;
    PdfEditSession probe;
    probe.open(path);
    for (int p = 0; p < probe.pageCount(); ++p) {
        const QString originalText = probe.original()->pageText(p);
        for (const TextBlock& b : probe.blocks(p)) {
            if (!b.editable)
                continue;
            ++total;
            PdfEditSession s;
            s.open(path);
            const BlockState st = s.blockState(p, b.id);
            // Realistic edit: remove the last word of the first line (shorter, never needs more space).
            QStringList lines = st.text.split('\n');
            QString first = lines[0];
            const int cut = first.lastIndexOf(' ');
            const QString removed = cut > 0 ? first.mid(cut) : QString();
            if (cut > 0)
                first = first.left(cut);
            lines[0] = first;
            EditOp op;
            op.page = p;
            op.blockId = b.id;
            op.text = lines.join('\n');
            QString err;
            if (!s.apply(op, &err)) {
                ++refused;
                std::printf("block %3d refused: %s\n", b.id, qPrintable(err.left(110)));
                continue;
            }
            QByteArray out;
            s.serialize(&out, nullptr);
            const bool ok = s.verifyOutput(out, &err);
            auto doc = PdfDocument::openData(out);
            // Independent loss check: page characters minus the removed word must equal the output's.
            QString expectedBag = charBag(originalText);
            for (QChar c : norm(removed)) {
                const int i = expectedBag.indexOf(c);
                if (i >= 0)
                    expectedBag.remove(i, 1);
            }
            const bool charsOk = charBag(doc->pageText(p)) == expectedBag;
            const double scale = 1.5;
            const QImage before = s.original()->render(p, scale);
            const QTransform t = s.original()->pageToDevice(p, before.size());
            const QRect ignore = t.mapRect(b.lines.first().bounds.adjusted(-2, -3, 2, 3)).toAlignedRect();
            const double d = pixelDiff(before, doc->render(p, scale), ignore);
            const bool fail = !ok || !charsOk || d > 0.0002;
            if (fail) {
                ++failures;
                QFile f(QStringLiteral("%1/p%2-b%3.pdf").arg(outDir).arg(p + 1).arg(b.id));
                f.open(QIODevice::WriteOnly);
                f.write(out);
            }
            std::printf("block %3d verify=%s chars=%s pixel-outside-line=%.5f %s\n", b.id, ok ? "OK  " : "FAIL",
                        charsOk ? "OK  " : "LOST", d, fail ? qPrintable("<< " + err.left(100)) : "");
        }
    }
    std::printf("SWEEP: %d blocks, %d failures, %d refused\n", total, failures, refused);
    return failures ? 1 : 0;
}

static void cidtest(const QString& outPath)
{
    auto doc = PdfDocument::createEmpty();
    std::lock_guard lock(pdfiumMutex());
    FPDF_PAGE page = FPDFPage_New(doc->handle(), 0, 595, 842);
    const QString fontPath = folvio::FontMatcher::byFamily(QStringLiteral("Arial"), false, false).filePath;
    QFile ff(fontPath);
    ff.open(QIODevice::ReadOnly);
    const QByteArray data = ff.readAll();
    QRawFont raw(fontPath, 100.0);
    const QByteArray maxp = raw.fontTable("maxp");
    const int numGlyphs = (uchar(maxp[4]) << 8) | uchar(maxp[5]);
    QByteArray cidToGid;
    for (int g = 0; g < numGlyphs; ++g) {
        cidToGid.append(char(g >> 8));
        cidToGid.append(char(g & 0xFF));
    }
    const QString text = QString::fromUtf8("Zażółć-gęślą jaźń 1000 – x");
    QMap<quint32, QChar> m;
    for (QChar c : text) {
        const auto g = raw.glyphIndexesForString(QString(c));
        if (!g.isEmpty() && !m.contains(g[0]))
            m.insert(g[0], c);
    }
    QString cmap = QStringLiteral("/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                                  "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                                  "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n"
                                  "<0000> <FFFF>\nendcodespacerange\n");
    cmap += QString::number(m.size()) + QStringLiteral(" beginbfchar\n");
    for (auto it = m.begin(); it != m.end(); ++it)
        cmap += QString::asprintf("<%04X> <%04X>\n", it.key(), it.value().unicode());
    cmap += QStringLiteral("endbfchar\nendcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n");
    const QByteArray cm = cmap.toLatin1();
    FPDF_FONT font = FPDFText_LoadCidType2Font(doc->handle(), reinterpret_cast<const uint8_t*>(data.constData()),
                                               uint32_t(data.size()), cm.constData(),
                                               reinterpret_cast<const uint8_t*>(cidToGid.constData()), uint32_t(cidToGid.size()));
    std::printf("font loaded: %d (glyphs %d)\n", font != nullptr, numGlyphs);
    FPDF_PAGEOBJECT o = FPDFPageObj_CreateTextObj(doc->handle(), font, 20);
    std::u16string u = text.toStdU16String();
    FPDFText_SetText(o, reinterpret_cast<FPDF_WIDESTRING>(u.c_str()));
    FPDFPageObj_Transform(o, 1, 0, 0, 1, 50, 700);
    FPDFPage_InsertObject(page, o);
    FPDFPage_GenerateContent(page);
    float l = 0, b = 0, r = 0, t = 0;
    FPDFPageObj_GetBounds(o, &l, &b, &r, &t);
    double w = 0;
    for (const QPointF& a : raw.advancesForGlyphIndexes(raw.glyphIndexesForString(text)))
        w += a.x();
    std::printf("bounds width %.1f, QRawFont advance width %.1f\n", r - l, w * 20 / 100);
    float gw = 0;
    FPDFFont_GetGlyphWidth(font, 'a', 20, &gw);
    std::printf("GetGlyphWidth('a', 20) = %.2f (QRawFont %.2f)\n", gw,
                raw.advancesForGlyphIndexes(raw.glyphIndexesForString("a")).value(0).x() * 0.2);
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
    unsigned short buf[256] = {0};
    const unsigned long n = FPDFTextObj_GetText(o, tp, buf, sizeof(buf));
    const QString back = QString::fromUtf16(reinterpret_cast<const char16_t*>(buf), int(n / 2) - 1);
    std::printf("readback match: %d\n", back == text);
    FPDFText_ClosePage(tp);
    FPDF_ClosePage(page);
    QByteArray out;
    doc->save(&out);
    QFile of(outPath);
    of.open(QIODevice::WriteOnly);
    of.write(out);
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    ConversionRegistry::registerBuiltins();
    if (argc < 3) {
        std::printf("usage: pdfdiag dump|baseline|sweep|cidtest file [outdir]\n");
        return 2;
    }
    const QString cmd = QString::fromLocal8Bit(argv[1]);
    const QString file = QString::fromLocal8Bit(argv[2]);
    if (cmd == "dump")
        dump(file);
    else if (cmd == "baseline")
        baseline(file);
    else if (cmd == "cidtest")
        cidtest(file);
    else if (cmd == "compare" && argc >= 5) {
        // pdfdiag compare before.pdf after.pdf out.png [y0 y1]  (fractions of page height)
        auto a = PdfDocument::openFile(file), b = PdfDocument::openFile(QString::fromLocal8Bit(argv[3]));
        const QImage ia = a->render(0, 2.5), ib = b->render(0, 2.5);
        const double y0 = argc > 5 ? atof(argv[5]) : 0.0, y1 = argc > 6 ? atof(argv[6]) : 1.0;
        const QRect crop(0, int(ia.height() * y0), ia.width(), int(ia.height() * (y1 - y0)));
        QImage out(crop.width(), crop.height() * 2 + 8, QImage::Format_RGB32);
        out.fill(Qt::red);
        QPainter p(&out);
        p.drawImage(QPoint(0, 0), ia.copy(crop));
        p.drawImage(QPoint(0, crop.height() + 8), ib.copy(crop));
        p.end();
        out.save(QString::fromLocal8Bit(argv[4]));
    }
    else if (cmd == "edit" && argc >= 7) {
        // pdfdiag edit file.pdf "<text in block>" "<old>" "<new>" out.pdf
        PdfEditSession s;
        s.open(file);
        const QString find = QString::fromUtf8(argv[3]);
        for (int p = 0; p < s.pageCount(); ++p)
            for (const TextBlock& b : s.blocks(p)) {
                const BlockState st = s.blockState(p, b.id);
                if (!st.text.contains(find))
                    continue;
                EditOp op;
                op.page = p;
                op.blockId = b.id;
                op.text = QString(st.text).replace(QString::fromUtf8(argv[4]), QString::fromUtf8(argv[5]));
                QString err;
                if (!s.apply(op, &err)) {
                    std::printf("apply failed: %s\n", qPrintable(err));
                    return 1;
                }
                for (const QString& w : s.lastWarnings())
                    std::printf("warning: %s\n", qPrintable(w));
                QByteArray out;
                s.serialize(&out, nullptr);
                std::printf("verify: %s %s\n", s.verifyOutput(out, &err) ? "OK" : "FAIL", qPrintable(err));
                QFile f(QString::fromLocal8Bit(argv[6]));
                f.open(QIODevice::WriteOnly);
                f.write(out);
                auto doc = PdfDocument::openData(out);
                const QString t = doc->pageText(p);
                const int i = t.indexOf("Structured");
                QString cps;
                for (QChar c : t.mid(i, 80))
                    cps += c.unicode() == 0xAD ? QStringLiteral("<SHY>") : c.unicode() == 0xA0 ? QStringLiteral("<NBSP>") : QString(c);
                std::printf("extracted: %s\n", cps.toUtf8().constData());
                return 0;
            }
        std::printf("block not found\n");
        return 1;
    }
    else if (cmd == "mktext" && argc >= 4) {
        // pdfdiag mktext out.pdf "line 1" ["line 2" ...]  (QPdfWriter, Arial 14 pt, like the tests)
        QPdfWriter w(file);
        w.setPageSize(QPageSize(QPageSize::A4));
        w.setResolution(72);
        w.setPageMargins(QMarginsF(0, 0, 0, 0));
        QPainter p(&w);
        QFont f(QStringLiteral("Arial"));
        f.setPointSizeF(14);
        p.setFont(f);
        for (int i = 3; i < argc; ++i)
            p.drawText(72, 100 + 28 * (i - 3), QString::fromUtf8(argv[i]));
        return p.end() ? 0 : 1;
    }
    else if (cmd == "sweep")
        return sweep(file, argc > 3 ? QString::fromLocal8Bit(argv[3]) : QDir::tempPath() + "/pdfdiag");
    return 0;
}
