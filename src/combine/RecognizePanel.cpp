#include "combine/RecognizePanel.h"

#include "combine/AnnotationRender.h"
#include "combine/ImageGeometry.h"
#include "combine/PageCanvas.h"
#include "core/persistence/Database.h"
#include "engines/ocr/TesseractEngine.h"
#include "ui/themes/Theme.h"
#include "ui/widgets/CommonWidgets.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>

namespace folvio::combine {

namespace {

constexpr double kA4 = 595.0;

QLabel* mutedLabel(const QString& t, QWidget* parent)
{
    auto* l = new QLabel(t, parent);
    l->setWordWrap(true);
    l->setProperty("muted", true);
    return l;
}

QString modeKey(TextRecognition::Mode m)
{
    return m == TextRecognition::Mode::Printed ? "printed" : m == TextRecognition::Mode::Handwriting ? "handwriting" : "auto";
}

} // namespace

RecognizePanel::RecognizePanel(CombineDocument* doc, PageCanvas* canvas, Banner* banner, QWidget* parent)
    : QWidget(parent), m_doc(doc), m_canvas(canvas), m_banner(banner)
{
    setObjectName("recognize-panel");
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(8);
    auto* title = new QLabel(tr("Recognize Text"), this);
    title->setObjectName("SectionTitle");
    l->addWidget(title);
    l->addWidget(mutedLabel(tr("Drag a rectangle around the text. Recognition runs on this computer - nothing is uploaded."), this));

    m_mode = new QComboBox(this);
    m_mode->setObjectName("recognize-mode");
    m_mode->addItem(tr("Auto"), int(TextRecognition::Mode::Auto));
    m_mode->addItem(tr("Printed text"), int(TextRecognition::Mode::Printed));
    m_mode->addItem(tr("Handwriting (experimental)"), int(TextRecognition::Mode::Handwriting));
    const QString saved = Database::instance().setting("recognize.mode", "auto").toString();
    m_mode->setCurrentIndex(saved == "printed" ? 1 : saved == "handwriting" ? 2 : 0);
    auto* modeRow = new QHBoxLayout;
    modeRow->addWidget(new QLabel(tr("Text type"), this));
    modeRow->addWidget(m_mode, 1);
    l->addLayout(modeRow);

    // Handwriting module (optional download)
    m_moduleLabel = mutedLabel(QString(), this);
    m_moduleLabel->setObjectName("recognize-module");
    m_download = new QPushButton(this);
    m_download->setObjectName("recognize-download");
    m_remove = new QPushButton(tr("Remove handwriting models"), this);
    m_remove->setObjectName("recognize-remove");
    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(true);
    m_progress->hide();
    l->addWidget(m_moduleLabel);
    l->addWidget(m_download);
    l->addWidget(m_progress);
    l->addWidget(m_remove);

    m_status = mutedLabel(tr("No area selected yet."), this);
    m_status->setObjectName("recognize-status");
    l->addWidget(m_status);
    auto* resultTitle = new QLabel(tr("Recognized Text"), this);
    resultTitle->setObjectName("SectionTitle");
    l->addWidget(resultTitle);
    m_text = new QPlainTextEdit(this);
    m_text->setObjectName("recognize-text");
    m_text->setPlaceholderText(tr("The recognized text appears here. You can correct it before using it."));
    m_text->setMinimumHeight(140);
    l->addWidget(m_text, 1);

    m_copy = new QPushButton(tr("Copy"), this);
    m_insert = makePrimaryButton(tr("Insert as Text"), this);
    m_replace = new QPushButton(tr("Replace Selection…"), this);
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_copy->setObjectName("recognize-copy");
    m_insert->setObjectName("recognize-insert");
    m_replace->setObjectName("recognize-replace");
    m_cancel->setObjectName("recognize-cancel");
    m_insert->setToolTip(tr("Add the text as a text box below the selection; the image stays as it is."));
    m_replace->setToolTip(tr("Show the text instead of the selected area - previewed before anything changes. "
                             "The original photo is kept; Undo restores the area."));
    auto* row1 = new QHBoxLayout;
    row1->addWidget(m_copy);
    row1->addWidget(m_cancel);
    l->addLayout(row1);
    l->addWidget(m_insert);
    l->addWidget(m_replace);

    connect(m_mode, &QComboBox::currentIndexChanged, this, [this] {
        Database::instance().setSetting("recognize.mode", modeKey(mode()));
        updateModuleState();
        if (!m_area.isEmpty() && !isBusy())
            recognizeArea(m_area); // same area, other recogniser
    });
    connect(m_download, &QPushButton::clicked, this, [this] { ensureHandwritingModels(); });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Remove handwriting models"),
                                  tr("Remove the handwriting recognition models (%1 MB) from this computer? They can be "
                                     "downloaded again at any time.")
                                      .arg(HandwritingOcr::downloadSize() / (1024 * 1024)))
            != QMessageBox::Yes)
            return;
        QString err;
        if (!HandwritingOcr::removeModels(&err))
            m_banner->show(Banner::Kind::Error, err);
        updateModuleState();
    });
    connect(m_copy, &QPushButton::clicked, this, &RecognizePanel::copyText);
    connect(m_insert, &QPushButton::clicked, this, [this] { insertAsText(); });
    connect(m_replace, &QPushButton::clicked, this, &RecognizePanel::openReplaceDialog);
    connect(m_cancel, &QPushButton::clicked, this, &RecognizePanel::cancel);
    connect(m_text, &QPlainTextEdit::textChanged, this, [this] {
        const bool has = !m_text->toPlainText().trimmed().isEmpty() && !isBusy();
        m_copy->setEnabled(has);
        m_insert->setEnabled(has && !m_area.isEmpty());
        m_replace->setEnabled(has && !m_area.isEmpty());
    });
    Q_EMIT m_text->textChanged();
    updateModuleState();
}

