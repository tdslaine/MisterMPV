// Small Qt front-end for the mpv MiSTer fork: starts mpv with --vo=mister --ao=mister and
// drives it over mpv's JSON IPC (play/pause, seek, tracks, modeline).
#include <algorithm>
#include <unistd.h>
#include <functional>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QShortcut>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QKeySequence>
#include <QAbstractSpinBox>
#include <functional>
#include <QCollator>
#include <QMenu>
#include <QClipboard>
#include <QToolButton>
#include <QRandomGenerator>
#include <QDateTime>
#include <random>
#include <QUdpSocket>
#include <QHostAddress>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QGridLayout>
#include <QGroupBox>
#include <QDialog>
#include <QUrl>
#include <QDirIterator>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QHash>
#include <QListWidget>
#include <QTreeWidget>
#include <QSet>
#include <QHeaderView>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLocalSocket>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "jellyfin.h"
#include "modeline_keys.h"

// MISTERMPV_DEBUG=1: trace start/IPC/process events to stderr (for bug reports).
static void dbg(const QString& m) {
    static const bool on = qEnvironmentVariableIsSet("MISTERMPV_DEBUG");
    if (on) fprintf(stderr, "[misterMPV %s] %s\n", qPrintable(QDateTime::currentDateTime().toString("HH:mm:ss.zzz")),
                    qPrintable(m));
}

// Colour themes: Fusion style + a palette, the same on every desktop (and in the AppImage, which has
// no KDE style). "System" leaves the desktop's own style and colours alone.
struct Theme {
    const char* name;
    const char *window, *base, *altBase, *text, *button, *highlight, *highlightText, *link, *mid, *disabled;
};
static const Theme kThemes[] = {
    {"System", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
    {"Light", "#efefef", "#ffffff", "#f5f5f5", "#1e1e1e", "#e6e6e6", "#308cc6", "#ffffff", "#0b5cad", "#c4c4c4", "#9a9a9a"},
    {"Dark", "#2b2b2b", "#1e1e1e", "#262626", "#e6e6e6", "#353535", "#3d7bd9", "#ffffff", "#6ea8fe", "#4a4a4a", "#7a7a7a"},
    {"Nord", "#2e3440", "#3b4252", "#434c5e", "#eceff4", "#3b4252", "#88c0d0", "#2e3440", "#81a1c1", "#4c566a", "#7b88a1"},
    {"Gruvbox Dark", "#282828", "#1d2021", "#32302f", "#ebdbb2", "#3c3836", "#d79921", "#282828", "#83a598", "#504945", "#928374"},
    {"Dracula", "#282a36", "#21222c", "#2c2e3b", "#f8f8f2", "#343746", "#bd93f9", "#282a36", "#8be9fd", "#44475a", "#6272a4"},
    {"Solarized Dark", "#002b36", "#073642", "#0a3f4c", "#93a1a1", "#073642", "#268bd2", "#fdf6e3", "#2aa198", "#586e75", "#657b83"},
    {"Catppuccin Mocha", "#1e1e2e", "#181825", "#24243a", "#cdd6f4", "#313244", "#cba6f7", "#1e1e2e", "#89b4fa", "#45475a", "#6c7086"},
    {"Tokyo Night", "#1a1b26", "#16161e", "#202230", "#c0caf5", "#24283b", "#7aa2f7", "#1a1b26", "#7dcfff", "#414868", "#565f89"},
};
static void applyTheme(const QString& name) {
    static const QString sysStyle = QApplication::style()->name();   // captured on the first call
    static const QPalette sysPalette = QApplication::palette();
    const Theme* t = &kThemes[0];
    for (const Theme& x : kThemes) if (name == x.name) t = &x;
    if (!t->window) {
        QApplication::setStyle(QStyleFactory::create(sysStyle));
        QApplication::setPalette(sysPalette);
        return;
    }
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    const QColor win(t->window), base(t->base), alt(t->altBase), text(t->text), btn(t->button),
                 hl(t->highlight), hlText(t->highlightText), link(t->link), mid(t->mid), dis(t->disabled);
    QPalette p;
    p.setColor(QPalette::Window, win);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, btn);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::ToolTipBase, btn);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Highlight, hl);
    p.setColor(QPalette::HighlightedText, hlText);
    p.setColor(QPalette::Link, link);
    p.setColor(QPalette::LinkVisited, link);
    p.setColor(QPalette::PlaceholderText, dis);
    p.setColor(QPalette::BrightText, QColor("#ff5555"));
    p.setColor(QPalette::Light, btn.lighter(135));
    p.setColor(QPalette::Midlight, btn.lighter(115));
    p.setColor(QPalette::Mid, mid);
    p.setColor(QPalette::Dark, btn.darker(160));
    p.setColor(QPalette::Shadow, QColor(0, 0, 0));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, dis);
    p.setColor(QPalette::Disabled, QPalette::Highlight, mid);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, dis);
    QApplication::setPalette(p);
}

// Collapsible section: a flat header button (arrow + title) above a content widget; the state is
// remembered in QSettings and the window follows the new height.
class Section : public QWidget {
public:
    Section(const QString& title, QWidget* content, QSettings& set, const QString& key, bool open)
        : m_content(content) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(2);
        m_head = new QPushButton;
        m_head->setCheckable(true);
        m_head->setFlat(true);
        m_head->setStyleSheet("QPushButton { text-align: left; padding: 4px 2px; border: none; }");
        m_head->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QFont f = m_head->font();
        f.setBold(true);
        m_head->setFont(f);
        lay->addWidget(m_head);
        lay->addWidget(content);
        const bool o = set.value(key, open).toBool();
        m_head->setChecked(o);
        apply(title, o);
        QObject::connect(m_head, &QPushButton::toggled, this, [this, title, &set, key](bool on) {
            set.setValue(key, on);
            apply(title, on);
            QTimer::singleShot(0, this, [this] {
                QWidget* w = window();
                if (w && !w->isMaximized()) w->resize(w->width(), w->sizeHint().height());
            });
        });
    }
private:
    void apply(const QString& title, bool on) {
        m_head->setText((on ? "▾  " : "▸  ") + title);
        m_content->setVisible(on);
    }
    QPushButton* m_head;
    QWidget* m_content;
};

// Playlist table (# | Artist | Title | Time). A QTreeWidget with a few QListWidget-style helpers so the
// rest of the code can treat the top-level items as rows.
class PlaylistView : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    int count() const { return topLevelItemCount(); }
    QTreeWidgetItem* item(int i) const { return topLevelItem(i); }
    int row(const QTreeWidgetItem* it) const { return indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(it)); }
    QTreeWidgetItem* takeItem(int r) { return takeTopLevelItem(r); }
    void addItem(QTreeWidgetItem* it) { addTopLevelItem(it); }
    int currentRow() const { const QModelIndex i = currentIndex(); return i.isValid() ? i.row() : -1; }
    void setCurrentRow(int r) { if (auto* it = topLevelItem(r)) setCurrentItem(it); }
};

class Window : public QWidget {
public:
    Window() {
        migrateLogin();
        applyTheme(m_set.value("theme", "System").toString());
        setWindowTitle("misterMPV");
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 14, 12, 12);
        root->setSpacing(7);

        auto* fileRow = new QHBoxLayout;
        m_file = new QLineEdit;
        m_file->setPlaceholderText("Stream or YouTube URL, smb:// share, or a path - then + URL");
        auto* browse = new QPushButton("+ File(s)");
        browse->setToolTip("Pick one or more files (also network shares) and add them to the playlist");
        auto* addDir = new QPushButton("+ Folder");
        addDir->setToolTip("Add all media files of a folder (including subfolders)");
        auto* addUrl = new QPushButton("+ URL");
        addUrl->setToolTip("Add what is in the box: a stream URL (http, HLS .m3u8), a YouTube link (needs yt-dlp), "
                           "an smb:// share (needs kio-fuse) or a local path");
        auto* addShare = new QPushButton("+ Network share…");
        addShare->setToolTip("Add a file or folder from an SMB share (smb://server/share/...), mounted through kio-fuse");
        fileRow->addWidget(m_file);
        fileRow->addWidget(addUrl);
        fileRow->addWidget(addShare);

        m_list = new PlaylistView;
        m_list->setMinimumHeight(110);
        m_list->setColumnCount(5);
        m_list->setHeaderLabels({"#", "Artist", "Title", "Album", "Time"});
        m_list->setRootIsDecorated(false);
        m_list->setItemsExpandable(false);
        m_list->setUniformRowHeights(true);
        m_list->setAlternatingRowColors(true);
        m_list->setTextElideMode(Qt::ElideRight);
        m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_list->setToolTip("Playlist - double-click to play, drag to reorder, click a column header to sort");
        m_list->setDragDropMode(QAbstractItemView::InternalMove);
        m_list->setDefaultDropAction(Qt::MoveAction);
        m_list->setStyleSheet("QTreeView { alternate-background-color: rgba(128, 128, 128, 34); }");
        {
            QHeaderView* h = m_list->header();
            h->setStretchLastSection(false);
            h->setSectionsClickable(true);
            h->setSortIndicatorShown(true);
            h->setSortIndicator(-1, Qt::AscendingOrder);
            h->setSectionResizeMode(0, QHeaderView::Fixed);
            h->setSectionResizeMode(1, QHeaderView::Interactive);
            h->setSectionResizeMode(2, QHeaderView::Stretch);
            h->setSectionResizeMode(3, QHeaderView::Interactive);
            h->setSectionResizeMode(4, QHeaderView::Fixed);
            m_list->setColumnWidth(0, 56);
            m_list->setColumnWidth(1, 190);
            m_list->setColumnWidth(3, 170);
            m_list->setColumnWidth(4, 68);
            m_list->setColumnHidden(3, !m_set.value("showAlbum", false).toBool());   // optional: header right-click
            m_list->headerItem()->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
            m_list->headerItem()->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        }
        auto* plRow = new QHBoxLayout;
        auto* plPrev = new QPushButton("◀◀  Prev");
        auto* plNext = new QPushButton("Next  ▶▶");
        auto* plRemove = new QPushButton("Remove");
        auto* plClear = new QPushButton("Clear");
        m_repeat = new QComboBox;
        m_repeat->addItems({"Repeat: off", "Repeat: one", "Repeat: all"});
        m_repeat->setCurrentIndex(m_set.value("repeatMode", 0).toInt());
        m_repeat->setToolTip("off: stop after the last entry, one: repeat the current entry, all: loop the playlist");
        m_shuffle = new QCheckBox("Shuffle");
        m_shuffle->setChecked(m_set.value("shuffle", false).toBool());
        m_shuffle->setToolTip("Play the playlist in random order (switch off to restore the original order)");
        m_prev = plPrev;
        m_next = plNext;
        for (auto* b : {browse, addDir, plRemove, plClear}) plRow->addWidget(b);
        auto* sortBtn = new QPushButton("Sort ▾");
        auto* sortMenu = new QMenu(sortBtn);
        const struct { const char* label; const char* key; } sorts[] = {
            {"By artist", "artist"}, {"By title / name", "title"}, {"By album", "album"}, {"By time", "time"},
            {"By folder, then name", "folder"},
            {"By date (oldest first)", "dateold"}, {"By date (newest first)", "datenew"},
            {"Reverse order", "reverse"}, {"Randomize", "random"}};
        for (const auto& so : sorts) {
            const QString key = so.key;
            sortMenu->addAction(so.label, this, [this, key] { sortPlaylist(key); });
        }
        sortBtn->setMenu(sortMenu);
        sortBtn->setToolTip("Sort the playlist (like Winamp's Sort List); \"by folder\" groups entries per directory");
        plRow->addWidget(sortBtn);
        plRow->addWidget(m_repeat);
        plRow->addWidget(m_shuffle);
        plRow->addStretch(1);
        m_libRoots = m_set.value("libRoots").toStringList();
        loadTags();
        for (const QString& f : m_set.value("playlist").toStringList()) addLocal(f);

