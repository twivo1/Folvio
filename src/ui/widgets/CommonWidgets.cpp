#include "ui/widgets/CommonWidgets.h"

#include "ui/themes/Theme.h"
#include "ui/widgets/Icons.h"

#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

namespace folvio {

DropZone::DropZone(const QString& title, const QString& hint, QWidget* parent) : QFrame(parent)
{
    setAcceptDrops(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(180);
    auto* lay = new QVBoxLayout(this);
    lay->setAlignment(Qt::AlignCenter);
    lay->setSpacing(8);
    m_icon = new QLabel(this);
    m_icon->setAlignment(Qt::AlignCenter);
    m_title = new QLabel(title, this);
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setStyleSheet("font-size: 13pt; font-weight: 600;");
    m_hint = new QLabel(hint, this);
    m_hint->setAlignment(Qt::AlignCenter);
    m_hint->setProperty("muted", true);
    m_hint->setWordWrap(true);
    m_hint->setMinimumWidth(420);
    m_hint->setMaximumWidth(720);
    auto* choose = new QPushButton(tr("Choose files…"), this);
    choose->setProperty("primary", true);
    choose->setCursor(Qt::PointingHandCursor);
    connect(choose, &QPushButton::clicked, this, &DropZone::chooseFiles);
    lay->addWidget(m_icon);
    lay->addWidget(m_title);
    lay->addWidget(m_hint, 0, Qt::AlignHCenter);
    lay->addSpacing(4);
    lay->addWidget(choose, 0, Qt::AlignCenter);
    auto refresh = [this] { m_icon->setPixmap(Icons::pixmap("open", 40, Icons::Tone::Accent, devicePixelRatioF())); };
    refresh();
    connect(&Theme::instance(), &Theme::changed, this, refresh);
}

void DropZone::setCompact(bool compact)
{
    setMinimumHeight(compact ? 110 : 180);
    m_icon->setVisible(!compact);
}

void DropZone::chooseFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(window(), tr("Choose files"), QString(), m_filter);
    if (!files.isEmpty())
        Q_EMIT filesDropped(files);
}

void DropZone::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        chooseFiles();
}

void DropZone::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) {
        e->acceptProposedAction();
        m_hover = true;
        update();
    }
}

void DropZone::dragLeaveEvent(QDragLeaveEvent*)
{
    m_hover = false;
    update();
}

void DropZone::dropEvent(QDropEvent* e)
{
    m_hover = false;
    update();
    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls()) {
        if (!u.isLocalFile())
            continue;
        const QFileInfo fi(u.toLocalFile());
        if (fi.isDir()) {
            // A dropped folder contributes its files (non-recursive, safe default).
            for (const QFileInfo& f : QDir(fi.absoluteFilePath()).entryInfoList(QDir::Files))
                paths << f.absoluteFilePath();
        } else if (fi.isFile()) {
            paths << fi.absoluteFilePath();
        }
    }
    if (!paths.isEmpty()) {
        e->acceptProposedAction();
        Q_EMIT filesDropped(paths);
    }
}

void DropZone::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemeColors& c = tc();
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    p.setBrush(m_hover ? c.accentSoft : c.surface);
    QPen pen(m_hover ? c.accent : c.borderStrong, m_hover ? 1.6 : 1.2, Qt::DashLine);
    pen.setDashPattern({5, 4});
    p.setPen(pen);
    p.drawRoundedRect(r, 6, 6);
    if (hasFocus()) {
        p.setPen(QPen(c.accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(3, 3, -3, -3), 4, 4);
    }
}

// ---------------------------------------------------------------------------

Banner::Banner(QWidget* parent) : QFrame(parent)
{
    setObjectName("Banner");
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(10, 6, 6, 6);
    lay->setSpacing(8);
    m_icon = new QLabel(this);
    m_text = new QLabel(this);
    m_text->setWordWrap(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_actionsLayout = new QHBoxLayout;
    m_actionsLayout->setSpacing(6);
    auto* close = makeToolButton("close", tr("Dismiss"), this);
    close->setAutoRaise(true);
    connect(close, &QToolButton::clicked, this, [this] {
        hide();
        Q_EMIT dismissed();
    });
    lay->addWidget(m_icon, 0, Qt::AlignTop);
    lay->addWidget(m_text, 1);
    lay->addLayout(m_actionsLayout);
    lay->addWidget(close, 0, Qt::AlignTop);
    hide();
}

void Banner::show(Kind kind, const QString& text, const QStringList& actions)
{
    const char* k = kind == Kind::Info ? "info" : kind == Kind::Warning ? "warning" : kind == Kind::Error ? "error" : "success";
    setProperty("kind", k);
    style()->unpolish(this);
    style()->polish(this);
    const char* icon = kind == Kind::Info ? "info" : kind == Kind::Warning ? "warning" : kind == Kind::Error ? "warning" : "check";
    const Icons::Tone tone = kind == Kind::Error ? Icons::Tone::Danger
                             : kind == Kind::Warning ? Icons::Tone::Warning
                             : kind == Kind::Success ? Icons::Tone::Accent : Icons::Tone::Normal;
    m_icon->setPixmap(Icons::pixmap(icon, 18, tone, devicePixelRatioF()));
    m_text->setText(text);
    qDeleteAll(m_actionButtons);
    m_actionButtons.clear();
    for (int i = 0; i < actions.size(); ++i) {
        auto* b = new QPushButton(actions[i], this);
        connect(b, &QPushButton::clicked, this, [this, i] { Q_EMIT actionTriggered(i); });
        m_actionsLayout->addWidget(b);
        m_actionButtons << b;
    }
    QFrame::show();
}

void Banner::setDetails(const QString& text)
{
    m_text->setToolTip(text);
}

QFrame* makeSeparator(QWidget* parent)
{
    auto* f = new QFrame(parent);
    f->setProperty("separator", true);
    f->setFixedWidth(1);
    f->setFrameShape(QFrame::NoFrame);
    return f;
}

QToolButton* makeToolButton(const QString& icon, const QString& tooltip, QWidget* parent, bool checkable)
{
    auto* b = new QToolButton(parent);
    b->setIcon(Icons::get(icon));
    b->setToolTip(tooltip);
    b->setCheckable(checkable);
    b->setIconSize(QSize(18, 18));
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::TabFocus);
    return b;
}

QPushButton* makePrimaryButton(const QString& text, QWidget* parent)
{
    auto* b = new QPushButton(text, parent);
    b->setProperty("primary", true);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

QString humanSize(qint64 bytes)
{
    if (bytes < 1024)
        return QObject::tr("%1 B").arg(bytes);
    const char* units[] = {"KB", "MB", "GB", "TB"};
    double v = bytes;
    int u = -1;
    while (v >= 1024 && u < 3) {
        v /= 1024;
        ++u;
    }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', v < 10 ? 1 : 0).arg(QLatin1String(units[u]));
}

void revealInExplorer(const QString& path)
{
#ifdef Q_OS_WIN
    const QFileInfo fi(path);
    if (fi.isFile()) {
        // explorer.exe expects: /select,"C:\path\file". Windows paths cannot contain quotes,
        // so wrapping the native path in quotes is safe.
        QProcess p;
        p.setProgram(QStringLiteral("explorer.exe"));
        p.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(fi.absoluteFilePath())));
        p.startDetached();
        return;
    }
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath()));
}

} // namespace folvio