RecognizePanel::~RecognizePanel()
{
    if (m_cancelFlag)
        m_cancelFlag->store(true);
    if (m_watcher)
        m_watcher->waitForFinished();
}

TextRecognition::Mode RecognizePanel::mode() const
{
    return TextRecognition::Mode(m_mode->currentData().toInt());
}

void RecognizePanel::setMode(TextRecognition::Mode m)
{
    m_mode->setCurrentIndex(m_mode->findData(int(m)));
}

bool RecognizePanel::isBusy() const
{
    return m_watcher && m_watcher->isRunning();
}

void RecognizePanel::updateModuleState()
{
    QString reason;
    const bool runtime = HandwritingOcr::runtimeAvailable(&reason);
    const bool models = HandwritingOcr::modelsInstalled();
    const bool downloading = m_downloader && m_downloader->isRunning();
    m_download->setText(downloading ? tr("Cancel download")
                                    : tr("Download handwriting models (%1 MB)…").arg(HandwritingOcr::downloadSize() / (1024 * 1024)));
    m_download->setVisible(runtime && (!models || downloading));
    m_remove->setVisible(runtime && models && !downloading);
    if (!runtime)
        m_moduleLabel->setText(tr("Handwriting recognition is not available: %1").arg(reason));
    else if (models)
        m_moduleLabel->setText(tr("Handwriting recognition: installed (experimental, offline). Results need checking."));
    else
        m_moduleLabel->setText(tr("Handwriting recognition needs an optional download. Printed text works without it."));
}