        m_host = new QLineEdit(qEnvironmentVariable("MISTER_HOST", m_set.value("host").toString()));
        m_host->setPlaceholderText("IP address of the MiSTer");
        m_modeline = new QComboBox;
        m_modeline->addItem("auto (match source)");
        for (const char* k : kModelineKeys) m_modeline->addItem(k);
        m_modeline->setCurrentText(m_set.value("modeline", m_modeline->itemText(0)).toString());
        m_near = new QComboBox;
        m_near->addItems({"lossless", "near 1", "near 2", "near 3"});
        m_near->setCurrentIndex(m_set.value("near", 1).toInt());
        m_field = new QComboBox;
        m_field->addItems({"TFF (top first)", "BFF (bottom first)"});
        m_field->setCurrentIndex(m_set.value("fieldOrder", 0).toInt());
        m_field->setToolTip("Interlaced modelines only. Flip it if motion shimmers or combs.");
        m_flicker = new QComboBox;
        m_flicker->addItems({"off", "light (1-6-1)", "full (1-2-1)"});
        m_flicker->setCurrentIndex(m_set.value("flicker", 1).toInt());
        m_flicker->setToolTip("Interlaced modelines only: vertical softening against 1-line flicker (subtitles, thin lines).");
        m_audio = new QComboBox;
        m_sub = new QComboBox;
        for (auto* c : {m_audio, m_sub}) {
            c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            c->setMinimumContentsLength(52);
            c->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        }
        m_vis = new QComboBox;
        m_vis->addItem("Off (cover art / black)", "off");
        m_vis->addItem("Spectrum (CQT)", "spectrum");
        m_vis->addItem("Waves", "waves");
        m_vis->addItem("Vectorscope", "vectorscope");
        m_vis->addItem("Spectrogram", "spectrogram");
        m_vis->addItem("Frequency bars", "bars");
        m_vis->setCurrentIndex(qMax(0, m_vis->findData(m_set.value("visualizer", "off"))));
        m_vis->setToolTip("Picture for audio-only files (mp3, flac, ...); video files are not affected.");
        m_subScale = new QSpinBox;
        m_subScale->setRange(20, 300);
        m_subScale->setSingleStep(10);
        m_subScale->setSuffix(" %");
        m_subScale->setValue(m_set.value("subScale", 100).toInt());
        m_subScale->setToolTip("Text subtitle size (bitmap subs like PGS/DVD are not scalable).");
        m_subPos = new QSpinBox;
        m_subPos->setRange(0, 100);
        m_subPos->setSuffix(" %");
        m_subPos->setValue(m_set.value("subPos", 100).toInt());
        m_subPos->setToolTip("Vertical subtitle position: 100 = bottom of the screen, lower moves up.");
        m_osdSize = new QSpinBox;
        m_osdSize->setRange(12, 72);
        m_osdSize->setSuffix(" px");
        m_osdSize->setValue(m_set.value("osdSize", 28).toInt());
        m_osdSize->setToolTip("Height of the on-screen text (gamepad status, volume, track names) in raster "
                              "pixels, independent of the modeline. Bold with a heavy outline so it stays "
                              "readable at 240p.");
        m_trackInfo = new QCheckBox("Show artist / title for music");
        m_trackInfo->setChecked(m_set.value("trackInfo", false).toBool());
        m_trackInfo->setToolTip("Permanent artist + title text on the TV while an audio-only file plays "
                                "(from the tags, else the file name). Uses the OSD text size.");
        m_trackPos = new QComboBox;
        m_trackPos->addItems({"Bottom", "Top"});
        m_trackPos->setCurrentIndex(m_set.value("trackPos", 0).toInt());
        m_subBars = new QCheckBox("Subtitles in black bars");
        m_subBars->setChecked(m_set.value("subBars", true).toBool());
        m_subBars->setToolTip("Letterboxed films: put subtitles into the black bar below the picture "
                              "instead of over it (also for styled ASS subtitles).");
        m_subColor = new QComboBox;
        m_subColor->addItems({"White", "Yellow", "Cyan", "Light green"});
        m_subColor->setCurrentIndex(m_set.value("subColor", 0).toInt());
        m_subColor->setToolTip("Colour of text subtitles (SRT/ASS). Bitmap subtitles (DVD/PGS) cannot be recoloured.");
        auto mkSpin = [this](const char* key, int lo, int hi, int def, const char* suffix) {
            auto* s = new QSpinBox;
            s->setRange(lo, hi);
            s->setSuffix(suffix);
            s->setValue(m_set.value(key, def).toInt());
            return s;
        };
        m_bri = mkSpin("brightness", -100, 100, 0, "");
        m_con = mkSpin("contrast", -100, 100, 0, "");
        m_sat = mkSpin("saturation", -100, 100, 0, "");
        m_blk = mkSpin("blacklevel", 0, 30, 0, "");
        m_scn = mkSpin("scanlines", 0, 100, 0, " %");
        m_hsize = new QDoubleSpinBox;
        m_hsize->setRange(80, 100); m_hsize->setDecimals(1); m_hsize->setSingleStep(0.5); m_hsize->setSuffix(" %");
        m_hsize->setValue(m_set.value("hsize", 100.0).toDouble());
        m_vsize = new QDoubleSpinBox;
        m_vsize->setRange(80, 100); m_vsize->setDecimals(1); m_vsize->setSingleStep(0.5); m_vsize->setSuffix(" %");
        m_vsize->setValue(m_set.value("vsize", 100.0).toDouble());
        m_hoff = mkSpin("hoffset", -72, 72, 0, " px");
        m_voff = mkSpin("voffset", -28, 28, 0, " lines");
        m_calib = new QCheckBox("Show calibration pattern");
        m_calib->setToolTip("Test pattern instead of the video (needs a loaded file). "
                            "Adjust until the white border is just visible on the CRT.");
        auto* calReset = new QPushButton("Reset");
        connect(calReset, &QPushButton::clicked, this, [this] {
            m_hsize->setValue(100); m_vsize->setValue(100); m_hoff->setValue(0); m_voff->setValue(0);
        });
        m_scanStyle = new QComboBox;
        m_scanStyle->addItems({"Line (every 2nd row)", "Thin (every 3rd row)",
                               "Bright lines (dark + boosted)", "RGB mask (columns)"});
        m_scanStyle->setCurrentIndex(m_set.value("scanStyle", 0).toInt());
        m_scanStyle->setToolTip("Pattern the Scanlines strength is applied with. At 240p try Thin, "
                                "Bright lines or RGB mask if the standard lines look blurry.");
        m_aspect = new QComboBox;
        m_aspect->addItems({"Auto (keep aspect, bars)", "Stretch (fill screen)", "Zoom (crop to fill)",
                            "Force 4:3", "Force 16:9", "Force 21:9"});
        m_aspect->setCurrentIndex(m_set.value("aspect", 0).toInt());
        // ---- Output group: settings in pairs -----------------------------------------------
        auto* outBox = new QGroupBox("Output");
        auto* og = new QGridLayout(outBox);
        auto addPair = [og](int row, const QString& l1, QWidget* w1, const QString& l2, QWidget* w2) {
            og->addWidget(new QLabel(l1), row, 0);
            og->addWidget(w1, row, 1);
            if (w2) { og->addWidget(new QLabel(l2), row, 2); og->addWidget(w2, row, 3); }
        };
        addPair(0, "MiSTer:", m_host, "Codec:", m_near);
        addPair(1, "Modeline:", m_modeline, "Format:", m_aspect);
        addPair(2, "Field order:", m_field, "Flicker filter:", m_flicker);
        auto* testBtn = new QPushButton("Test connection");
        m_core = new QComboBox;
        m_core->addItems({"GroovyNLC (default)", "Groovy (original)"});
        m_core->setCurrentIndex(m_set.value("coreKind", 0).toInt());
        m_core->setToolTip("Which core \"Launch core\" loads. misterMPV streams with the NLC codec, "
                           "so it needs GroovyNLC; the original Groovy core cannot decode it.");
        connect(m_core, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int i) { m_set.setValue("coreKind", i); });
        auto* coreBtn = new QPushButton("Launch core");
        auto* sshBtn = new QPushButton("Setup…");
        sshBtn->setToolTip("SSH login, SSH port and core path for \"Launch Groovy core\"");
        testBtn->setToolTip("Sends a status request to the MiSTer (UDP 32100) and waits for the reply");
        coreBtn->setToolTip("Over SSH: load the selected core on the MiSTer (newest matching file)");
        auto* misterRow = new QHBoxLayout;
        misterRow->addWidget(testBtn);
        misterRow->addWidget(coreBtn);
        misterRow->addWidget(m_core);
        m_pad = new QCheckBox("Gamepad");
        m_pad->setChecked(m_set.value("pad", true).toBool());
        m_pad->setToolTip("Control playback with the pad on the MiSTer:\n"
                          "D-pad left/right: seek 10 s (hold = scan), up/down: volume\n"
                          "B1/Start: pause, B2: stop, B3: audio track, B4: subtitle track\n"
                          "L1/R1: previous/next, L2/R2: seek 60 s, Select: status on TV\n"
                          "(applies on the next Play)");
        misterRow->addWidget(sshBtn);
        m_jfLabel = new QLabel;
        m_jfLabel->setToolTip("Jellyfin receiver (Setup… → Jellyfin receiver)");
        misterRow->addSpacing(8);
        misterRow->addWidget(m_jfLabel, 1);
        og->addLayout(misterRow, 3, 0, 1, 4);
        connect(testBtn, &QPushButton::clicked, this, [this] { testConnection(); });
        connect(coreBtn, &QPushButton::clicked, this, [this] { launchCore(); });
        connect(sshBtn, &QPushButton::clicked, this, [this] { sshDialog(); });
        buildSetupDialog();
        og->setColumnStretch(1, 1);
        og->setColumnStretch(3, 1);

        // ---- Tracks group --------------------------------------------------------------------
        auto* trackBox = new QGroupBox("Audio / Subtitles");
        auto* tf = new QFormLayout(trackBox);
        tf->addRow("Audio:", m_audio);
        tf->addRow("Subtitles:", m_sub);
        tf->addRow("Visualizer:", m_vis);
        tf->addRow("Subtitle size:", sliderRow(m_subScale));
        tf->addRow("Subtitle position:", sliderRow(m_subPos));

        // ---- Picture group: colour sliders + CRT calibration dialog ---------------------------
        auto* picBox = new QGroupBox("Picture");
        auto* pf = new QFormLayout(picBox);
        pf->addRow("Brightness:", sliderRow(m_bri));
        pf->addRow("Contrast:", sliderRow(m_con));
        pf->addRow("Saturation:", sliderRow(m_sat));
        pf->addRow("Black level:", sliderRow(m_blk));
        pf->addRow("Scanlines:", sliderRow(m_scn));
        pf->addRow("Scanline style:", m_scanStyle);
        auto* picBtns = new QHBoxLayout;
        auto* picReset = new QPushButton("Reset colours");
        auto* calBtn = new QPushButton("CRT calibration…");
        picBtns->addWidget(picReset);
        picBtns->addStretch(1);
        picBtns->addWidget(calBtn);
        pf->addRow(picBtns);
        connect(picReset, &QPushButton::clicked, this, [this] {
            for (auto* sp : {m_bri, m_con, m_sat, m_blk, m_scn}) sp->setValue(0);
        });

        m_calDlg = new QDialog(this);
        m_calDlg->setWindowTitle("CRT calibration");
        auto* cf = new QFormLayout(m_calDlg);
        cf->addRow("Width:", sliderRow(m_hsize));
        cf->addRow("Height:", sliderRow(m_vsize));
        cf->addRow("Shift horizontal:", sliderRow(m_hoff));
        cf->addRow("Shift vertical:", sliderRow(m_voff));
        auto* calRow = new QHBoxLayout;
        calRow->addWidget(m_calib, 1);
        calRow->addWidget(calReset);
        cf->addRow(calRow);

        // Modeline fine-tune: moves the sync pulse of one modeline, so the whole picture shifts on the CRT
        // without losing pixels (unlike the shift above, which moves the picture inside the raster).
        auto* fine = new QGroupBox("Modeline fine-tune (sync position, per modeline)");
        auto* ff = new QFormLayout(fine);
        m_shiftMl = new QComboBox;
        for (const char* k : kModelineKeys) m_shiftMl->addItem(k);
        m_shiftMl->setToolTip("The modeline to adjust. With Modeline = auto, misterMPV picks 240p/480i (60 Hz) or "
                              "288p/576i (50 Hz) from the source; adjust each mode you use once.");
        m_shiftH = new QSpinBox; m_shiftH->setRange(-16, 16); m_shiftH->setSuffix(" px");
        m_shiftV = new QSpinBox; m_shiftV->setRange(-8, 8); m_shiftV->setSuffix(" lines");
        m_shiftH->setToolTip("+ moves the picture right. Applied live; limited to what the blanking allows.");
        m_shiftV->setToolTip("+ moves the picture down. Applied live; limited to what the blanking allows.");
        auto* shiftReset = new QPushButton("Reset this modeline");
        ff->addRow("Modeline:", m_shiftMl);
        ff->addRow("Horizontal:", m_shiftH);
        ff->addRow("Vertical:", m_shiftV);
        ff->addRow("", shiftReset);
        cf->addRow(fine);
        loadSyncShift();
        {
            const QString cur = m_modeline->currentIndex() > 0 ? m_modeline->currentText()
                                                                : m_set.value("syncShiftLast", "640x480i60").toString();
            const int i = m_shiftMl->findText(cur);
            m_shiftMl->setCurrentIndex(i >= 0 ? i : 0);
            showSyncShift();
        }
        connect(m_shiftMl, &QComboBox::currentTextChanged, this, [this](const QString& k) {
            m_set.setValue("syncShiftLast", k);
            showSyncShift();
        });
        for (QSpinBox* sp : {m_shiftH, m_shiftV})
            connect(sp, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { storeSyncShift(); });
        connect(shiftReset, &QPushButton::clicked, this, [this] { m_shiftH->setValue(0); m_shiftV->setValue(0); });
        m_calDlg->setMinimumWidth(460);
        connect(calBtn, &QPushButton::clicked, this, [this] { m_calDlg->show(); m_calDlg->raise(); });
        connect(m_calDlg, &QDialog::finished, this, [this] { m_calib->setChecked(false); });

        // ---- Transport (top) -------------------------------------------------------------------
        m_seek = new QSlider(Qt::Horizontal);
        m_seek->setRange(0, 1000);
        m_time = new QLabel("0:00 / 0:00");
        m_status = new QLabel;
        auto* back = new QPushButton("−10 s");
        auto* fwd = new QPushButton("+10 s");
        for (auto* b : {back, fwd}) { b->setFixedWidth(60); b->setToolTip("Seek 10 seconds"); }
        auto* seekRow = new QHBoxLayout;
        seekRow->addWidget(back);
        seekRow->addWidget(m_seek, 1);
        seekRow->addWidget(fwd);
        seekRow->addWidget(m_time);

        // Prev | Play/Pause (one toggle) | Stop | Next, all the same width
        m_play = new QPushButton("▶  Play");
        m_stop = new QPushButton("■  Stop");
        auto* btns = new QHBoxLayout;
        for (auto* b : {(QPushButton*)plPrev, m_play, m_stop, (QPushButton*)plNext}) {
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            btns->addWidget(b, 1);
        }

        auto* plBox = new QGroupBox("Playlist");
        m_plBox = plBox;
        auto* plLay = new QVBoxLayout(plBox);
        plLay->addWidget(m_list, 1);
        plLay->addLayout(plRow);
        plLay->addLayout(fileRow);   // URL bar for YouTube / streams under the button row
        QFont sf = m_status->font();
        sf.setBold(true);
        sf.setPointSizeF(sf.pointSizeF() + 1);
        m_status->setFont(sf);
        m_status->setText("stopped");
        m_led = new QLabel;
        m_led->setFixedSize(16, 16);
        setLed(Led::Stopped);
        auto* statusRow = new QHBoxLayout;
        statusRow->addWidget(m_led, 0, Qt::AlignVCenter);
        statusRow->addSpacing(4);
        statusRow->addWidget(m_status, 1);
        root->addLayout(statusRow);
        root->addSpacing(8);   // air between the title line and the seek bar
        root->addLayout(seekRow);
        root->addLayout(btns);

        m_vol = new QSpinBox;
        m_vol->setRange(0, 130);
        m_vol->setSuffix(" %");
        m_vol->setValue(m_set.value("volume", 100).toInt());
        m_vol->setToolTip("Volume (above 100 % is software gain)");
        m_speed = new QDoubleSpinBox;
        m_speed->setRange(0.25, 4.0);
        m_speed->setDecimals(2);
        m_speed->setSingleStep(0.05);
        m_speed->setSuffix(" ×");
        m_speed->setValue(1.0);
        m_speed->setToolTip("Playback speed (pitch is corrected). Resets to 1× on the next start.");
        auto* speedReset = new QPushButton("1×");
        speedReset->setMaximumWidth(40);
        connect(speedReset, &QPushButton::clicked, this, [this] { m_speed->setValue(1.0); });
        auto* volRow = new QHBoxLayout;
        volRow->addWidget(new QLabel("Volume"));
        volRow->addWidget(sliderRow(m_vol), 3);
        volRow->addSpacing(14);
        volRow->addWidget(new QLabel("Speed"));
        volRow->addWidget(sliderRow(m_speed, 100), 3);
        volRow->addWidget(speedReset);
        root->addLayout(volRow);
        connect(m_vol, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
            if (m_remoteVS) return;
            m_set.setValue("volume", v);
            ctl({"set_property", "volume", v});
        });
        connect(m_speed, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
            if (m_remoteVS) return;
            ctl({"set_property", "speed", v});
        });
        root->addSpacing(14);        // air between the transport buttons and the playlist
        root->addWidget(plBox, 1);
        root->addWidget(outBox);
        trackBox->setTitle(QString());
        root->addWidget(new Section("Audio / Subtitles", trackBox, m_set, "secAudio", false));
        picBox->setTitle(QString());
        root->addWidget(new Section("Picture", picBox, m_set, "secPicture", false));
        setMinimumWidth(720);
        setControlsEnabled(false);

        connect(browse, &QPushButton::clicked, this, [this] {
            const QList<QUrl> urls = QFileDialog::getOpenFileUrls(
                this, "Add media", QUrl(), QString(), nullptr, {}, {"file", "smb", "sftp", "ftp", "davs", "webdav"});
            for (const QUrl& u : urls) addEntry(resolveLocation(u.isLocalFile() ? u.toLocalFile() : u.toString()));
        });
        connect(addDir, &QPushButton::clicked, this, [this] {
            const QUrl dir = QFileDialog::getExistingDirectoryUrl(
                this, "Add folder", QUrl(), QFileDialog::ShowDirsOnly,
                {"file", "smb", "sftp", "ftp", "davs", "webdav"});
            if (dir.isEmpty()) return;
            addFolder(resolveLocation(dir.isLocalFile() ? dir.toLocalFile() : dir.toString()));
        });
        connect(addUrl, &QPushButton::clicked, this, [this] {
            const QString f = m_file->text().trimmed();
            if (f.isEmpty()) return;
            if (addLocation(f)) m_file->clear();
        });
        connect(addShare, &QPushButton::clicked, this, [this] { addNetworkShare(); });
        connect(m_file, &QLineEdit::returnPressed, addUrl, &QPushButton::click);
        connect(plRemove, &QPushButton::clicked, this, [this] { removeSelected(); });
        connect(plClear, &QPushButton::clicked, this, [this] {
            if (running()) cmd({"playlist-clear"}); else { m_list->clear(); renumber(); }
            savePlaylist();
        });
        connect(plPrev, &QPushButton::clicked, this, [this] { prevAny(); });
        connect(plNext, &QPushButton::clicked, this, [this] { nextAny(); });
        // Drag & drop reorder: while mpv runs it owns the order, so mirror each move there
        // (it then reports the new list back); otherwise just persist the local order.
        connect(m_list->model(), &QAbstractItemModel::rowsMoved, this,
                [this](const QModelIndex&, int start, int end, const QModelIndex&, int dest) {
            if (m_syncing) return;
            if (!running()) renumber();
            if (running()) {
                const int n = end - start + 1;
                if (dest > end)
                    for (int i = 0; i < n; i++) cmd(QJsonArray{"playlist-move", start, dest});
                else
                    for (int i = 0; i < n; i++) cmd(QJsonArray{"playlist-move", start + i, dest + i});
            }
            savePlaylist();
        });
        m_list->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) { playlistMenu(p); });
        m_list->header()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_list->header(), &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
            QMenu menu(this);
            QAction* a = menu.addAction("Show album column");
            a->setCheckable(true);
            a->setChecked(!m_list->isColumnHidden(3));
            connect(a, &QAction::toggled, this, [this](bool on) {
                m_list->setColumnHidden(3, !on);
                m_set.setValue("showAlbum", on);
            });
            menu.exec(m_list->header()->mapToGlobal(p));
        });
        connect(m_list->header(), &QHeaderView::sectionClicked, this, [this](int c) { sortByColumn(c); });
        connect(m_repeat, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
            m_set.setValue("repeatMode", i);
            cmd({"set_property", "loop-file", i == 1 ? "inf" : "no"});
            cmd({"set_property", "loop-playlist", i == 2 ? "inf" : "no"});
        });
        connect(m_shuffle, &QCheckBox::toggled, this, [this](bool on) {
            m_set.setValue("shuffle", on);
            cmd({"script-message", "mister-shuffle", on ? "yes" : "no"});
        });
        connect(m_play, &QPushButton::clicked, this, [this] { togglePauseAny(); });   // label follows mpv's pause
        connect(m_stop, &QPushButton::clicked, this, [this] { stopAny(); });
        connect(back, &QPushButton::clicked, this, [this] { ctl({"seek", -10, "relative"}); });
        connect(fwd, &QPushButton::clicked, this, [this] { ctl({"seek", 10, "relative"}); });
        connect(m_seek, &QSlider::sliderReleased, this, [this] {
            if (m_duration > 0)
                ctl({"seek", m_seek->value() / 1000.0 * m_duration, "absolute"});
        });
        connect(m_audio, &QComboBox::activated, this, [this](int i) {
            ctl(QJsonArray{"set_property", "aid", QJsonValue::fromVariant(m_audio->itemData(i))});
        });
        connect(m_sub, &QComboBox::activated, this, [this](int i) {
            ctl(QJsonArray{"set_property", "sid", QJsonValue::fromVariant(m_sub->itemData(i))});
        });
        for (auto* s : {m_hoff, m_voff})
            connect(s, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { applyView(); });
        for (auto* s : {m_hsize, m_vsize})
            connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { applyView(); });
        connect(m_calib, &QCheckBox::toggled, this, [this](bool) { applyView(); });
        for (auto* s : {m_bri, m_con, m_sat, m_blk, m_scn})
            connect(s, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { applyView(); });
        connect(m_scanStyle, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { applyView(); });
        connect(m_subScale, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { applyView(); });
        connect(m_subPos, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { applyView(); });
        connect(m_subColor, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { applyView(); });
        connect(m_subBars, &QCheckBox::toggled, this, [this](bool) { applyView(); });
        connect(m_osdSize, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { applyView(); });
        auto applyTrack = [this] {
            m_set.setValue("trackInfo", m_trackInfo->isChecked());
            m_set.setValue("trackPos", m_trackPos->currentIndex());
            const QJsonArray msg{"script-message", "mister-track", m_trackInfo->isChecked() ? "yes" : "no",
                                 m_trackPos->currentIndex() ? "top" : "bottom"};
            cmd(msg);
            m_jf.cmd(msg);
        };
        connect(m_trackInfo, &QCheckBox::toggled, this, applyTrack);
        connect(m_trackPos, qOverload<int>(&QComboBox::currentIndexChanged), this, applyTrack);
        connect(m_vis, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            m_set.setValue("visualizer", m_vis->currentData().toString());
            const QJsonArray msg{"script-message", "mister-vis", m_vis->currentData().toString()};
            cmd(msg);
            m_jf.cmd(msg);
        });
        connect(m_aspect, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { applyView(); });
        connect(&m_proc, &QProcess::finished, this, [this] { onStopped(); });
        connect(&m_proc, &QProcess::stateChanged, this, [this](QProcess::ProcessState st) {
            dbg(QString("mpv process state %1 pid %2 exit %3").arg(int(st)).arg(m_proc.processId()).arg(m_proc.exitCode()));
        });
        connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            dbg(QString("mpv process error %1: %2").arg(int(e)).arg(m_proc.errorString()));
        });
        connect(&m_sock, &QLocalSocket::stateChanged, this, [](QLocalSocket::LocalSocketState st) {
            dbg(QString("ipc socket state %1").arg(int(st)));
        });
        connect(&m_ssh, &QProcess::finished, this, [this](int code) { onSshFinished(code); });
        connect(&m_ssh, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) { m_status->setText("ssh not found - install openssh"); }
        });
        connect(&m_sock, &QLocalSocket::readyRead, this, &Window::onData);
        m_retry.setInterval(150);
        connect(&m_retry, &QTimer::timeout, this, [this] {
            m_sock.connectToServer(m_sockPath);
            const bool ok = m_sock.waitForConnected(50);
            dbg(QString("ipc try %1: %2 state=%3 err=%4 file=%5 proc=%6").arg(m_tries).arg(m_sockPath)
                .arg(int(m_sock.state())).arg(m_sock.errorString()).arg(QFileInfo::exists(m_sockPath)).arg(int(m_proc.state())));
            if (ok || ++m_tries > 40) {
                m_retry.stop();
                if (m_sock.state() == QLocalSocket::ConnectedState) onConnected();
                else if (m_proc.state() != QProcess::NotRunning) { setLed(Led::Stopped); m_status->setText("could not reach mpv's control socket"); }
            }
        });

        setAcceptDrops(true);
        m_list->viewport()->installEventFilter(this);
        installShortcuts();
        restoreGeometry(m_set.value("geometry").toByteArray());
        showTagProgress();

        // Jellyfin receiver: state into the status line, output settings follow the UI.
        m_jf.onStatus = [this](const QString& s) {
            m_jfStatus->setText(s);
            m_jfLabel->setText(s == "off" ? QString() : "Jellyfin: " + s.section(" - ", 0, 0));
            m_jfLabel->setToolTip(s);
        };
        m_jf.onLoginCode = [this](const QString& code) {
            m_jfStatus->setText("<b style='font-size:15pt'>" + code.toHtmlEscaped() + "</b><br>"
                                "Enter this code in Jellyfin within 5 minutes: user menu → Quick Connect");
        };
        m_jf.onState = [this](bool a, const QString& t, bool p) { jfState(a, t, p); };
        m_jf.onConnected = [this] { applyView(); };
        m_jf.onProperty = [this](const QString& n, const QJsonValue& v) { if (casting()) onProp(n, v); };
        connect(m_host, &QLineEdit::editingFinished, this, [this] { m_set.setValue("host", m_host->text()); jfSync(); });
        for (auto* c : {m_modeline, m_near, m_field, m_flicker})
            connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { jfSync(); });
        connect(m_pad, &QCheckBox::toggled, this, [this](bool) { jfSync(); });
        m_jfName->setText(m_set.value("jfName", "MiSTer").toString());
        m_jfOn->setChecked(m_set.value("jfOn", false).toBool());
        QTimer::singleShot(0, this, [this] { jfApplySetup(); });
    }

