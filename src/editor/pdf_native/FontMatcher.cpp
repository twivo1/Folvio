#include "editor/pdf_native/FontMatcher.h"

#include "core/files/PathUtils.h"

#include <QDir>
#include <QDirIterator>
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

#ifdef Q_OS_WIN
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
const std::pair<const char*, const char*> kSubstitutes[] = {{"Helvetica", "Arial"}, {"Cambria", "Georgia"}};
#else
// Common free fonts on Linux (Liberation, Carlito and Caladea are metric-compatible with
// Arial/Times New Roman/Courier New, Calibri and Cambria). Folvio's packages ship Liberation,
// Carlito and DejaVu so text edits keep their line lengths on every distribution.
const FamilyFiles kFamilies[] = {
    {"Liberation Sans", "LiberationSans-Regular.ttf", "LiberationSans-Bold.ttf", "LiberationSans-Italic.ttf", "LiberationSans-BoldItalic.ttf"},
    {"Liberation Serif", "LiberationSerif-Regular.ttf", "LiberationSerif-Bold.ttf", "LiberationSerif-Italic.ttf", "LiberationSerif-BoldItalic.ttf"},
    {"Liberation Mono", "LiberationMono-Regular.ttf", "LiberationMono-Bold.ttf", "LiberationMono-Italic.ttf", "LiberationMono-BoldItalic.ttf"},
    {"Carlito", "Carlito-Regular.ttf", "Carlito-Bold.ttf", "Carlito-Italic.ttf", "Carlito-BoldItalic.ttf"},
    {"Caladea", "Caladea-Regular.ttf", "Caladea-Bold.ttf", "Caladea-Italic.ttf", "Caladea-BoldItalic.ttf"},
    {"DejaVu Sans", "DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSans-Oblique.ttf", "DejaVuSans-BoldOblique.ttf"},
    {"DejaVu Serif", "DejaVuSerif.ttf", "DejaVuSerif-Bold.ttf", "DejaVuSerif-Italic.ttf", "DejaVuSerif-BoldItalic.ttf"},
    {"DejaVu Sans Mono", "DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf", "DejaVuSansMono-Oblique.ttf", "DejaVuSansMono-BoldOblique.ttf"},
    {"Noto Sans", "NotoSans-Regular.ttf", "NotoSans-Bold.ttf", "NotoSans-Italic.ttf", "NotoSans-BoldItalic.ttf"},
    {"Noto Serif", "NotoSerif-Regular.ttf", "NotoSerif-Bold.ttf", "NotoSerif-Italic.ttf", "NotoSerif-BoldItalic.ttf"},
};
// Windows family names (used by PDFs and by the rest of Folvio) -> installed substitute.
const std::pair<const char*, const char*> kSubstitutes[] = {
    {"Arial", "Liberation Sans"}, {"Helvetica", "Liberation Sans"}, {"Times New Roman", "Liberation Serif"},
    {"Courier New", "Liberation Mono"}, {"Calibri", "Carlito"}, {"Cambria", "Caladea"}, {"Georgia", "DejaVu Serif"},
    {"Garamond", "Liberation Serif"}, {"Book Antiqua", "Liberation Serif"}, {"Verdana", "DejaVu Sans"},
    {"Tahoma", "DejaVu Sans"}, {"Segoe UI", "DejaVu Sans"}, {"Trebuchet MS", "DejaVu Sans"},
    {"Century Gothic", "DejaVu Sans"}, {"Aptos", "DejaVu Sans"}, {"Consolas", "DejaVu Sans Mono"},
    // Second choices when the metric-compatible font is missing.
    {"Liberation Sans", "DejaVu Sans"}, {"Liberation Serif", "DejaVu Serif"}, {"Liberation Mono", "DejaVu Sans Mono"},
    {"Carlito", "Liberation Sans"}, {"Caladea", "Liberation Serif"}, {"DejaVu Sans", "Noto Sans"},
    {"DejaVu Serif", "Noto Serif"},
};
#endif

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

// File name (lower case) -> path, built once from all font directories (recursively on Linux,
// where fonts live in per-package subdirectories).
const QHash<QString, QString>& fontIndex()
{
    static const QHash<QString, QString> index = [] {
        QHash<QString, QString> h;
        for (const QString& dir : FontMatcher::fontDirectories()) {
            QDirIterator it(dir, {QStringLiteral("*.ttf"), QStringLiteral("*.TTF"), QStringLiteral("*.otf")}, QDir::Files,
                            QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
            while (it.hasNext()) {
                const QString path = it.next();
                const QString key = QFileInfo(path).fileName().toLower();
                if (!h.contains(key)) // earlier directories win (bundled before system)
                    h.insert(key, path);
            }
        }
        return h;
    }();
    return index;
}

QString findFile(const QString& name)
{
    return fontIndex().value(name.toLower());
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
#ifdef Q_OS_WIN
    const QString windir = qEnvironmentVariable("WINDIR", "C:/Windows");
    dirs << QDir::fromNativeSeparators(windir) + "/Fonts";
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty())
        dirs << QDir::fromNativeSeparators(local) + "/Microsoft/Windows/Fonts";
#else
    // Fonts bundled with the package first, so results do not depend on what the distribution installed.
    dirs << QDir::cleanPath(PathUtils::resourceDir() + "/../fonts");
    const QString home = QDir::homePath();
    dirs << home + "/.local/share/fonts" << home + "/.fonts" << "/usr/local/share/fonts" << "/usr/share/fonts";
#endif
    return dirs;
}

FontMatch FontMatcher::byFamily(const QString& family, bool bold, bool italic)
{
    FontMatch m;
    const FamilyFiles* f = familyEntry(family);
    if (!f || findFile(QString::fromLatin1(f->regular)).isEmpty()) {
        // Not installed here: use the substitute (Linux maps Windows families to free equivalents).
        for (const auto& [from, to] : kSubstitutes)
            if (family.compare(QLatin1String(from), Qt::CaseInsensitive) == 0)
                return byFamily(QString::fromLatin1(to), bold, italic);
        if (!f)
            return m;
    }
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