bool RecognizePanel::ensureHandwritingModels()
{
    if (HandwritingOcr::modelsInstalled())
        return true;
    if (m_downloader && m_downloader->isRunning()) {
        m_downloader->cancel();
        m_progress->hide();
        updateModuleState();
        return false;
    }
    QString reason;
    if (!HandwritingOcr::runtimeAvailable(&reason)) {
        m_banner->show(Banner::Kind::Error, reason);
        return false;
    }
    QMessageBox box(QMessageBox::Question, tr("Download handwriting recognition"),
                    tr("Folvio can read handwriting with an optional, experimental module (%1 MB).\n\n"
                       "Source: %2\n\n"
                       "Only the model files are downloaded, once. Your images and notes are never uploaded - "
                       "recognition runs on this computer. You can remove the module at any time.")
                        .arg(HandwritingOcr::downloadSize() / (1024 * 1024))
                        .arg(HandwritingOcr::sourceDescription()),
                    QMessageBox::Cancel, this);
    auto* ok = box.addButton(tr("Download"), QMessageBox::AcceptRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != ok)
        return false;
    m_downloader = new HandwritingModelDownloader(this);
    connect(m_downloader, &HandwritingModelDownloader::progress, this, [this](qint64 done, qint64 total) {
        m_progress->setRange(0, 1000);
        m_progress->setValue(total > 0 ? int(1000 * done / total) : 0);
        m_progress->setFormat(tr("%1 of %2 MB").arg(done / (1024 * 1024)).arg(total / (1024 * 1024)));
    });
    connect(m_downloader, &HandwritingModelDownloader::finished, this, [this](bool success, const QString& error) {
        m_progress->hide();
        m_downloader->deleteLater();
        updateModuleState();
        if (success) {
            m_banner->show(Banner::Kind::Success, tr("Handwriting recognition is ready."));
            if (!m_area.isEmpty())
                recognizeArea(m_area);
        } else if (!error.isEmpty()) {
            m_banner->show(Banner::Kind::Error, error);
        }
    });
    m_progress->setValue(0);
    m_progress->show();
    m_downloader->start();
    updateModuleState();
    return false;
}

const CombinePage* RecognizePanel::currentPage() const
{
    const int row = m_doc->indexOf(m_canvas->pageId());
    return row >= 0 ? &m_doc->page(row) : nullptr;
}

void RecognizePanel::recognizeArea(const QRectF& pageArea)
{
    const CombinePage* page = currentPage();
    if (!page || pageArea.isEmpty())
        return;
    if (isBusy()) {
        m_cancelFlag->store(true);
        m_watcher->waitForFinished();
    }
    m_area = pageArea;
    m_pageId = page->id;
    m_result = {};
    const TextRecognition::Mode m = mode();
    if (m == TextRecognition::Mode::Handwriting && !HandwritingOcr::modelsInstalled()) {
        m_status->setText(tr("Handwriting recognition needs the optional models - download them to continue."));
        ensureHandwritingModels();
        return;
    }
    m_status->setText(tr("Recognizing…"));
    m_text->setReadOnly(true);
    m_text->clear();
    m_cancelFlag = std::make_shared<std::atomic_bool>(false);
    if (!m_watcher) {
        m_watcher = new QFutureWatcher<RecognitionResult>(this);
        connect(m_watcher, &QFutureWatcherBase::finished, this, &RecognizePanel::onFinished);
    }
    const CombinePage copy = *page;
    const auto flag = m_cancelFlag;
    const QString langs = TesseractEngine::defaultLanguages();
    auto* areaImage = &m_areaImage;
    m_watcher->setFuture(QtConcurrent::run([copy, pageArea, m, langs, flag, areaImage]() {
        // Full resolution of the final page (perspective and crop applied), page units -> pixels.
        QString err;
        const QImage final = ImageProcessor::render(copy, ImageProcessor::Stage::Final, 0, &err);
        RecognitionResult r;
        if (final.isNull()) {
            r.error = err.isEmpty() ? QObject::tr("The page image could not be read.") : err;
            return r;
        }
        const double k = final.width();
        const QRect px = QRectF(pageArea.left() * k, pageArea.top() * k, pageArea.width() * k, pageArea.height() * k)
                             .toAlignedRect()
                             .intersected(final.rect());
        if (px.width() < 4 || px.height() < 4) {
            r.error = QObject::tr("The selected area is too small.");
            return r;
        }
        const QImage crop = final.copy(px);
        *areaImage = crop; // read on the GUI thread after finished()
        return TextRecognition::recognize(crop, m, langs, flag.get());
    }));
    Q_EMIT m_text->textChanged();
}

