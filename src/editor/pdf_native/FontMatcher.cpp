#include "editor/pdf_native/FontMatcher.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

namespace folvio {

namespace {

struct FamilyFiles {
    const char* family;
    const char* regular;
    const char* bold;
    const char* italic;
    const char* boldItalic;
};

// Fonts shipped with Windows 10/11. Only .ttf files (PDFium cannot embed TTC collections).
const FamilyFiles kFamilies[] = {
    {"Arial", "arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf"},
    {"Times New Roman", "times.ttf", "timesbd.ttf", "timesi.ttf", "timesbi.ttf"},
    {"Calibri", "calibri.ttf", "calibrib.ttf", "calibrii.ttf", "calibriz.ttf"},
    {"Courier New", "cour.ttf", "courbd.ttf", "couri.ttf", "courbi.ttf"},
    {"Verdana", "verdana.ttf", "verdanab.ttf", "verdanai.ttf", "verdanaz.ttf"},
    {"Georgia", "georgia.ttf", "georgiab.ttf", "georgiai.ttf", "georgiaz.ttf"},
    {"Tahoma", "tahoma.ttf", "tahomabd.ttf", "tahoma.ttf", "tahomabd.ttf"},
    {"Segoe UI", "segoeui.ttf", "segoeuib.ttf", "segoeuii.ttf", "segoeuiz.ttf"},
    {"Trebuchet MS", "trebuc.ttf", "trebucbd.ttf", "trebucit.ttf", "trebucbi.ttf"},
    {"Consolas", "consola.ttf", "consolab.ttf", "consolai.ttf", "consolaz.ttf"},
    {"Garamond", "GARA.TTF", "GARABD.TTF", "GARAIT.TTF", "GARABD.TTF"},
    {"Book Antiqua", "BKANT.TTF", "ANTQUAB.TTF", "ANTQUAI.TTF", "ANTQUABI.TTF"},
    {"Century Gothic", "GOTHIC.TTF", "GOTHICB.TTF", "GOTHICI.TTF", "GOTHICBI.TTF"},
    {"Aptos", "Aptos.ttf", "Aptos-Bold.ttf", "Aptos-Italic.ttf", "Aptos-Bold-Italic.ttf"},
};

// Lower-case name fragments -> family. Order matters (more specific first).
const std::pair<const char*, const char*> kAliases[] = {
    {"timesnewroman", "Times New Roman"}, {"times", "Times New Roman"}, {"liberationserif", "Times New Roman"},
    {"tinos", "Times New Roman"}, {"nimbusroman", "Times New Roman"}, {"cambria", "Georgia"},
    {"georgia", "Georgia"}, {"garamond", "Garamond"}, {"bookantiqua", "Book Antiqua"}, {"palatino", "Book Antiqua"},
    {"calibri", "Calibri"}, {"carlito", "Calibri"}, {"aptos", "Aptos"},
    {"liberationmono", "Courier New"}, {"arial", "Arial"}, {"helvetica", "Arial"}, {"liberationsans", "Arial"}, {"arimo", "Arial"},
    {"nimbussans", "Arial"}, {"dejavusansmono", "Consolas"}, {"dejavusans", "Verdana"}, {"verdana", "Verdana"}, {"tahoma", "Tahoma"},
    {"segoe", "Segoe UI"}, {"trebuchet", "Trebuchet MS"}, {"centurygothic", "Century Gothic"},
    {"consolas", "Consolas"}, {"couriernew", "Courier New"}, {"courier", "Courier New"},
    {"liberationmono", "Courier New"}, {"cousine", "Courier New"},
};

QString findFile(const QString& name)
{
    for (const QString& dir : FontMatcher::fontDirectories()) {
        const QString p = dir + "/" + name;
        if (QFileInfo::exists(p))
            return p;
        // File systems are case-insensitive on Windows, but be explicit elsewhere.
        const QString lower = dir + "/" + name.toLower();
        if (QFileInfo::exists(lower))
            return lower;
    }
    return {};
}

const FamilyFiles* familyEntry(const QString& family)
{
    for (const auto& f : kFamilies)
        if (family.compare(QLatin1String(f.family), Qt::CaseInsensitive) == 0)
            return &f;
    return nullptr;
}

} // namespace

QStringList FontMatcher::fontDirectories()
{
    QStringList dirs;
    const QString windir = qEnvironmentVariable("WINDIR", "C:/Windows");
    dirs << QDir::fromNativeSeparators(windir) + "/Fonts";
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty())
        dirs << QDir::fromNativeSeparators(local) + "/Microsoft/Windows/Fonts";
    return dirs;
}

FontMatch FontMatcher::byFamily(const QString& family, bool bold, bool italic)
{
    FontMatch m;
    const FamilyFiles* f = familyEntry(family);
    if (!f)
        return m;
    const char* file = bold && italic ? f->boldItalic : bold ? f->bold : italic ? f->italic : f->regular;
    m.filePath = findFile(QString::fromLatin1(file));
    if (m.filePath.isEmpty()) // fall back to regular weight of the same family
        m.filePath = findFile(QString::fromLatin1(f->regular));
    if (m.filePath.isEmpty())
        return m;
    m.family = QString::fromLatin1(f->family);
    m.bold = bold;
    m.italic = italic;
    return m;
}

FontMatch FontMatcher::match(const QString& pdfFontName, bool bold, bool italic, bool serif, bool monospace)
{
    QString key = pdfFontName.toLower();
    key.remove(QRegularExpression(QStringLiteral("^[a-z]{6}\\+")));
    key.remove(QRegularExpression(QStringLiteral("[^a-z]")));
    for (const auto& [frag, family] : kAliases) {
        if (key.contains(QLatin1String(frag))) {
            FontMatch m = byFamily(QString::fromLatin1(family), bold, italic);
            if (m.isValid()) {
                // "exact" only when the family name itself appears in the PDF font name.
                QString fam = m.family.toLower();
                fam.remove(' ');
                m.exact = key.contains(fam);
                return m;
            }
        }
    }
    FontMatch m = byFamily(monospace ? "Courier New" : serif ? "Times New Roman" : "Arial", bold, italic);
    if (!m.isValid())
        m = byFamily("Segoe UI", bold, italic);
    return m;
}

QStringList FontMatcher::availableFamilies()
{
    QStringList list;
    for (const auto& f : kFamilies)
        if (!findFile(QString::fromLatin1(f.regular)).isEmpty())
            list << QString::fromLatin1(f.family);
    return list;
}

} // namespace folvio