protected:
    void closeEvent(QCloseEvent* e) override {
        m_jf.shutdown();   // the receiver lives and dies with the window
        m_set.setValue("geometry", saveGeometry());
        saveTags();
        QWidget::closeEvent(e);
    }
    void dragEnterEvent(QDragEnterEvent* e) override { acceptDrop(e); }
    void dragMoveEvent(QDragMoveEvent* e) override { acceptDrop(e); }
    void dropEvent(QDropEvent* e) override { takeDrop(e); }
    bool eventFilter(QObject* o, QEvent* ev) override {
        if (o == m_list->viewport()) {   // files from a file manager dropped onto the list
            switch (ev->type()) {
            case QEvent::DragEnter:
            case QEvent::DragMove: if (acceptDrop(static_cast<QDropEvent*>(ev))) return true; break;
            case QEvent::Drop:     if (takeDrop(static_cast<QDropEvent*>(ev))) return true; break;
            default: break;
            }
        }
        return QWidget::eventFilter(o, ev);
    }

public:
    // Command line / "open with": add everything and start with the first new entry.
    void openArgs(const QStringList& args) {
        const int first = m_list->count();
        for (const QString& a : args) {
            const QFileInfo fi(a);
            if (fi.isDir()) addFolder(fi.absoluteFilePath());
            else if (fi.exists()) addEntry(fi.absoluteFilePath());
            else if (a.contains("://")) addEntry(resolveLocation(a));
        }
        if (m_list->count() > first && m_proc.state() == QProcess::NotRunning) start(first);
    }