void RecognizePanel::onFinished()
{
    m_result = m_watcher->result();
    m_text->setReadOnly(false);
    if (!m_result.ok) {
        m_status->setText(m_result.error);
        Q_EMIT m_text->textChanged();
        Q_EMIT recognitionFinished(false);
        return;
    }
    m_text->setPlainText(m_result.text());
    QStringList info;
    info << (m_result.engine == "handwriting" ? tr("Handwriting (experimental)") : tr("Printed text"));
    info << tr("confidence %1%").arg(int(std::lround(m_result.confidence * 100)));
    QString status = info.join(" · ");
    if (m_result.lines.isEmpty())
        status += "\n" + tr("No text was found in the selected area.");
    else if (m_result.confidence < 0.7)
        status += "\n⚠ " + tr("Low confidence - please check and correct the text.");
    if (looksLikeMathOrDrawing(m_result))
        status += "\n⚠ " + tr("This area may contain formulas or a drawing. Recognition is unreliable there; keep the "
                              "original visible.");
    if (mode() == TextRecognition::Mode::Auto && m_result.engine == "printed" && !HandwritingOcr::modelsInstalled())
        status += "\n" + tr("Handwriting models are not installed - only printed text was recognised.");
    m_status->setText(status);
    Q_EMIT m_text->textChanged();
    Q_EMIT recognitionFinished(true);
}

void RecognizePanel::cancel()
{
    if (isBusy()) {
        m_cancelFlag->store(true);
        m_watcher->waitForFinished();
    }
    m_area = {};
    m_result = {};
    m_text->clear();
    m_text->setReadOnly(false);
    m_status->setText(tr("No area selected yet."));
    m_canvas->clearRecognizeArea();
}

void RecognizePanel::copyText()
{
    QApplication::clipboard()->setText(m_text->toPlainText());
    m_banner->show(Banner::Kind::Success, tr("Recognized text copied."));
}

bool RecognizePanel::looksLikeMathOrDrawing(const RecognitionResult& r)
{
    const QString t = r.text();
    if (t.isEmpty())
        return false;
    int symbols = 0, letters = 0;
    for (const QChar c : t) {
        if (c.isLetter())
            ++letters;
        else if (QStringLiteral("=+-*/^√∫∑∏<>≤≥±×÷()[]{}|∞π").contains(c) || c.isDigit())
            ++symbols;
    }
    return symbols > 0 && double(symbols) / std::max(1, symbols + letters) > 0.35;
}

RecognizePanel::Background RecognizePanel::analyzeBackground() const
{
    Background b;
    const QImage img = m_areaImage.convertToFormat(QImage::Format_RGB32);
    if (img.width() < 6 || img.height() < 6)
        return b;
    // Background: median of the border strip; ink: median of the darkest pixels inside.
    QList<int> r, g, bl, lum;
    auto add = [&](int x, int y) {
        const QRgb p = img.pixel(x, y);
        r << qRed(p);
        g << qGreen(p);
        bl << qBlue(p);
        lum << qGray(p);
    };
    const int m = std::max(1, std::min(img.width(), img.height()) / 40);
    for (int x = 0; x < img.width(); x += 2)
        for (int d = 0; d < m; ++d) {
            add(x, d);
            add(x, img.height() - 1 - d);
        }
    for (int y = 0; y < img.height(); y += 2)
        for (int d = 0; d < m; ++d) {
            add(d, y);
            add(img.width() - 1 - d, y);
        }
    auto median = [](QList<int> v) {
        std::sort(v.begin(), v.end());
        return v.isEmpty() ? 255 : v[v.size() / 2];
    };
    b.color = QColor(median(r), median(g), median(bl));
    const int bgLum = median(lum);
    // Grid, ruled lines or texture: a noticeable share of the border clearly differs from the paper.
    int off = 0;
    for (int v : lum)
        off += std::abs(v - bgLum) > 28 ? 1 : 0;
    b.uniform = double(off) / std::max<qsizetype>(1, lum.size()) < 0.04;
    QList<QRgb> dark;
    for (int y = 0; y < img.height(); y += 2)
        for (int x = 0; x < img.width(); x += 2)
            if (qGray(img.pixel(x, y)) < bgLum - 80)
                dark << img.pixel(x, y);
    if (dark.size() > 20) {
        QList<int> dr, dg, db;
        for (QRgb p : dark) {
            dr << qRed(p);
            dg << qGreen(p);
            db << qBlue(p);
        }
        b.ink = QColor(median(dr), median(dg), median(db));
    }
    return b;
}

QList<Annotation> RecognizePanel::annotationsFor(Variant v) const
{
    QList<Annotation> out;
    const QString text = m_text->toPlainText().trimmed();
    const CombinePage* page = currentPage();
    if (text.isEmpty() || m_area.isEmpty() || !page)
        return out;
    const Background bg = analyzeBackground();
    Annotation t;
    t.type = Annotation::Type::Text;
    t.text = text;
    t.fontFamily = QStringLiteral("Arial");
    t.stroke = v == Variant::CleanNote || v == Variant::KeepAndAdd ? QColor(20, 20, 20) : bg.ink;
    const int lines = std::max(1, int(text.count('\n')) + 1);
    // Size from the handwriting: about 60% of a line's height, 8-36 pt at A4 width.
    t.fontSize = std::clamp(m_area.height() / lines * 0.6, 8.0 / kA4, 36.0 / kA4);
    t.rect = QRectF(m_area.left(), m_area.top(), m_area.width(), 0.01);
    AnnotationRender::fitTextBox(t);
    if (v != Variant::KeepAndAdd)
        while (t.rect.height() > m_area.height() && t.fontSize > 6.0 / kA4) { // fit into the selection
            t.fontSize *= 0.92;
            AnnotationRender::fitTextBox(t);
        }
    const QSizeF px = Geometry::rectifiedPixelSize(*page);
    const QSizeF finalPx = page->crop ? QSizeF(page->crop->width() * px.width(), page->crop->height() * px.height()) : px;
    const double pageHeight = finalPx.height() / std::max(1.0, finalPx.width());
    switch (v) {
    case Variant::Overlay: break;
    case Variant::KeepAndAdd: {
        const double gap = 0.01;
        double top = m_area.bottom() + gap;
        if (top + t.rect.height() > pageHeight) // no room below: above the selection
            top = std::max(0.0, m_area.top() - gap - t.rect.height());
        t.rect.moveTop(top);
        break;
    }
    case Variant::MatchBackground:
    case Variant::CleanNote: {
        Annotation cover;
        cover.type = Annotation::Type::Rect;
        cover.rect = m_area;
        cover.hasFill = true;
        cover.fill = v == Variant::CleanNote ? QColor(Qt::white) : bg.color;
        cover.stroke = v == Variant::CleanNote ? QColor(200, 200, 200) : bg.color;
        cover.width = v == Variant::CleanNote ? 0.6 / kA4 : 0.0;
        out << cover;
        break;
    }
    }
    out << t;
    return out;
}

QImage RecognizePanel::renderPage(const CombinePage& page, int maxEdge)
{
    QImage img = ImageProcessor::render(page, ImageProcessor::Stage::Final, maxEdge).convertToFormat(QImage::Format_ARGB32);
    if (img.isNull())
        return img;
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.scale(img.width(), img.width());
    for (const Annotation& a : page.annotations)
        AnnotationRender::paint(p, a);
    return img;
}

QUuid RecognizePanel::insertAsText()
{
    const CombinePage* page = currentPage();
    QList<Annotation> add = annotationsFor(Variant::KeepAndAdd);
    if (!page || add.isEmpty())
        return {};
    CombinePage copy = *page;
    copy.annotations << add;
    m_doc->updatePage(copy, tr("Insert recognized text"));
    m_canvas->clearRecognizeArea();
    m_banner->show(Banner::Kind::Success, tr("Text inserted below the selection. Select it to move, resize or edit it."));
    return add.last().id;
}

bool RecognizePanel::applyVariant(Variant v)
{
    const CombinePage* page = currentPage();
    const QList<Annotation> add = annotationsFor(v);
    if (!page || add.isEmpty())
        return false;
    CombinePage copy = *page;
    copy.annotations << add;
    m_doc->updatePage(copy, tr("Replace selection with recognized text"));
    m_canvas->clearRecognizeArea();
    m_banner->show(Banner::Kind::Success, tr("Done. The original photo is unchanged - Undo (Ctrl+Z) brings the area back."));
    return true;
}