private:
    bool acceptDrop(QDropEvent* e) {
        if (e->mimeData()->hasUrls() || (e->mimeData()->hasText() && e->mimeData()->text().contains("://"))) {
            e->acceptProposedAction();
            return true;
        }
        return false;
    }
    bool takeDrop(QDropEvent* e) {
        if (!acceptDrop(e)) return false;
        QList<QUrl> urls = e->mimeData()->urls();
        if (urls.isEmpty()) urls << QUrl::fromUserInput(e->mimeData()->text().trimmed());
        for (const QUrl& u : urls) {
            const QString loc = u.isLocalFile() ? u.toLocalFile() : u.toString();
            if (u.isLocalFile() && QFileInfo(loc).isDir()) addFolder(loc);
            else addEntry(resolveLocation(loc));
        }
        return true;
    }

    // Winamp keys: Z prev, X play/restart, C pause, V stop, B next; Space = pause; Ctrl+arrows seek/volume.
    void installShortcuts() {
        auto typing = [] {
            QWidget* f = QApplication::focusWidget();
            auto* cb = qobject_cast<QComboBox*>(f);
            return qobject_cast<QLineEdit*>(f) || qobject_cast<QAbstractSpinBox*>(f) || (cb && cb->isEditable());
        };
        auto bind = [this, typing](const QKeySequence& k, std::function<void()> fn) {
            auto* sc = new QShortcut(k, this);
            connect(sc, &QShortcut::activated, this, [typing, fn] { if (!typing()) fn(); });
        };
        bind(Qt::Key_Space, [this] { togglePauseAny(); });
        bind(Qt::Key_C, [this] { togglePauseAny(); });
        bind(Qt::Key_X, [this] { if (m_proc.state() == QProcess::NotRunning) start(); else cmd({"seek", 0, "absolute"}); });
        bind(Qt::Key_V, [this] { stopAny(); });
        bind(Qt::Key_Z, [this] { prevAny(); });
        bind(Qt::Key_B, [this] { nextAny(); });
        bind(Qt::Key_M, [this] { ctl({"cycle", "mute"}); });
        bind(Qt::CTRL | Qt::Key_Left,  [this] { ctl({"seek", -10, "relative"}); });
        bind(Qt::CTRL | Qt::Key_Right, [this] { ctl({"seek", 10, "relative"}); });
        bind(Qt::CTRL | Qt::SHIFT | Qt::Key_Left,  [this] { ctl({"seek", -60, "relative"}); });
        bind(Qt::CTRL | Qt::SHIFT | Qt::Key_Right, [this] { ctl({"seek", 60, "relative"}); });
        bind(Qt::CTRL | Qt::Key_Up,   [this] { m_vol->setValue(m_vol->value() + 5); });
        bind(Qt::CTRL | Qt::Key_Down, [this] { m_vol->setValue(m_vol->value() - 5); });
        auto* del = new QShortcut(Qt::Key_Delete, m_list);
        del->setContext(Qt::WidgetShortcut);
        connect(del, &QShortcut::activated, this, [this] { removeSelected(); });
        // Double-click and Enter both arrive as itemActivated (a double-click also emits itemDoubleClicked,
        // so connecting both started mpv twice: the second start deleted the first mpv's IPC socket).
        connect(m_list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* it) {
            const int r = m_list->row(it);
            if (running()) cmd(QJsonArray{"set_property", "playlist-pos", r}); else start(r);
        });
    }

    void setControlsEnabled(bool on) {
        for (auto* w : {(QWidget*)m_prev, (QWidget*)m_next, (QWidget*)m_stop, (QWidget*)m_seek, (QWidget*)m_audio, (QWidget*)m_sub})
            w->setEnabled(on);
    }

    QString mpvPath() const {
        const QString env = qEnvironmentVariable("MPV_MISTER");
        if (!env.isEmpty()) return env;
        const QString dir = QCoreApplication::applicationDirPath();
        for (const QString& c : {dir + "/mpv", dir + "/../../build/mpv"})   // next to us, then the build tree
            if (QFileInfo::exists(c)) return c;
        return "mpv";
    }

    // KDE network locations (smb://user@host/share/file, sftp://..., ...) are not playable by mpv
    // directly (no libsmbclient in ffmpeg); kio-fuse exposes them as plain files under
    // $XDG_RUNTIME_DIR/kio-fuse-*/<scheme>/<user@host>/<path>, so translate to that path.
    static QString resolveLocation(const QString& loc) {
        const QUrl u(loc);
        const QString scheme = u.scheme();
        static const QStringList kio = {"smb", "sftp", "ftp", "davs", "webdav"};
        if (!kio.contains(scheme)) return loc;
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR", QString("/run/user/%1").arg(getuid()));
        QDirIterator it(runtime, {"kio-fuse-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            const QString base = it.next() + "/" + scheme;
            QString hostDir = u.host();
            if (!u.userName().isEmpty()) hostDir = u.userName() + "@" + u.host();
            QString cand = base + "/" + hostDir + u.path();
            if (QFileInfo::exists(cand)) return cand;
            // URL without user: look for any "<user>@host" directory
            for (const QString& d : QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                if (d == u.host() || d.endsWith("@" + u.host())) {
                    cand = base + "/" + d + u.path();
                    if (QFileInfo::exists(cand)) return cand;
                }
            }
        }
        // No mount yet (e.g. after a reboot): ask kio-fuse to mount it; it answers with the local path.
        QDBusInterface vfs("org.kde.KIOFuse", "/org/kde/KIOFuse", "org.kde.KIOFuse.VFS");
        vfs.setTimeout(15000);
        const QDBusReply<QString> r = vfs.call("mountUrl", loc);
        if (r.isValid() && QFileInfo::exists(r.value())) return r.value();
        return loc;   // kio-fuse not available: let mpv try (will fail for smb)
    }

    // Before handing the list to mpv: network URLs -> kio-fuse paths, and paths into a kio-fuse
    // dir that no longer exists (its name changes with every kio-fuse start) back to their URL first.
    // One resolve per host; the other entries reuse its prefix so a big list is not stat'ed over SMB.
    static QStringList playableLocations(const QStringList& in) {
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR", QString("/run/user/%1").arg(getuid()));
        QHash<QString, QString> prefix;   // "smb://user@host" -> "<kio-fuse dir>/smb/user@host"
        QHash<QString, int> tries;
        QHash<QString, bool> fuseDirOk;
        QStringList out;
        for (QString f : in) {
            if (f.startsWith(runtime + "/kio-fuse-")) {
                const QString dir = f.section('/', 0, runtime.count('/') + 1);
                if (!fuseDirOk.contains(dir)) fuseDirOk[dir] = QFileInfo::exists(dir);
                if (!fuseDirOk[dir]) {   // <dir>/<scheme>/<user@host>/<path>
                    const QString rest = f.mid(dir.size() + 1);
                    QUrl u;
                    u.setScheme(rest.section('/', 0, 0));
                    u.setAuthority(rest.section('/', 1, 1));
                    u.setPath("/" + rest.section('/', 2));
                    f = u.toString();
                }
            }
            const QUrl u(f);
            if (u.scheme().isEmpty() || u.isLocalFile() || !f.contains("://")) { out << f; continue; }
            const QString key = u.scheme() + "://" + u.authority();
            if (prefix.value(key).isEmpty() && tries[key]++ < 3) {   // a missing file fails; try the next one
                const QString r = resolveLocation(f);
                if (r != f && r.endsWith(u.path())) prefix[key] = r.chopped(u.path().size());
            }
            out << (prefix.value(key).isEmpty() ? f : prefix[key] + u.path());
        }
        return out;
    }

    // Status LED: green = playing, blue = paused, red = stopped
    enum class Led { Stopped, Starting, Playing, Paused };
    void setLed(Led l) {
        const char *hi, *mid, *lo, *tip;
        switch (l) {
        case Led::Playing: hi = "#c4ffc4"; mid = "#22c522"; lo = "#0b6b0b"; tip = "playing"; break;
        case Led::Paused:  hi = "#c4dcff"; mid = "#2a7bff"; lo = "#0b3a8a"; tip = "paused"; break;
        case Led::Starting: hi = "#fff3b8"; mid = "#f2c200"; lo = "#7a5c00"; tip = "starting"; break;
        default:           hi = "#ffc4c4"; mid = "#e02020"; lo = "#7a0a0a"; tip = "stopped"; break;
        }
        m_led->setToolTip(tip);
        m_led->setStyleSheet(QString("background: qradialgradient(cx:0.4, cy:0.35, radius:0.75, fx:0.35, fy:0.3,"
                                     " stop:0 %1, stop:0.45 %2, stop:1 %3);"
                                     " border: 1px solid %3; border-radius: 8px;").arg(hi, mid, lo));
    }

    // Slider + number box in one row; the spin box stays the single source of truth.
    QWidget* sliderRow(QSpinBox* s) {
        auto* sl = new QSlider(Qt::Horizontal);
        sl->setRange(s->minimum(), s->maximum());
        sl->setValue(s->value());
        connect(sl, &QSlider::valueChanged, s, &QSpinBox::setValue);
        connect(s, qOverload<int>(&QSpinBox::valueChanged), sl, &QSlider::setValue);
        return rowOf(sl, s);
    }
    QWidget* sliderRow(QDoubleSpinBox* s, int scale = 10) {   // slider in 1/scale steps
        auto* sl = new QSlider(Qt::Horizontal);
        sl->setRange(qRound(s->minimum() * scale), qRound(s->maximum() * scale));
        sl->setValue(qRound(s->value() * scale));
        connect(sl, &QSlider::valueChanged, s, [s, scale](int v) { s->setValue(double(v) / scale); });
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), sl, [sl, scale](double v) { sl->setValue(qRound(v * scale)); });
        return rowOf(sl, s);
    }
    static QWidget* rowOf(QSlider* sl, QWidget* spin) {
        auto* w = new QWidget;
        auto* l = new QHBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        spin->setMinimumWidth(90);
        l->addWidget(sl, 1);
        l->addWidget(spin);
        return w;
    }

    // UDP reachability: the Groovy core answers CMD_GET_STATUS (1 byte, 0x05) from the FPGA.
    // A different source port counts as a new sender for the core, so don't do this mid-playback.
    // Asks the core for its protocol version (CMD_GET_VERSION = 8): the GroovyNLC core answers it without a
    // session and ignores everything else until CMD_INIT. The original Groovy core is recognised by its
    // answer to CMD_GET_STATUS (5). Never sent while streaming: the core replies to the last sender.
    void testConnection() {
        if (m_proc.state() != QProcess::NotRunning || m_jf.active()) {
            m_status->setText(running() || m_jf.active()
                ? "Streaming to the MiSTer right now, so the connection works (Stop first to run the test)"
                : "Stop playback first - the MiSTer takes one sender at a time");
            return;
        }
        const QHostAddress addr(m_host->text().trimmed());
        if (addr.isNull()) { m_status->setText("Test: invalid MiSTer IP"); return; }
        auto ask = [&addr](char cmd, QByteArray* reply) -> qint64 {
            QUdpSocket sock;
            QEventLoop loop;
            QTimer t;
            t.setSingleShot(true);
            QObject::connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
            QObject::connect(&sock, &QUdpSocket::readyRead, &loop, &QEventLoop::quit);
            QElapsedTimer el;
            el.start();
            sock.writeDatagram(QByteArray(1, cmd), addr, 32100);
            t.start(1000);
            loop.exec();
            if (!sock.hasPendingDatagrams()) return -1;
            reply->resize(int(sock.pendingDatagramSize()));
            sock.readDatagram(reply->data(), reply->size());
            return el.elapsed();
        };
        m_status->setText("Testing…");
        QCoreApplication::processEvents();
        QByteArray r;
        qint64 ms = ask(8, &r);
        dbg(QString("test connection: version reply %1 ms, %2 bytes").arg(ms).arg(r.size()));
        if (ms >= 0 && r.size() >= 1 && uchar(r[0]) >= 2) {
            m_status->setText(QString("GroovyNLC core answered in %1 ms (protocol v%2) - ready").arg(ms).arg(uchar(r[0])));
            return;
        }
        r.clear();
        ms = ask(5, &r);
        if (ms >= 0)
            m_status->setText("A Groovy core answered, but not the GroovyNLC core misterMPV needs - choose GroovyNLC "
                              "and press \"Launch core\"");
        else
            m_status->setText("No reply from the MiSTer - core not loaded (try \"Launch core\") or wrong IP?");
    }

    // SSH login for "Launch core" / "Test login". From the Setup fields (test before saving) or the stored file.
    struct SshLogin { QString host, user, pass, key, nlc, orig; int port = 22; };
    SshLogin storedLogin() const {
        SshLogin l;
        l.host = m_login.value("sshHost").toString().trimmed();
        l.user = m_login.value("sshUser").toString().trimmed();
        l.pass = m_login.value("sshPass").toString();
        l.key = m_login.value("sshKey").toString().trimmed();
        l.port = m_login.value("sshPort", 22).toInt();
        l.nlc = m_login.value("corePathNlc").toString().trimmed();
        l.orig = m_login.value("corePathOrig").toString().trimmed();
        return l;
    }
    SshLogin fieldLogin() const {
        SshLogin l;
        l.host = m_sshHost->text().trimmed(); l.user = m_sshUser->text().trimmed(); l.pass = m_sshPass->text();
        l.key = m_sshKey->text().trimmed(); l.port = m_sshPort->value();
        l.nlc = m_coreNlc->text().trimmed(); l.orig = m_coreOrig->text().trimmed();
        return l;
    }
    static QString knownHostsPath() { return configDir() + "/misterMPV-known_hosts"; }
    // Core path patterns go into a remote shell command: allow path and wildcard characters only.
    static bool safePattern(const QString& p) {
        static const QRegularExpression ok("^[A-Za-z0-9_./*?\\[\\]+-]+$");
        return ok.match(p).hasMatch();
    }

    // Runs `remote` on the MiSTer with ssh. The password (if any) reaches ssh through SSH_ASKPASS and only
    // lives in the child's environment; keys and ssh-agent are tried first by ssh itself.
    // The MiSTer's host key is remembered on first contact (accept-new) in our own known_hosts file.
    bool runSsh(const SshLogin& l, const QString& remote, int mode, QLabel* out) {
        if (m_ssh.state() != QProcess::NotRunning) return false;
        const QString host = l.host.isEmpty() ? m_host->text().trimmed() : l.host;
        if (host.isEmpty()) { out->setText("Enter the MiSTer IP first"); return false; }
        if (l.user.isEmpty()) { out->setText("Enter the SSH user in Setup… (a stock MiSTer uses root)"); return false; }
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const QString askpass = dir + QString("/misterMPV-askpass-%1.sh").arg(QCoreApplication::applicationPid());
        QFile f(askpass);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { out->setText("Cannot write askpass helper"); return false; }
        f.write("#!/bin/sh\nprintf '%s\\n' \"$MISTER_SSH_PW\"\n");
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        f.close();
        m_askpass = askpass;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("SSH_ASKPASS", askpass);
        env.insert("SSH_ASKPASS_REQUIRE", "force");
        env.insert("MISTER_SSH_PW", l.pass);
        m_ssh.setProcessEnvironment(env);
        m_ssh.setStandardInputFile(QProcess::nullDevice());
        QStringList a = {"-p", QString::number(l.port), "-o", "StrictHostKeyChecking=accept-new",
                         "-o", "UserKnownHostsFile=" + knownHostsPath(), "-o", "LogLevel=ERROR",
                         "-o", "ConnectTimeout=5"};
        if (!l.key.isEmpty()) {
            QString k = l.key;
            if (k.startsWith("~/")) k = QDir::homePath() + k.mid(1);
            a << "-i" << k << "-o" << "IdentitiesOnly=yes";
        }
        if (l.pass.isEmpty()) a << "-o" << "BatchMode=yes";   // keys only: never hang on a prompt
        a << l.user + "@" + host << remote;
        m_sshMode = mode;
        m_sshOut = out;
        m_ssh.start("ssh", a);
        return true;
    }

    void launchCore() {
        const SshLogin l = storedLogin();
        const bool nlc = m_core->currentIndex() == 0;
        QString pat = nlc ? l.nlc : l.orig;
        if (pat.isEmpty()) pat = nlc ? kDefaultNlcCore : kDefaultOrigCore;
        if (!safePattern(pat)) { m_status->setText("Core path contains unsupported characters (Setup…)"); return; }
        const QString remote =
            "f=$(ls -t " + pat + " 2>/dev/null | head -1); [ -n \"$f\" ] || exit 3; "
            "echo \"load_core $f\" > /dev/MiSTer_cmd";
        if (runSsh(l, remote, 0, m_status)) m_status->setText("Loading Groovy core on the MiSTer…");
    }
    // Setup… → Test login: log in with the fields as they are and show which core files would be loaded.
    void testLogin() {
        const SshLogin l = fieldLogin();
        const QString nlc = l.nlc.isEmpty() ? kDefaultNlcCore : l.nlc;
        const QString orig = l.orig.isEmpty() ? kDefaultOrigCore : l.orig;
        if (!safePattern(nlc) || !safePattern(orig)) { m_loginStatus->setText("A core path contains unsupported characters"); return; }
        const QString remote = "echo \"NLC: $(ls -t " + nlc + " 2>/dev/null | head -1)\"; "
                               "echo \"Groovy: $(ls -t " + orig + " 2>/dev/null | head -1)\"";
        if (runSsh(l, remote, 1, m_loginStatus)) m_loginStatus->setText("Logging in…");
    }
    void onSshFinished(int code) {
        if (!m_askpass.isEmpty()) { QFile::remove(m_askpass); m_askpass.clear(); }
        QLabel* out = m_sshOut ? m_sshOut : m_status;
        const QString err = QString::fromUtf8(m_ssh.readAllStandardError()).trimmed();
        if (err.contains("IDENTIFICATION HAS CHANGED") || err.contains("Host key verification failed")) {
            out->setText("The MiSTer's SSH host key has changed (new SD card or reinstall?). If that is expected, "
                         "delete " + knownHostsPath() + " and try again.");
            return;
        }
        if (code == 255 || (code != 0 && code != 3)) {
            out->setText("SSH failed (" + QString::number(code) + "): " +
                         (err.isEmpty() ? QString("check host, user, password or key") : err.left(160)));
            return;
        }
        if (m_sshMode == 1) {
            QStringList lines;
            for (QString ln : QString::fromUtf8(m_ssh.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
                if (ln.trimmed().endsWith(':')) ln = ln.trimmed() + " (no file found)";
                lines << ln.trimmed();
            }
            out->setText("Login OK.\n" + lines.join('\n'));
            return;
        }
        if (code == 0)
            out->setText((m_core->currentIndex() == 0 ? "GroovyNLC" : "Groovy") + QString(" core loading - give it a few seconds, then \"Test connection\""));
        else
            out->setText("No core file found on the MiSTer (check the core path in Setup…)");
    }
    static inline const QString kDefaultNlcCore = "/media/fat/_Utility/GroovyNLC_*.rbf";
    static inline const QString kDefaultOrigCore = "/media/fat/_Utility/Groovy_*.rbf";

    // Persistent "Setup" dialog: rarely changed things live here instead of the main window.
    // Live widgets (OSD size, subtitle colour, ...) apply immediately; the rest on Save.
    void buildSetupDialog() {
        m_setupDlg = new QDialog(this);
        m_setupDlg->setWindowTitle("misterMPV setup");
        auto* lay = new QVBoxLayout(m_setupDlg);

        auto* disp = new QGroupBox("Display");
        auto* df = new QFormLayout(disp);
        df->addRow("OSD text size:", sliderRow(m_osdSize));
        df->addRow("Subtitle colour:", m_subColor);
        df->addRow("", m_subBars);
        m_colorOrder = new QComboBox;
        m_colorOrder->addItems({"BGR (default - what the Groovy core expects)", "RGB"});
        m_colorOrder->setToolTip("Channel order sent to the MiSTer. If skin tones look blue (or red tinted), "
                                 "flip it. Applies on the next Play.");
        df->addRow("Colour order:", m_colorOrder);
        df->addRow("", m_trackInfo);
        df->addRow("Artist / title position:", m_trackPos);
        m_theme = new QComboBox;
        for (const Theme& t : kThemes) m_theme->addItem(t.name);
        m_theme->setCurrentText(m_set.value("theme", "System").toString());
        m_theme->setToolTip("Colours of this window. System follows your desktop; the others look the same everywhere.");
        connect(m_theme, &QComboBox::currentTextChanged, this, [this](const QString& n) {
            m_set.setValue("theme", n);
            applyTheme(n);
        });
        df->addRow("Theme:", m_theme);
        auto* geoBtn = new QPushButton("CRT geometry…");
        geoBtn->setToolTip("Picture size and position on the CRT, test pattern, and the per-modeline sync "
                           "fine-tune (also under Picture → CRT calibration…)");
        connect(geoBtn, &QPushButton::clicked, m_setupDlg, [this] {
            m_setupDlg->accept();   // saves Setup, then the (non-modal) calibration dialog opens
            m_calDlg->show(); m_calDlg->raise();
        });
        df->addRow("Picture on the CRT:", geoBtn);
        lay->addWidget(disp);

        auto* ctl = new QGroupBox("Control");
        auto* cl = new QVBoxLayout(ctl);
        cl->addWidget(m_pad);
        lay->addWidget(ctl);

        auto* jf = new QGroupBox("Jellyfin receiver");
        auto* jl = new QFormLayout(jf);
        m_jfOn = new QCheckBox("Receive casts from Jellyfin (also SyncPlay)");
        m_jfOn->setToolTip("Runs a private jellyfin-mpv-shim (own login, own device) that plays through this "
                           "MiSTer while misterMPV is open. Idle it sends nothing to the MiSTer; local Play "
                           "stops a running cast and a new cast stops local playback.");
        m_jfName = new QLineEdit;
        m_jfName->setToolTip("Device name shown in Jellyfin's cast / SyncPlay menu");
        m_jfServer = new QLineEdit;
        m_jfServer->setPlaceholderText("http://server:8096");
        auto* jfLogin = new QPushButton("Sign in (Quick Connect)…");
        jfLogin->setToolTip("Shows a code here; enter it in Jellyfin: user menu → Quick Connect");
        m_jfStatus = new QLabel;
        m_jfStatus->setWordWrap(true);
        m_jfStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* jfLoginRow = new QHBoxLayout;
        jfLoginRow->addWidget(m_jfServer, 1);
        jfLoginRow->addWidget(jfLogin);
        jl->addRow("", m_jfOn);
        jl->addRow("Device name:", m_jfName);
        jl->addRow("Server:", jfLoginRow);
        jl->addRow("State:", m_jfStatus);
        lay->addWidget(jf);
        connect(jfLogin, &QPushButton::clicked, m_setupDlg, [this] {
            const QString srv = m_jfServer->text().trimmed();
            if (srv.isEmpty()) { m_jfStatus->setText("Enter the server address first"); return; }
            m_set.setValue("jfServer", srv);
            m_set.setValue("jfName", m_jfName->text().trimmed());
            m_jfOn->setChecked(true);
            m_set.setValue("jfOn", true);
            m_jf.configure(mpvPath(), m_jfName->text(), outputOptions(), shimSettings());
            m_jf.signIn(srv);
        });

        auto* ssh = new QGroupBox("MiSTer login (for \"Launch core\")");
        auto* sf = new QFormLayout(ssh);
        m_sshHost = new QLineEdit;
        m_sshHost->setPlaceholderText("empty = the MiSTer IP from the main window");
        m_sshUser = new QLineEdit;
        m_sshUser->setPlaceholderText("root on a stock MiSTer");
        m_sshPass = new QLineEdit;
        m_sshPass->setEchoMode(QLineEdit::Password);
        m_sshPass->setPlaceholderText("1 on a stock MiSTer; empty if you use a key");
        m_sshKey = new QLineEdit;
        m_sshKey->setPlaceholderText("optional, e.g. ~/.ssh/id_ed25519");
        auto* keyBrowse = new QPushButton("…");
        keyBrowse->setFixedWidth(32);
        connect(keyBrowse, &QPushButton::clicked, m_setupDlg, [this] {
            const QString f = QFileDialog::getOpenFileName(m_setupDlg, "SSH private key", QDir::homePath() + "/.ssh");
            if (!f.isEmpty()) m_sshKey->setText(f);
        });
        auto* keyRow = new QHBoxLayout;
        keyRow->addWidget(m_sshKey, 1);
        keyRow->addWidget(keyBrowse);
        m_sshPort = new QSpinBox;
        m_sshPort->setRange(1, 65535);
        m_coreNlc = new QLineEdit;
        m_coreNlc->setPlaceholderText(kDefaultNlcCore + " (used when empty)");
        m_coreNlc->setToolTip("File (or wildcard) of the GroovyNLC core on the MiSTer; the newest match is loaded.");
        m_coreOrig = new QLineEdit;
        m_coreOrig->setPlaceholderText(kDefaultOrigCore + " (used when empty)");
        m_coreOrig->setToolTip("File (or wildcard) of the original Groovy core; the newest match is loaded.");
        auto* testLoginBtn = new QPushButton("Test login");
        testLoginBtn->setToolTip("Logs in with these values and shows which core files \"Launch core\" would load");
        connect(testLoginBtn, &QPushButton::clicked, m_setupDlg, [this] { testLogin(); });
        m_loginStatus = new QLabel;
        m_loginStatus->setWordWrap(true);
        m_loginStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
        sf->addRow("SSH host:", m_sshHost);
        sf->addRow("SSH user:", m_sshUser);
        sf->addRow("SSH password:", m_sshPass);
        sf->addRow("SSH key file:", keyRow);
        sf->addRow("SSH port:", m_sshPort);
        sf->addRow("GroovyNLC core:", m_coreNlc);
        sf->addRow("Groovy core (original):", m_coreOrig);
        sf->addRow(testLoginBtn, m_loginStatus);
        auto* note = new QLabel("Stored only on this PC, readable only by you:\n" +
                                loginPath().replace(QDir::homePath(), "~"));
        note->setWordWrap(true);
        sf->addRow(note);
        lay->addWidget(ssh);

        auto* btns = new QHBoxLayout;
        auto* defaults = new QPushButton("Defaults");
        auto* ok = new QPushButton("Save");
        btns->addWidget(defaults);
        btns->addStretch(1);
        btns->addWidget(ok);
        lay->addLayout(btns);
        connect(defaults, &QPushButton::clicked, m_setupDlg, [this] {
            m_colorOrder->setCurrentIndex(0); m_theme->setCurrentText("System");
            for (QLineEdit* e : {m_sshHost, m_sshUser, m_sshPass, m_sshKey, m_coreNlc, m_coreOrig}) e->clear();
            m_sshPort->setValue(22);
            m_osdSize->setValue(28); m_trackInfo->setChecked(false); m_trackPos->setCurrentIndex(0); m_subColor->setCurrentIndex(0); m_subBars->setChecked(true); m_pad->setChecked(true);
        });
        connect(ok, &QPushButton::clicked, m_setupDlg, &QDialog::accept);
        connect(m_setupDlg, &QDialog::accepted, this, [this] {
            m_set.setValue("colorOrder", m_colorOrder->currentIndex());
            m_login.setValue("sshHost", m_sshHost->text().trimmed());
            m_login.setValue("sshUser", m_sshUser->text().trimmed());
            m_login.setValue("sshPass", m_sshPass->text());
            m_login.setValue("sshKey", m_sshKey->text().trimmed());
            m_login.setValue("sshPort", m_sshPort->value());
            m_login.setValue("corePathNlc", m_coreNlc->text().trimmed());
            m_login.setValue("corePathOrig", m_coreOrig->text().trimmed());
            saveLogin();
            m_set.setValue("pad", m_pad->isChecked());
            const QString name = m_jfName->text().trimmed();
            const bool renamed = name != m_set.value("jfName", "MiSTer").toString();
            m_set.setValue("jfOn", m_jfOn->isChecked());
            m_set.setValue("jfName", name);
            m_set.setValue("jfServer", m_jfServer->text().trimmed());
            if (renamed) m_jf.setEnabled(false);   // the name is read at shim start
            jfApplySetup();
            jfSync();
        });
        m_setupDlg->setMinimumWidth(540);
    }

    void sshDialog() {   // "Setup…" button: reload the stored values, then show
        m_colorOrder->setCurrentIndex(m_set.value("colorOrder", 0).toInt());
        const SshLogin l = storedLogin();
        m_sshHost->setText(l.host); m_sshUser->setText(l.user); m_sshPass->setText(l.pass);
        m_sshKey->setText(l.key); m_sshPort->setValue(l.port);
        m_coreNlc->setText(l.nlc); m_coreOrig->setText(l.orig);
        m_loginStatus->clear();
        m_jfOn->setChecked(m_set.value("jfOn", false).toBool());
        m_jfName->setText(m_set.value("jfName", "MiSTer").toString());
        QString srv = m_set.value("jfServer").toString();
        if (srv.isEmpty()) srv = JellyfinReceiver::mainShimServer();   // same server as the normal shim
        m_jfServer->setText(srv);
        m_setupDlg->exec();
    }

    // Winamp-style "Sort List". Stopped: reorder the local list. Running: mpv owns the order, so
    // turn the wanted order into playlist-move commands (the current entry keeps playing);
    // mpv reports the result back and the debounced refresh redraws the list.
    // Column header click: toggle ascending/descending on the same column.
    void sortByColumn(int c) {
        if (c < 1 || c > 4) return;
        m_sortAsc = (c == m_sortCol) ? !m_sortAsc : true;
        m_sortCol = c;
        m_list->header()->setSortIndicator(c, m_sortAsc ? Qt::AscendingOrder : Qt::DescendingOrder);
        sortPlaylist(c == 1 ? "artist" : c == 2 ? "title" : c == 3 ? "album" : "time", m_sortAsc);
    }
    void sortPlaylist(const QString& key, bool asc = true) {
        const int n = m_list->count();
        if (n < 2) return;
        if (key == "random" && running()) { cmd({"playlist-shuffle"}); return; }
        struct E { int idx; QString path, name, artist, album, dir; QDateTime mtime; bool local; double dur; };
        QVector<E> es;
        for (int i = 0; i < n; i++) {
            E e;
            e.idx = i;
            e.path = m_list->item(i)->data(0, Qt::UserRole).toString();
            const QFileInfo fi(e.path);
            e.local = fi.exists();
            const QString lbl = m_list->item(i)->data(0, R_TITLE).toString();
            e.name = !lbl.isEmpty() ? lbl : (e.local ? fi.fileName() : e.path);
            e.artist = m_list->item(i)->data(0, R_ARTIST).toString();
            e.album = m_list->item(i)->data(0, R_ALBUM).toString();
            e.dur = m_list->item(i)->data(0, R_DUR).toDouble();
            e.dir = e.local ? fi.absolutePath() : QString();
            if (e.local) e.mtime = fi.lastModified();
            es << e;
        }
        QCollator coll;
        coll.setNumericMode(true);
        coll.setCaseSensitivity(Qt::CaseInsensitive);
        auto byName = [&](const E& a, const E& b) { return coll.compare(a.name, b.name) < 0; };
        if (key == "title") { std::stable_sort(es.begin(), es.end(), byName); if (!asc) std::reverse(es.begin(), es.end()); }
        else if (key == "artist") {
            std::stable_sort(es.begin(), es.end(), [&](const E& a, const E& b) {
                if (a.artist.isEmpty() != b.artist.isEmpty()) return !a.artist.isEmpty();   // untagged last
                const int c = coll.compare(a.artist, b.artist);
                return c != 0 ? c < 0 : byName(a, b);
            });
            if (!asc) std::reverse(es.begin(), es.end());
        } else if (key == "album") {
            std::stable_sort(es.begin(), es.end(), [&](const E& a, const E& b) {
                if (a.album.isEmpty() != b.album.isEmpty()) return !a.album.isEmpty();
                const int c = coll.compare(a.album, b.album);
                return c != 0 ? c < 0 : byName(a, b);
            });
            if (!asc) std::reverse(es.begin(), es.end());
        } else if (key == "time") {
            std::stable_sort(es.begin(), es.end(), [&](const E& a, const E& b) {
                if ((a.dur > 0) != (b.dur > 0)) return a.dur > 0;                          // unknown last
                return a.dur < b.dur;
            });
            if (!asc) std::reverse(es.begin(), es.end());
        }
        else if (key == "folder")
            std::stable_sort(es.begin(), es.end(), [&](const E& a, const E& b) {
                if (a.local != b.local) return a.local;            // streams/URLs go last
                const int c = coll.compare(a.dir, b.dir);
                return c != 0 ? c < 0 : byName(a, b);
            });
        else if (key == "dateold" || key == "datenew") {
            const bool newest = key == "datenew";
            std::stable_sort(es.begin(), es.end(), [&](const E& a, const E& b) {
                if (a.local != b.local) return a.local;            // streams/URLs go last
                return newest ? a.mtime > b.mtime : a.mtime < b.mtime;
            });
        } else if (key == "reverse") std::reverse(es.begin(), es.end());
        else if (key == "random") {
            std::mt19937 rng(QRandomGenerator::global()->generate());
            std::shuffle(es.begin(), es.end(), rng);
        }
        QVector<int> order;
        for (const E& e : es) order << e.idx;
        applyOrder(order);
    }

    // Bring the playlist into `order` (a permutation of the current row indices). Stopped: rebuild the
    // rows; running: mpv owns the order, so issue playlist-move commands (the playing entry keeps playing).
    void applyOrder(const QVector<int>& order) {
        const int n = m_list->count();
        if (order.size() != n) return;
        if (!running()) {
            QStringList paths;
            for (int i : order) paths << m_list->item(i)->data(0, Qt::UserRole).toString();
            const QSignalBlocker b(m_list);
            m_list->clear();
            for (const QString& pth : paths) addLocal(pth);
            renumber();
            savePlaylist();
            return;
        }
        QVector<int> cur(n);
        for (int i = 0; i < n; i++) cur[i] = i;
        for (int i = 0; i < n; i++) {
            const int want = order[i];
            const int j = cur.indexOf(want);
            if (j != i) {
                cmd(QJsonArray{"playlist-move", j, i});
                cur.remove(j);
                cur.insert(i, want);
            }
        }
    }

    // ---- right-click menu on the playlist --------------------------------------------------------
    void playlistMenu(const QPoint& pos) {
        QTreeWidgetItem* hit = m_list->itemAt(pos);
        if (!hit) return;
        if (!hit->isSelected()) { m_list->clearSelection(); hit->setSelected(true); }
        QList<int> rows;
        for (auto* it : m_list->selectedItems()) rows << m_list->row(it);
        std::sort(rows.begin(), rows.end());
        const QString path = hit->data(0, Qt::UserRole).toString();
        QMenu menu(this);
        menu.addAction("Play now", this, [this, hit] {
            const int r = m_list->row(hit);
            if (running()) cmd(QJsonArray{"set_property", "playlist-pos", r}); else start(r);
        });
        menu.addAction("Play next", this, [this, rows] { moveRows(rows, false); });
        menu.addAction("Move to end", this, [this, rows] { moveRows(rows, true); });
        menu.addSeparator();
        menu.addAction("Remove", this, [this] { removeSelected(); });
        menu.addSeparator();
        if (!path.contains("://")) {
            menu.addAction("Open containing folder", this, [path] {
                QProcess::startDetached("xdg-open", {QFileInfo(path).absolutePath()});
            });
        }
        menu.addAction("Copy path / URL", this, [path] { QApplication::clipboard()->setText(path); });
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    }
    // "Play next": put the rows right behind the playing one (or at the top when nothing plays);
    // "Move to end": append them. Their relative order is kept.
    void moveRows(const QList<int>& rows, bool toEnd) {
        const int n = m_list->count();
        if (rows.isEmpty() || n < 2) return;
        QVector<int> rest, moved;
        for (int i = 0; i < n; i++) (rows.contains(i) ? moved : rest) << i;
        int insertAt = rest.size();
        if (!toEnd) {
            insertAt = 0;                                  // nothing playing: front of the list
            if (m_curRow >= 0 && running()) {
                const int pos = rest.indexOf(m_curRow);    // playing row stays where it is in `rest`
                insertAt = pos >= 0 ? pos + 1 : 0;
            }
        }
        QVector<int> order = rest;
        for (int k = 0; k < moved.size(); k++) order.insert(insertAt + k, moved[k]);
        applyOrder(order);
    }

    // Winamp-style "add dir": all media files below the folder, natural order (01, 02, ..., 10).
    void addFolder(const QString& path) {
        const QString root = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        if (!m_libRoots.contains(root)) {
            m_libRoots << root;
            m_set.setValue("libRoots", m_libRoots);
        }
        static const QStringList filters = {
            "*.mkv", "*.mp4", "*.avi", "*.mov", "*.webm", "*.mpg", "*.mpeg", "*.ts", "*.m2ts", "*.wmv",
            "*.flv", "*.m4v", "*.ogv", "*.vob", "*.mp3", "*.flac", "*.ogg", "*.opus", "*.wav", "*.m4a",
            "*.aac", "*.wma", "*.ape", "*.mka"};
        QStringList files;
        QDirIterator it(path, filters, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
        while (it.hasNext() && files.size() < 5000) files << it.next();
        QCollator coll;
        coll.setNumericMode(true);
        coll.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(files.begin(), files.end(), [&](const QString& a, const QString& b) { return coll.compare(a, b) < 0; });
        for (const QString& f : files) addEntry(f);
        m_status->setText(QString("Added %1 files from %2").arg(files.size()).arg(QFileInfo(path).fileName()));
    }

    bool running() const { return m_sock.state() == QLocalSocket::ConnectedState; }

    // ---- playlist rows: # | Artist | Title | Time ---------------------------------------------------
    struct Tag { QString artist, title, album; double dur = -1; };
    static constexpr int R_ARTIST = Qt::UserRole + 1, R_TITLE = Qt::UserRole + 2, R_DUR = Qt::UserRole + 3,
                         R_ALBUM = Qt::UserRole + 4;

    static QString fmtDur(double sec) {
        const qint64 t = qint64(sec + 0.5);
        if (t >= 3600)
            return QString("%1:%2:%3").arg(t / 3600).arg(t / 60 % 60, 2, 10, QChar('0')).arg(t % 60, 2, 10, QChar('0'));
        return QString("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
    }
    static const QStringList& mediaExts() {
        static const QStringList e = {"mkv", "mp4", "avi", "mov", "webm", "mpg", "mpeg", "ts", "m2ts", "wmv", "flv",
                                      "m4v", "ogv", "vob", "mp3", "flac", "ogg", "opus", "wav", "m4a", "aac", "wma",
                                      "ape", "mka"};
        return e;
    }

    static bool isAudioFile(const QString& path) {
        static const QStringList a = {"mp3", "flac", "ogg", "opus", "wav", "m4a", "aac", "wma", "ape", "mka"};
        return a.contains(QFileInfo(path).suffix().toLower());
    }
    // No artist tag: Artist/Album/track.mp3 -> the folder above the album folder; a file straight in
    // Artist/ -> that folder. Generic folders (Music, Downloads, ...) never count as an artist.
    QString folderArtist(const QString& path) const {
        static const QStringList generic = {"music", "musik", "audio", "mp3", "mp3s", "songs", "downloads", "media",
                                            "medien", "sounds", "library", "home", "mnt", "srv", "data", "tmp",
                                            "youtube", "videos", "video", "music video", "music videos", "playlist", "playlists"};
        auto ok = [&](const QString& n) { return !n.isEmpty() && n != "." && !generic.contains(n.toLower()); };
        QDir parent = QFileInfo(path).dir();
        const QString parentName = parent.dirName();
        QDir grand = parent;
        const bool up = grand.cdUp();
        const QString grandName = up ? grand.dirName() : QString();
        // the folder the user added is the library root, never an artist
        const bool grandIsRoot = up && m_libRoots.contains(QDir::cleanPath(grand.absolutePath()));
        if (ok(grandName) && !grandIsRoot) return grandName;
        return ok(parentName) ? parentName : QString();
    }

    void fillItem(QTreeWidgetItem* it) {
        const QString path = it->data(0, Qt::UserRole).toString();
        const Tag t = m_tags.value(path);
        QString artist = t.artist;
        const bool fromFolder = artist.isEmpty() && m_tags.contains(path) && isAudioFile(path);
        if (fromFolder) artist = folderArtist(path);
        QString album = t.album;
        if (album.isEmpty() && m_tags.contains(path) && isAudioFile(path)) {
            const QString parent = QFileInfo(path).dir().dirName();
            if (!parent.isEmpty() && !m_libRoots.contains(QDir::cleanPath(QFileInfo(path).dir().absolutePath())))
                album = parent;   // Artist/Album/track: the folder is the album
        }
        QString title = t.title;
        if (title.isEmpty()) title = path.contains("://") ? path : QFileInfo(path).fileName();
        it->setText(1, artist);
        it->setToolTip(1, fromFolder && !artist.isEmpty() ? "no artist tag - taken from the folder name" : QString());
        it->setText(2, title);
        it->setText(3, album);
        it->setText(4, t.dur > 0 ? fmtDur(t.dur) : QString());
        it->setData(0, R_ARTIST, artist);
        it->setData(0, R_TITLE, title);
        it->setData(0, R_DUR, t.dur);
        it->setData(0, R_ALBUM, album);
        it->setToolTip(2, path);
    }
    void addLocal(const QString& f) {
        auto* it = new QTreeWidgetItem;
        it->setData(0, Qt::UserRole, f);
        it->setFlags(it->flags() & ~Qt::ItemIsDropEnabled);   // drops go between rows, never onto one
        it->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
        it->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        it->setText(0, QString::number(m_list->count() + 1));
        fillItem(it);
        m_list->addItem(it);
        queueTags(f);
    }
    void setRowBold(QTreeWidgetItem* it, bool bold) {
        for (int c = 0; c < 5; c++) {
            QFont f = it->font(c);
            f.setBold(bold);
            it->setFont(c, f);
        }
    }
    // Row numbers (with ▶ on the playing row) and the group title with count and total time.
    void renumber() {
        double total = 0;
        int known = 0;
        for (int i = 0; i < m_list->count(); i++) {
            auto* it = m_list->item(i);
            it->setText(0, (i == m_curRow ? QString("▶ ") : QString()) + QString::number(i + 1));
            const double d = it->data(0, R_DUR).toDouble();
            if (d > 0) { total += d; known++; }
        }
        if (m_plBox)
            m_plBox->setTitle(QString("Playlist  ·  %1 tracks").arg(m_list->count()) +
                              (known ? "  ·  " + fmtDur(total) : QString()));
    }

    // ---- tags (artist / title / duration via ffprobe), read in the background and cached ---------
    QString tagCachePath() const {
        QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
        return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/misterMPV-tags2.json";
    }
    void loadTags() {
        QFile f(tagCachePath());
        if (!f.open(QIODevice::ReadOnly)) return;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        for (auto it = o.begin(); it != o.end(); ++it) {
            const QJsonObject t = it.value().toObject();
            m_tags.insert(it.key(), Tag{t.value("a").toString(), t.value("t").toString(), t.value("al").toString(), t.value("d").toDouble(-1)});
        }
    }
    void saveTags() {
        QJsonObject o;
        for (auto it = m_tags.begin(); it != m_tags.end(); ++it)
            o.insert(it.key(), QJsonObject{{"a", it->artist}, {"t", it->title}, {"al", it->album}, {"d", it->dur}});
        QFile f(tagCachePath());
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
    // While nothing plays, the status line tells that tags are still being read.
    void showTagProgress() {
        if (!m_status || m_proc.state() != QProcess::NotRunning) return;
        const int left = m_tagQueue.size() + m_tagRunning;
        m_status->setText(left > 0 ? QString("Reading tags… %1 left").arg(left) : QString("stopped"));
    }
    bool inList(const QString& path) const {
        for (int i = 0; i < m_list->count(); i++)
            if (m_list->item(i)->data(0, Qt::UserRole).toString() == path) return true;
        return false;
    }
    void queueTags(const QString& path) {
        if (path.contains("://") || m_tags.contains(path) || m_tagQueued.contains(path)) return;
        if (!mediaExts().contains(QFileInfo(path).suffix().toLower())) return;
        m_tagQueued.insert(path);
        m_tagQueue << path;
        if (m_tagQueue.size() % 25 == 1) showTagProgress();
        pumpTags();
    }
    void pumpTags() {
        while (m_tagRunning < 4 && !m_tagQueue.isEmpty()) {
            const QString path = m_tagQueue.takeFirst();
            m_tagQueued.remove(path);
            if (!inList(path)) continue;   // removed/cleared while waiting
            auto* pr = new QProcess(this);
            m_tagRunning++;
            connect(pr, &QProcess::finished, this, [this, pr, path] {
                const QJsonObject fmt = QJsonDocument::fromJson(pr->readAllStandardOutput()).object().value("format").toObject();
                const QJsonObject tags = fmt.value("tags").toObject();
                auto get = [&](const char* k) {
                    for (auto it = tags.begin(); it != tags.end(); ++it)
                        if (it.key().compare(k, Qt::CaseInsensitive) == 0) return it.value().toString().trimmed();
                    return QString();
                };
                Tag t;
                t.artist = get("artist");
                if (t.artist.isEmpty()) t.artist = get("album_artist");
                t.title = get("title");
                t.album = get("album");
                t.dur = fmt.value("duration").toString().toDouble();
                if (t.dur <= 0) t.dur = -1;
                m_tags.insert(path, t);   // even an empty Tag: do not probe again
                for (int i = 0; i < m_list->count(); i++) {
                    auto* it = m_list->item(i);
                    if (it->data(0, Qt::UserRole).toString() == path) fillItem(it);
                }
                pr->deleteLater();
                m_tagRunning--;
                if ((++m_tagDone % 40) == 0 || (m_tagQueue.isEmpty() && m_tagRunning == 0)) { saveTags(); renumber(); }
                showTagProgress();
                pumpTags();
            });
            connect(pr, &QProcess::errorOccurred, this, [this, pr, path](QProcess::ProcessError e) {
                if (e != QProcess::FailedToStart) return;
                m_tags.insert(path, Tag());
                pr->deleteLater();
                m_tagRunning--;
            });
            pr->start("ffprobe", {"-v", "error", "-show_entries",
                                  "format=duration:format_tags=artist,title,album_artist,album", "-of", "json", path});
        }
    }

    void savePlaylist() {
        QStringList l;
        for (int i = 0; i < m_list->count(); i++) l << m_list->item(i)->data(0, Qt::UserRole).toString();
        m_set.setValue("playlist", l);
    }
    // While mpv runs it owns the list (it reports back via the "playlist" property).
    // URL box / network share: folders are scanned, YouTube links get a yt-dlp check.
    bool addLocation(const QString& f) {
        const QUrl u(f);
        const QString scheme = u.scheme().toLower();
        if ((scheme == "http" || scheme == "https") && needsYtdl(u) && !haveYtdl()) {
            QMessageBox::warning(this, "yt-dlp not found",
                "YouTube links are opened through yt-dlp, which is not installed (or not on the PATH).\n\n"
                "Install it from your distribution (e.g. 'pacman -S yt-dlp', 'apt install yt-dlp', "
                "'dnf install yt-dlp') or with 'pipx install yt-dlp', then add the link again.");
            m_status->setText("yt-dlp missing - YouTube link not added");
            return false;
        }
        const bool share = QStringList{"smb", "sftp", "ftp", "davs", "webdav"}.contains(scheme);
        const QString loc = resolveLocation(f);
        if (share && loc == f) {
            QMessageBox::warning(this, "Network share not available",
                "misterMPV could not mount " + f + " through kio-fuse.\n\n" + kioFuseHint());
            return false;
        }
        if (QFileInfo(loc).isDir()) addFolder(loc); else addEntry(loc);
        return true;
    }
    static bool needsYtdl(const QUrl& u) {
        const QString h = u.host().toLower();
        return h == "youtu.be" || h.endsWith("youtube.com") || h.endsWith("youtube-nocookie.com");
    }
    static bool haveYtdl() {
        return !QStandardPaths::findExecutable("yt-dlp").isEmpty() ||
               !QStandardPaths::findExecutable("youtube-dl").isEmpty();
    }
    static bool haveKioFuse() {
        auto* bus = QDBusConnection::sessionBus().interface();
        if (!bus) return false;
        if (bus->isServiceRegistered("org.kde.KIOFuse")) return true;
        const QDBusReply<QStringList> act = bus->call("ListActivatableNames");
        return act.isValid() && act.value().contains("org.kde.KIOFuse");
    }
    static QString kioFuseHint() {
        return haveKioFuse()
            ? QString("kio-fuse is running; check the address and that you are allowed to open the share "
                      "(KDE asks for the password once and keeps it in KWallet).")
            : QString("kio-fuse is not installed or not running (package 'kio-fuse', part of KDE). Without it, "
                      "mount the share with your system's tools (e.g. 'gio mount smb://server/share' or a CIFS "
                      "entry in /etc/fstab) and add the mounted folder with + Folder. See the README.");
    }
    void addNetworkShare() {
        if (!haveKioFuse()) {
            QMessageBox::information(this, "Network share", kioFuseHint());
            return;
        }
        bool ok = false;
        const QString url = QInputDialog::getText(this, "Add network share",
            "SMB address of a folder or file, for example\n"
            "smb://server/Videos/Films   or   smb://user@server/Videos/film.mkv\n"
            "(never put a password here; KDE asks for it and keeps it in KWallet):",
            QLineEdit::Normal, m_set.value("lastShare", "smb://").toString(), &ok).trimmed();
        if (!ok || url.isEmpty() || url == "smb://") return;
        if (!QUrl(url).password().isEmpty()) {
            QMessageBox::warning(this, "Network share", "Please leave the password out of the address.");
            return;
        }
        m_set.setValue("lastShare", url);
        m_status->setText("Mounting " + url + " …");
        if (addLocation(url)) m_status->setText("Added " + url);
    }

    void addEntry(const QString& f) {
        if (running()) cmd({"loadfile", f, "append"}); else addLocal(f);
        savePlaylist();
    }
    void removeSelected() {
        QList<int> rows;
        for (auto* it : m_list->selectedItems()) rows << m_list->row(it);
        std::sort(rows.begin(), rows.end(), std::greater<int>());   // highest first keeps indices valid
        for (int r : rows) removeEntry(r);
        if (!running()) renumber();
        savePlaylist();
    }
    void removeEntry(int row) {
        if (running()) cmd(QJsonArray{"playlist-remove", row}); else delete m_list->takeItem(row);
    }
    void showPlaylist(const QJsonArray& pl) {
        if (!running()) return;   // late event while mpv shuts down: keep the stopped list as it is
        // Never rebuild the list while the user is dragging/pressing in it: Qt's drag loop still
        // holds pointers to the items and clear() would free them under it.
        if ((QApplication::mouseButtons() & Qt::LeftButton) && m_list->underMouse()) {
            m_pendingPl = pl;
            m_hasPendingPl = true;
            QTimer::singleShot(200, this, [this] {
                if (m_hasPendingPl) { m_hasPendingPl = false; showPlaylist(m_pendingPl); }
            });
            return;
        }
        m_hasPendingPl = false;
        const QSignalBlocker b(m_list);
        m_syncing = true;
        m_list->clear();
        int cur = -1;
        for (int i = 0; i < pl.size(); i++) {
            const QJsonObject o = pl[i].toObject();
            const QString f = o.value("filename").toString();
            addLocal(f);
            auto* it = m_list->item(i);
            const QString title = o.value("title").toString();
            if (!title.isEmpty() && m_tags.value(f).title.isEmpty()) {   // streams: title from mpv
                it->setText(2, title);
                it->setData(0, R_TITLE, title);
            }
            if (o.value("current").toBool()) cur = i;
        }
        m_curRow = cur;
        for (int i = 0; i < m_list->count(); i++) setRowBold(m_list->item(i), i == cur);
        renumber();
        if (cur >= 0) m_list->scrollToItem(m_list->item(cur));
        m_syncing = false;
        savePlaylist();
    }

    void start(int startRow = -1) {
        dbg(QString("start(row %1) proc=%2 connected=%3").arg(startRow).arg(int(m_proc.state())).arg(running()));
        // mpv already starting or running (e.g. a second click before the IPC socket is up): never start a
        // second mpv; it would remove the socket of the running one and leave the window without control.
        if (m_proc.state() != QProcess::NotRunning) {
            if (startRow >= 0 && running()) cmd(QJsonArray{"set_property", "playlist-pos", startRow});
            return;
        }
        if (m_list->count() == 0 && !m_file->text().trimmed().isEmpty()) {
            addLocal(m_file->text().trimmed());
            m_file->clear();
        }
        if (m_list->count() == 0) return;
        if (m_host->text().trimmed().isEmpty()) { m_status->setText("Enter the MiSTer IP first"); return; }
        savePlaylist();
        m_set.setValue("host", m_host->text());
        m_set.setValue("modeline", m_modeline->currentText());
        m_set.setValue("near", m_near->currentIndex());
        m_set.setValue("fieldOrder", m_field->currentIndex());
        m_set.setValue("pad", m_pad->isChecked());
        m_set.setValue("flicker", m_flicker->currentIndex());
        if (m_jf.active()) {   // one sender at a time: the local list wins over a running cast
            m_jf.stopPlayback();
            m_status->setText("Jellyfin playback stopped - playing locally");
        }
        m_sockPath = QDir::temp().filePath(QString("mpv-mister-%1.sock").arg(QCoreApplication::applicationPid()));
        QFile::remove(m_sockPath);
        QStringList a = {"--no-config", "--vo=mister", "--ao=mister",
                         "--input-ipc-server=" + m_sockPath, "--no-terminal", "--idle=no"};
        for (const QString& o : outputOptions()) a << "--" + o;
        if (m_repeat->currentIndex() == 1) a << "--loop-file=inf";
        if (m_repeat->currentIndex() == 2) a << "--loop-playlist=inf";
        a << "--volume=" + QString::number(m_vol->value());
        a << "--script-opts=mister-shuffle=" + QString(m_shuffle->isChecked() ? "yes" : "no") +
             ",mister-vis-mode=" + m_vis->currentData().toString() +
             ",mister-track=" + QString(m_trackInfo->isChecked() ? "yes" : "no") +
             ",mister-track-pos=" + QString(m_trackPos->currentIndex() ? "top" : "bottom");
        if (startRow < 0) startRow = m_list->currentRow() >= 0 ? m_list->currentRow() : 0;
        a << "--playlist-start=" + QString::number(startRow);
        QStringList files;
        for (int i = 0; i < m_list->count(); i++) files << m_list->item(i)->data(0, Qt::UserRole).toString();
        a << playableLocations(files);
        dbg("starting " + mpvPath() + " with " + QString::number(a.size()) + " args, socket " + m_sockPath);
        m_proc.start(mpvPath(), a);
        m_status->setText("starting…");
        setLed(Led::Starting);
        m_tries = 0;
        m_retry.start();
    }

    // Output settings as mpv "key=value": passed as --key=value to our own mpv and written into the
    // Jellyfin receiver's mpv.conf, so both play through the MiSTer the same way.
    QStringList outputOptions() const {
        const QString ml = m_modeline->currentIndex() == 0 ? "auto" : m_modeline->currentText();
        const QString host = m_host->text().trimmed();
        const QString nearMode = QString::number(m_near->currentIndex());
        const QString bars = m_subBars->isChecked() ? "yes" : "no";
        return {"vo-mister-host=" + host, "vo-mister-modeline=" + ml, "vo-mister-near=" + nearMode,
                "vo-mister-color-order=" + QString(m_set.value("colorOrder", 0).toInt() ? "rgb" : "bgr"),
                "vo-mister-pad=" + QString(m_pad->isChecked() ? "yes" : "no"),
                "vo-mister-field-order=" + QString(m_field->currentIndex() ? "bff" : "tff"),
                "vo-mister-flicker=" + QString(m_flicker->currentIndex() == 2 ? "full"
                                               : m_flicker->currentIndex() == 1 ? "light" : "off"),
                "ao-mister-host=" + host, "ao-mister-modeline=" + ml, "ao-mister-near=" + nearMode,
                "mister-sync-shift=" + m_set.value("syncShift").toString(),
                "osd-scale-by-window=no", "osd-font-size=" + QString::number(m_osdSize->value()),
                "osd-bold=yes", "osd-border-size=2.5", "osd-blur=0", "osd-shadow-offset=0",
                "sub-auto=fuzzy", "sub-scale=" + QString::number(m_subScale->value() / 100.0),
                "sub-pos=" + QString::number(m_subPos->value()),
                "sub-use-margins=" + bars, "sub-ass-force-margins=" + bars,
                "sub-color=" + subColorHex(), "sub-ass-style-overrides=" + subColorAss()};
    }

    // ---- Jellyfin receiver ------------------------------------------------------------------
    // The shim sets subtitle colour/size/position itself on every file, from its conf.json; it only
    // knows top / middle / bottom, so the position slider maps to the nearest of those.
    QJsonObject shimSettings() const {
        const int pos = m_subPos->value();
        return {{"subtitle_color", "#FF" + subColorHex().mid(1)},   // mpv's #AARRGGBB
                {"subtitle_size", m_subScale->value()},
                {"subtitle_position", pos >= 90 ? "bottom" : pos <= 20 ? "top" : "middle"}};
    }
    void jfSync() { m_jf.updateOptions(outputOptions(), shimSettings()); }
    void jfApplySetup() {
        m_jf.configure(mpvPath(), m_jfName->text(), outputOptions(), shimSettings());
        m_jf.setEnabled(m_jfOn->isChecked());
    }
    void jfState(bool active, const QString& title, bool paused) {
        if (active && m_proc.state() != QProcess::NotRunning) {   // a cast started: local playback yields
            stop();
            return;   // onStopped() shows the cast once our mpv is gone
        }
        if (m_proc.state() != QProcess::NotRunning) return;
        if (active) {
            if (!m_castShown) {   // the window's transport now drives the cast
                m_castShown = true;
                setControlsEnabled(true);
                m_jf.replay();
            }
            setLed(paused ? Led::Paused : Led::Playing);
            m_status->setText("Jellyfin ▶ " + (title.isEmpty() ? QString("…") : title));
        } else {
            m_castShown = false;
            resetTransport();
            m_status->setText("stopped");
        }
    }

    // Transport target: our own mpv while it runs, otherwise a running Jellyfin cast.
    bool casting() const { return m_proc.state() == QProcess::NotRunning && m_jf.active(); }
    void ctl(const QJsonArray& a) { if (casting()) m_jf.cmd(a); else cmd(a); }
    void togglePauseAny() {
        if (casting()) m_jf.key("space");
        else if (m_proc.state() == QProcess::NotRunning) start();
        else cmd({"cycle", "pause"});
    }
    void stopAny() {
        if (casting()) m_jf.key("q");
        else if (m_proc.state() != QProcess::NotRunning) stop();
    }
    void prevAny() { if (casting()) m_jf.key("<"); else cmd({"script-message", "mister-prev"}); }
    void nextAny() { if (casting()) m_jf.key(">"); else cmd({"script-message", "mister-next"}); }
    void resetTransport() {
        m_play->setText("▶  Play");
        setLed(Led::Stopped);
        setControlsEnabled(false);
        m_audio->clear();
        m_sub->clear();
        m_duration = 0;
        m_time->setText("0:00 / 0:00");
        const QSignalBlocker b(m_seek);
        m_seek->setValue(0);
    }

    void stop() {
        if (m_sock.state() == QLocalSocket::ConnectedState) cmd({"quit"});
        QTimer::singleShot(1500, this, [this] {
            if (m_proc.state() != QProcess::NotRunning) m_proc.kill();
        });
    }

    void onStopped() {
        m_retry.stop();
        m_sock.abort();
        resetTransport();
        m_castShown = false;
        m_curRow = -1;
        if (m_plTimer) m_plTimer->stop();
        m_hasPendingPl = false;
        for (int i = 0; i < m_list->count(); i++) setRowBold(m_list->item(i), false);
        renumber();
        m_status->setText(m_proc.exitCode() == 0 ? "stopped" : "mpv exited (code " + QString::number(m_proc.exitCode()) + ")");
        QFile::remove(m_sockPath);
        if (m_jf.active()) jfState(true, m_jf.title(), m_jf.paused());
    }

    void onConnected() {
        setControlsEnabled(true);
        m_status->setText("playing");
        applyView();
        int id = 1;
        for (const char* p : {"time-pos", "duration", "pause", "track-list", "playlist", "media-title", "volume", "speed"})
            cmd({"observe_property", id++, p});
    }

    // index: 0 white, 1 yellow, 2 cyan, 3 light green
    QString subColorHex() const {
        static const char* c[] = {"#FFFFFF", "#FFFF00", "#00FFFF", "#80FF80"};
        return c[m_subColor->currentIndex()];
    }
    QString subColorAss() const {   // ASS wants &HBBGGRR&; white = leave the styles alone
        static const char* c[] = {"", "PrimaryColour=&H0000FFFF&", "PrimaryColour=&H00FFFF00&", "PrimaryColour=&H0080FF80&"};
        return c[m_subColor->currentIndex()];
    }

    // Live picture settings (sub size + aspect/format), also re-sent on every (re)connect.
    // They go to the Jellyfin receiver's mpv as well, so a cast looks like local playback.
    void applyView() {
        const auto cmd = [this](const QJsonArray& a) { this->cmd(a); m_jf.cmd(a); };
        m_set.setValue("subScale", m_subScale->value());
        m_set.setValue("subPos", m_subPos->value());
        m_set.setValue("subColor", m_subColor->currentIndex());
        m_set.setValue("subBars", m_subBars->isChecked());
        m_set.setValue("osdSize", m_osdSize->value());
        cmd({"set_property", "osd-font-size", m_osdSize->value()});
        cmd({"set_property", "sub-use-margins", m_subBars->isChecked()});
        cmd({"set_property", "sub-ass-force-margins", m_subBars->isChecked()});
        cmd({"set_property", "sub-color", subColorHex()});
        cmd({"set_property", "sub-ass-style-overrides", subColorAss()});
        cmd({"set_property", "sub-pos", m_subPos->value()});
        m_set.setValue("aspect", m_aspect->currentIndex());
        cmd({"set_property", "sub-scale", m_subScale->value() / 100.0});
        const struct { const char* key; QSpinBox* w; } fx[] = {
            {"brightness", m_bri}, {"contrast", m_con}, {"saturation", m_sat},
            {"blacklevel", m_blk}, {"scanlines", m_scn}};
        for (const auto& f : fx) m_set.setValue(f.key, f.w->value());
        cmd({"set_property", "brightness", m_bri->value()});
        cmd({"set_property", "contrast", m_con->value()});
        cmd({"set_property", "saturation", m_sat->value()});
        cmd({"set_property", "mister-blacklevel", m_blk->value()});
        cmd({"set_property", "mister-scanlines", m_scn->value()});
        m_set.setValue("scanStyle", m_scanStyle->currentIndex());
        cmd({"set_property", "mister-scanstyle", m_scanStyle->currentIndex()});
        m_set.setValue("hsize", m_hsize->value());
        m_set.setValue("vsize", m_vsize->value());
        m_set.setValue("hoffset", m_hoff->value());
        m_set.setValue("voffset", m_voff->value());
        cmd({"set_property", "mister-hsize", m_hsize->value()});
        cmd({"set_property", "mister-vsize", m_vsize->value()});
        cmd({"set_property", "mister-hoffset", m_hoff->value()});
        cmd({"set_property", "mister-voffset", m_voff->value()});
        cmd({"set_property", "mister-calibrate", m_calib->isChecked()});
        const int a = m_aspect->currentIndex();
        cmd({"set_property", "keepaspect", a != 1});
        cmd({"set_property", "panscan", a == 2 ? 1.0 : 0.0});
        static const char* kOverride[] = {"-1", "-1", "-1", "4:3", "16:9", "21:9"};
        cmd({"set_property", "video-aspect-override", kOverride[a]});
        jfSync();   // OSD / subtitle values also land in the receiver's mpv.conf
    }

    // Sync position fine-tune per modeline, stored as "key:h:v,key:h:v" (same text as --mister-sync-shift).
    void loadSyncShift() {
        m_shift.clear();
        for (const QString& e : m_set.value("syncShift").toString().split(',', Qt::SkipEmptyParts)) {
            const QStringList f = e.split(':');
            if (f.size() == 3) m_shift[f[0]] = {f[1].toInt(), f[2].toInt()};
        }
    }
    QString syncShiftSpec() const {
        QStringList out;
        for (auto it = m_shift.cbegin(); it != m_shift.cend(); ++it)
            if (it.value().first || it.value().second)
                out << QString("%1:%2:%3").arg(it.key()).arg(it.value().first).arg(it.value().second);
        return out.join(',');
    }
    void showSyncShift() {
        const auto v = m_shift.value(m_shiftMl->currentText(), {0, 0});
        m_shiftLoading = true;
        m_shiftH->setValue(v.first);
        m_shiftV->setValue(v.second);
        m_shiftLoading = false;
    }
    void storeSyncShift() {
        if (m_shiftLoading) return;
        m_shift[m_shiftMl->currentText()] = {m_shiftH->value(), m_shiftV->value()};
        m_set.setValue("syncShift", syncShiftSpec());
        cmd({"set_property", "mister-sync-shift", syncShiftSpec()});
        jfSync();
    }

    void cmd(const QJsonArray& args) {
        if (m_sock.state() != QLocalSocket::ConnectedState) return;
        m_sock.write(QJsonDocument(QJsonObject{{"command", args}}).toJson(QJsonDocument::Compact) + "\n");
    }

    static QString fmt(double s) {
        const int t = int(s);
        if (t >= 3600)
            return QString("%1:%2:%3").arg(t / 3600).arg(t / 60 % 60, 2, 10, QChar('0')).arg(t % 60, 2, 10, QChar('0'));
        return QString("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
    }

    void onData() {
        m_buf += m_sock.readAll();
        int nl;
        while ((nl = m_buf.indexOf('\n')) >= 0) {
            const QJsonObject o = QJsonDocument::fromJson(m_buf.left(nl)).object();
            m_buf.remove(0, nl + 1);
            if (o.value("event").toString() != "property-change") continue;
            onProp(o.value("name").toString(), o.value("data"));
        }
    }

    // Property changes of whichever mpv the transport drives (ours, or the Jellyfin cast's).
    void onProp(const QString& n, const QJsonValue& v) {
        {
            if (n == "duration") {
                m_duration = v.toDouble();
            } else if (n == "time-pos" && !v.isNull()) {
                const double t = v.toDouble();
                m_time->setText(fmt(t) + " / " + fmt(m_duration) +
                                (m_duration > 0 ? "  (-" + fmt(m_duration - t) + ")" : QString()));
                if (m_duration > 0 && !m_seek->isSliderDown()) {
                    const QSignalBlocker b(m_seek);
                    m_seek->setValue(int(t / m_duration * 1000));
                }
            } else if (n == "pause") {
                m_play->setText(v.toBool() ? "▶  Play" : "❚❚  Pause");
                setLed(v.toBool() ? Led::Paused : Led::Playing);
            } else if (n == "volume" && v.isDouble()) {   // gamepad / other changes follow the slider
                m_remoteVS = true;
                m_vol->setValue(qRound(v.toDouble()));
                m_remoteVS = false;
            } else if (n == "speed" && v.isDouble()) {
                m_remoteVS = true;
                m_speed->setValue(v.toDouble());
                m_remoteVS = false;
            } else if (n == "media-title") {
                if (v.isString()) m_status->setText("▶ " + v.toString());
            } else if (n == "playlist") {
                m_latestPl = v.toArray();
                if (!m_plTimer) {
                    m_plTimer = new QTimer(this);
                    m_plTimer->setSingleShot(true);
                    connect(m_plTimer, &QTimer::timeout, this, [this] { showPlaylist(m_latestPl); });
                }
                m_plTimer->start(60);   // many playlist-move commands in a row -> one refresh
            } else if (n == "track-list") {
                fillTracks(v.toArray());
            }
        }
    }

    void fillTracks(const QJsonArray& tracks) {
        const QSignalBlocker ba(m_audio), bs(m_sub);
        m_audio->clear();
        m_sub->clear();
        m_sub->addItem("None", "no");
        for (const auto& tv : tracks) {
            const QJsonObject t = tv.toObject();
            const QString type = t.value("type").toString();
            QString label = QString("#%1 %2 %3").arg(t.value("id").toInt())
                                .arg(t.value("lang").toString(), t.value("title").toString()).trimmed();
            if (type == "audio") {
                m_audio->addItem(label, t.value("id").toInt());
                if (t.value("selected").toBool()) m_audio->setCurrentIndex(m_audio->count() - 1);
            } else if (type == "sub") {
                m_sub->addItem(label, t.value("id").toInt());
                if (t.value("selected").toBool()) m_sub->setCurrentIndex(m_sub->count() - 1);
            }
        }
    }

    QSettings m_set{"mistercast", "mpv-mister-ui"};
    // SSH login and core paths live in their own file, readable only by the user (chmod 600).
    static QString configDir() {
        return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/mistercast";
    }
    static QString loginPath() {
        QDir().mkpath(configDir());
        const QString p = configDir() + "/misterMPV-login.conf";
        if (!QFileInfo::exists(p)) { QFile f(p); if (f.open(QIODevice::WriteOnly)) f.close(); }
        QFile::setPermissions(p, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        return p;
    }
    QSettings m_login{loginPath(), QSettings::IniFormat};
    void saveLogin() {
        m_login.sync();
        QFile::setPermissions(m_login.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    // Older versions kept the login in the general settings file: move it over once.
    void migrateLogin() {
        bool moved = false;
        for (const char* k : {"sshUser", "sshPass", "sshPort", "corePathNlc", "corePathOrig"}) {
            if (!m_set.contains(k)) continue;
            if (!m_login.contains(k)) m_login.setValue(k, m_set.value(k));
            m_set.remove(k);
            moved = true;
        }
        if (moved) { m_set.sync(); saveLogin(); }
    }
    JellyfinReceiver m_jf;
    bool m_castShown = false;   // the transport currently shows / drives a Jellyfin cast
    QCheckBox* m_jfOn;
    QLineEdit *m_jfName, *m_jfServer;
    QLabel *m_jfStatus, *m_jfLabel;
    QLineEdit *m_file, *m_host;
    QComboBox *m_modeline, *m_near, *m_field, *m_flicker, *m_audio, *m_sub, *m_aspect;
    QDoubleSpinBox *m_hsize, *m_vsize;
    QSpinBox *m_hoff, *m_voff;
    QCheckBox* m_calib;
    QComboBox *m_core, *m_subColor, *m_scanStyle;
    QCheckBox* m_subBars;
    QStringList m_libRoots;
    QHash<QString, Tag> m_tags;
    QSet<QString> m_tagQueued;
    QStringList m_tagQueue;
    QGroupBox* m_plBox = nullptr;
    int m_curRow = -1;
    bool m_sortAsc = true;
    int m_sortCol = -1;
    int m_tagRunning = 0;
    int m_tagDone = 0;
    bool m_syncing = false;
    bool m_remoteVS = false;   // volume/speed value came from mpv: do not send it back
    bool m_hasPendingPl = false;
    QJsonArray m_latestPl;
    QTimer* m_plTimer = nullptr;
    QJsonArray m_pendingPl;
    QCheckBox* m_pad;
    QDialog* m_setupDlg;
    QComboBox* m_colorOrder;
    QLineEdit *m_sshHost, *m_sshUser, *m_sshPass, *m_sshKey, *m_coreNlc, *m_coreOrig;
    QLabel* m_loginStatus = nullptr;
    QLabel* m_sshOut = nullptr;
    int m_sshMode = 0;   // 0 launch core, 1 test login
    QSpinBox* m_sshPort;
    QSpinBox *m_vol;
    QDoubleSpinBox* m_speed;
    QSpinBox *m_osdSize, *m_subPos, *m_subScale, *m_bri, *m_con, *m_sat, *m_blk, *m_scn;
    QComboBox *m_repeat, *m_vis, *m_trackPos;
    QCheckBox* m_trackInfo;
    QCheckBox* m_shuffle;
    PlaylistView* m_list;
    QDialog* m_calDlg;
    QComboBox* m_shiftMl;
    QComboBox* m_theme = nullptr;
    QSpinBox *m_shiftH, *m_shiftV;
    QMap<QString, QPair<int, int>> m_shift;
    bool m_shiftLoading = false;
    QPushButton *m_prev, *m_next;
    QSlider* m_seek;
    QLabel *m_time = nullptr, *m_status = nullptr, *m_led = nullptr;
    QPushButton *m_play, *m_stop;
    QProcess m_proc;
    QProcess m_ssh;
    QString m_askpass;
    QLocalSocket m_sock;
    QTimer m_retry;
    QString m_sockPath;
    QByteArray m_buf;
    double m_duration = 0;
    int m_tries = 0;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Window w;
    w.show();
    const QStringList args = app.arguments().mid(1);
    if (!args.isEmpty()) QTimer::singleShot(0, &w, [&] { w.openArgs(args); });
    return app.exec();
}