void RecognizePanel::openReplaceDialog()
{
    const CombinePage* page = currentPage();
    if (!page || m_text->toPlainText().trimmed().isEmpty() || m_area.isEmpty())
        return;
    const Background bg = analyzeBackground();
    QDialog dlg(this);
    dlg.setObjectName("replace-dialog");
    dlg.setWindowTitle(tr("Replace selection"));
    auto* l = new QVBoxLayout(&dlg);
    l->addWidget(mutedLabel(tr("Choose how the recognized text should appear. Nothing changes until you press Apply; the "
                               "original photo is always kept and Undo restores the area."),
                            &dlg));
    if (!bg.uniform)
        l->addWidget(mutedLabel("⚠ " + tr("The background of this area is not plain (grid, ruled lines or texture). Covering it "
                                         "would remove those lines there - an overlay or a clean note is usually better."),
                                &dlg));
    if (looksLikeMathOrDrawing(m_result))
        l->addWidget(mutedLabel("⚠ " + tr("The area may contain formulas or a drawing; keeping the original visible is safer."), &dlg));
    auto* group = new QButtonGroup(&dlg);
    const std::pair<Variant, QString> variants[] = {
        {Variant::Overlay, tr("Insert as overlay (original stays visible)")},
        {Variant::KeepAndAdd, tr("Keep the original and add the text below")},
        {Variant::MatchBackground, tr("Replace on a clean background (matched paper colour)")},
        {Variant::CleanNote, tr("Create a clean note over the area")},
    };
    for (const auto& [v, label] : variants) {
        auto* rb = new QRadioButton(label, &dlg);
        rb->setObjectName(QStringLiteral("variant-%1").arg(int(v)));
        group->addButton(rb, int(v));
        l->addWidget(rb);
    }
    group->button(int(bg.uniform && !looksLikeMathOrDrawing(m_result) ? Variant::MatchBackground : Variant::Overlay))->setChecked(true);

    auto* previews = new QHBoxLayout;
    auto* before = new QLabel(&dlg);
    auto* after = new QLabel(&dlg);
    before->setObjectName("preview-before");
    after->setObjectName("preview-after");
    for (QLabel* p : {before, after}) {
        p->setMinimumSize(300, 200);
        p->setAlignment(Qt::AlignCenter);
        p->setFrameShape(QFrame::StyledPanel);
    }
    auto* col1 = new QVBoxLayout;
    col1->addWidget(new QLabel(tr("Before"), &dlg));
    col1->addWidget(before);
    auto* col2 = new QVBoxLayout;
    col2->addWidget(new QLabel(tr("After"), &dlg));
    col2->addWidget(after);
    previews->addLayout(col1);
    previews->addLayout(col2);
    l->addLayout(previews);

    // Preview window: the selection plus a margin, on the rendered page.
    const QImage beforeImg = renderPage(*page, 1800);
    const double k = beforeImg.width();
    const QRectF win = m_area.adjusted(-m_area.width() * 0.25, -m_area.height() * 0.6, m_area.width() * 0.25, m_area.height() * 0.9);
    const QRect winPx = QRectF(win.left() * k, win.top() * k, win.width() * k, win.height() * k).toAlignedRect().intersected(beforeImg.rect());
    auto show = [](QLabel* label, const QImage& img) {
        label->setPixmap(QPixmap::fromImage(img.scaled(QSize(420, 300), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    };
    show(before, beforeImg.copy(winPx));
    auto refresh = [&] {
        CombinePage preview = *page;
        preview.annotations << annotationsFor(Variant(group->checkedId()));
        show(after, renderPage(preview, 1800).copy(winPx));
    };
    refresh();
    connect(group, &QButtonGroup::idClicked, &dlg, [&] { refresh(); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dlg);
    auto* apply = buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
    apply->setObjectName("replace-apply");
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    l->addWidget(buttons);
    if (dlg.exec() == QDialog::Accepted)
        applyVariant(Variant(group->checkedId()));
}

} // namespace folvio::combine
