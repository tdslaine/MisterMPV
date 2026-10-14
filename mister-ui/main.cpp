// Small Qt front-end for the mpv MiSTer fork: starts mpv with --vo=mister --ao=mister and
// drives it over mpv's JSON IPC (play/pause, seek, tracks, modeline).
#include <algorithm>
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
#include <QListWidget>
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

#include "modeline_keys.h"

class Window : public QWidget {
public:
    Window() {
        setWindowTitle("misterMPV");
        auto* root = new QVBoxLayout(this);

        auto* fileRow = new QHBoxLayout;
        m_file = new QLineEdit;
        m_file->setPlaceholderText("file or URL");
        auto* browse = new QPushButton("Load…");
        browse->setToolTip("Pick one or more files (also network shares) and add them to the playlist");
        auto* addUrl = new QPushButton("Add to PL");
        addUrl->setToolTip("Add the file/URL in the box to the playlist");
        fileRow->addWidget(m_file);
        fileRow->addWidget(browse);
        fileRow->addWidget(addUrl);

        m_list = new QListWidget;
        m_list->setMinimumHeight(110);
        m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_list->setToolTip("Playlist - double-click to play an entry, drag to reorder");
        m_list->setDragDropMode(QAbstractItemView::InternalMove);
        m_list->setDefaultDropAction(Qt::MoveAction);
        auto* plRow = new QHBoxLayout;
        auto* plPrev = new QPushButton("⏮ Prev");
        auto* plNext = new QPushButton("Next ⏭");
        auto* plRemove = new QPushButton("Remove");
        auto* plClear = new QPushButton("Clear");
        m_loopPl = new QCheckBox("Loop playlist");
        m_prev = plPrev;
        m_next = plNext;
        for (auto* b : {plRemove, plClear}) plRow->addWidget(b);
        plRow->addWidget(m_loopPl);
        plRow->addStretch(1);
        for (const QString& f : m_set.value("playlist").toStringList()) addLocal(f);
        m_loopPl->setChecked(m_set.value("loopPlaylist", false).toBool());

        m_host = new QLineEdit(qEnvironmentVariable("MISTER_HOST", m_set.value("host").toString()));
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
        m_calDlg->setMinimumWidth(460);
        connect(calBtn, &QPushButton::clicked, this, [this] { m_calDlg->show(); m_calDlg->raise(); });
        connect(m_calDlg, &QDialog::finished, this, [this] { m_calib->setChecked(false); });

        // ---- Transport (top) -------------------------------------------------------------------
        m_seek = new QSlider(Qt::Horizontal);
        m_seek->setRange(0, 1000);
        m_time = new QLabel("0:00 / 0:00");
        m_status = new QLabel;
        auto* seekRow = new QHBoxLayout;
        seekRow->addWidget(m_seek, 1);
        seekRow->addWidget(m_time);

        m_loop = new QCheckBox("Loop");
        m_play = new QPushButton("▶ Play");
        m_pause = new QPushButton("⏸");
        auto* back = new QPushButton("⏪ 10s");
        auto* fwd = new QPushButton("10s ⏩");
        auto* btns = new QHBoxLayout;
        btns->addWidget(m_play, 1);
        btns->addWidget(plPrev);
        btns->addWidget(back);
        btns->addWidget(m_pause);
        btns->addWidget(fwd);
        btns->addWidget(plNext);
        btns->addWidget(m_loop);

        auto* plBox = new QGroupBox("Playlist");
        auto* plLay = new QVBoxLayout(plBox);
        plLay->addWidget(m_list, 1);
        plLay->addLayout(plRow);
        QFont sf = m_status->font();
        sf.setBold(true);
        sf.setPointSizeF(sf.pointSizeF() + 1);
        m_status->setFont(sf);
        m_status->setText("stopped");
        root->addLayout(fileRow);
        root->addWidget(m_status);
        root->addLayout(seekRow);
        root->addLayout(btns);
        root->addWidget(plBox, 1);
        root->addWidget(outBox);
        root->addWidget(trackBox);
        root->addWidget(picBox);
        setMinimumWidth(720);
        setControlsEnabled(false);

        connect(browse, &QPushButton::clicked, this, [this] {
            const QList<QUrl> urls = QFileDialog::getOpenFileUrls(
                this, "Add media", QUrl(), QString(), nullptr, {}, {"file", "smb", "sftp", "ftp", "davs", "webdav"});
            for (const QUrl& u : urls) addEntry(resolveLocation(u.isLocalFile() ? u.toLocalFile() : u.toString()));
        });
        connect(addUrl, &QPushButton::clicked, this, [this] {
            const QString f = m_file->text().trimmed();
            if (!f.isEmpty()) { addEntry(resolveLocation(f)); m_file->clear(); }
        });
        connect(m_file, &QLineEdit::returnPressed, addUrl, &QPushButton::click);
        connect(plRemove, &QPushButton::clicked, this, [this] {
            QList<int> rows;
            for (auto* it : m_list->selectedItems()) rows << m_list->row(it);
            std::sort(rows.begin(), rows.end(), std::greater<int>());   // highest first keeps indices valid
            for (int r : rows) removeEntry(r);
            savePlaylist();
        });
        connect(plClear, &QPushButton::clicked, this, [this] {
            if (running()) cmd({"playlist-clear"}); else m_list->clear();
            savePlaylist();
        });
        connect(plPrev, &QPushButton::clicked, this, [this] { cmd({"playlist-prev"}); });
        connect(plNext, &QPushButton::clicked, this, [this] { cmd({"playlist-next"}); });
        // Drag & drop reorder: while mpv runs it owns the order, so mirror each move there
        // (it then reports the new list back); otherwise just persist the local order.
        connect(m_list->model(), &QAbstractItemModel::rowsMoved, this,
                [this](const QModelIndex&, int start, int end, const QModelIndex&, int dest) {
            if (m_syncing) return;
            if (running()) {
                const int n = end - start + 1;
                if (dest > end)
                    for (int i = 0; i < n; i++) cmd(QJsonArray{"playlist-move", start, dest});
                else
                    for (int i = 0; i < n; i++) cmd(QJsonArray{"playlist-move", start + i, dest + i});
            }
            savePlaylist();
        });
        connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
            const int r = m_list->row(it);
            if (running()) cmd(QJsonArray{"set_property", "playlist-pos", r});
            else start(r);
        });
        connect(m_loopPl, &QCheckBox::toggled, this, [this](bool on) {
            m_set.setValue("loopPlaylist", on);
            cmd({"set_property", "loop-playlist", on ? "inf" : "no"});
        });
        connect(m_play, &QPushButton::clicked, this, [this] {
            if (m_proc.state() == QProcess::NotRunning) start(); else stop();
        });
        connect(m_pause, &QPushButton::clicked, this, [this] { cmd({"cycle", "pause"}); });
        connect(back, &QPushButton::clicked, this, [this] { cmd({"seek", -10, "relative"}); });
        connect(fwd, &QPushButton::clicked, this, [this] { cmd({"seek", 10, "relative"}); });
        connect(m_seek, &QSlider::sliderReleased, this, [this] {
            if (m_duration > 0)
                cmd({"seek", m_seek->value() / 1000.0 * m_duration, "absolute"});
        });
        connect(m_audio, &QComboBox::activated, this, [this](int i) {
            cmd(QJsonArray{"set_property", "aid", QJsonValue::fromVariant(m_audio->itemData(i))});
        });
        connect(m_sub, &QComboBox::activated, this, [this](int i) {
            cmd(QJsonArray{"set_property", "sid", QJsonValue::fromVariant(m_sub->itemData(i))});
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
        connect(m_aspect, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { applyView(); });
        connect(m_loop, &QCheckBox::toggled, this, [this](bool on) {
            cmd({"set_property", "loop-file", on ? "inf" : "no"});
        });
        connect(&m_proc, &QProcess::finished, this, [this] { onStopped(); });
        connect(&m_ssh, &QProcess::finished, this, [this](int code) { onSshFinished(code); });
        connect(&m_ssh, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) { m_status->setText("ssh not found - install openssh"); }
        });
        connect(&m_sock, &QLocalSocket::readyRead, this, &Window::onData);
        m_retry.setInterval(150);
        connect(&m_retry, &QTimer::timeout, this, [this] {
            m_sock.connectToServer(m_sockPath);
            if (m_sock.waitForConnected(50) || ++m_tries > 40) {
                m_retry.stop();
                if (m_sock.state() == QLocalSocket::ConnectedState) onConnected();
            }
        });
    }

private:
    void setControlsEnabled(bool on) {
        for (auto* w : {(QWidget*)m_prev, (QWidget*)m_next, (QWidget*)m_pause, (QWidget*)m_seek, (QWidget*)m_audio, (QWidget*)m_sub})
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
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR", "/run/user/1000");
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
        return loc;   // no kio-fuse mount found: let mpv try (will fail for smb)
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
    QWidget* sliderRow(QDoubleSpinBox* s) {   // 0.1 steps
        auto* sl = new QSlider(Qt::Horizontal);
        sl->setRange(int(s->minimum() * 10), int(s->maximum() * 10));
        sl->setValue(int(s->value() * 10));
        connect(sl, &QSlider::valueChanged, s, [s](int v) { s->setValue(v / 10.0); });
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), sl, [sl](double v) { sl->setValue(int(v * 10 + 0.5)); });
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
    void testConnection() {
        if (m_proc.state() != QProcess::NotRunning) {
            m_status->setText("Stop playback first - the MiSTer takes one sender at a time");
            return;
        }
        QUdpSocket sock;
        const QHostAddress addr(m_host->text().trimmed());
        if (addr.isNull()) { m_status->setText("Test: invalid MiSTer IP"); return; }
        QEventLoop loop;
        QTimer t;
        t.setSingleShot(true);
        connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&sock, &QUdpSocket::readyRead, &loop, &QEventLoop::quit);
        QElapsedTimer el;
        el.start();
        sock.writeDatagram(QByteArray(1, 5), addr, 32100);
        t.start(1000);
        loop.exec();
        if (sock.hasPendingDatagrams()) {
            sock.readDatagram(nullptr, 0);
            m_status->setText(QString("MiSTer answered in %1 ms - Groovy core is running").arg(el.elapsed()));
        } else {
            m_status->setText("No reply from the MiSTer - core not loaded (try \"Launch core\") or wrong IP?");
        }
    }

    // SSH password via SSH_ASKPASS (no sshpass needed); the password only lives in the child's env.
    void launchCore() {
        if (m_ssh.state() != QProcess::NotRunning) return;
        const QString host = m_host->text().trimmed();
        if (host.isEmpty()) { m_status->setText("Enter the MiSTer IP first"); return; }
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const QString askpass = dir + QString("/misterMPV-askpass-%1.sh").arg(QCoreApplication::applicationPid());
        QFile f(askpass);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { m_status->setText("Cannot write askpass helper"); return; }
        f.write("#!/bin/sh\nprintf '%s\\n' \"$MISTER_SSH_PW\"\n");
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        f.close();
        m_askpass = askpass;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("SSH_ASKPASS", askpass);
        env.insert("SSH_ASKPASS_REQUIRE", "force");
        env.insert("MISTER_SSH_PW", m_set.value("sshPass", "1").toString());
        m_ssh.setProcessEnvironment(env);
        m_ssh.setStandardInputFile(QProcess::nullDevice());
        const QString user = m_set.value("sshUser", "root").toString();
        const QString port = QString::number(m_set.value("sshPort", 22).toInt());
        const bool nlc = m_core->currentIndex() == 0;
        QString pat = m_set.value(nlc ? "corePathNlc" : "corePathOrig", nlc ? kDefaultNlcCore : kDefaultOrigCore)
                          .toString().trimmed();
        if (pat.isEmpty()) pat = nlc ? kDefaultNlcCore : kDefaultOrigCore;
        const QString remote =
            "f=$(ls -t " + pat + " 2>/dev/null | head -1); [ -n \"$f\" ] || exit 3; "
            "echo \"load_core $f\" > /dev/MiSTer_cmd";
        m_ssh.start("ssh", {"-p", port, "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                            "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=5", user + "@" + host, remote});
        m_status->setText("Loading Groovy core on the MiSTer…");
    }
    void onSshFinished(int code) {
        if (!m_askpass.isEmpty()) { QFile::remove(m_askpass); m_askpass.clear(); }
        if (code == 0)
            m_status->setText((m_core->currentIndex() == 0 ? "GroovyNLC" : "Groovy") + QString(" core loading - give it a few seconds, then \"Test connection\""));
        else if (code == 3)
            m_status->setText("No core file found on the MiSTer (check the core path in Setup…)");
        else
            m_status->setText("SSH failed (" + QString::number(code) + "): " +
                              QString::fromUtf8(m_ssh.readAllStandardError()).trimmed().left(160));
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
        lay->addWidget(disp);

        auto* ctl = new QGroupBox("Control");
        auto* cl = new QVBoxLayout(ctl);
        cl->addWidget(m_pad);
        lay->addWidget(ctl);

        auto* ssh = new QGroupBox("MiSTer login (for \"Launch core\")");
        auto* sf = new QFormLayout(ssh);
        m_sshUser = new QLineEdit;
        m_sshPass = new QLineEdit;
        m_sshPass->setEchoMode(QLineEdit::Password);
        m_sshPass->setToolTip("Stored unencrypted in the settings file (MiSTer default: root / 1).");
        m_sshPort = new QSpinBox;
        m_sshPort->setRange(1, 65535);
        m_coreNlc = new QLineEdit;
        m_coreNlc->setToolTip("File (or glob) of the GroovyNLC core on the MiSTer; the newest match is loaded.");
        m_coreOrig = new QLineEdit;
        m_coreOrig->setToolTip("File (or glob) of the original Groovy core; the newest match is loaded.");
        sf->addRow("SSH user:", m_sshUser);
        sf->addRow("SSH password:", m_sshPass);
        sf->addRow("SSH port:", m_sshPort);
        sf->addRow("GroovyNLC core:", m_coreNlc);
        sf->addRow("Groovy core (original):", m_coreOrig);
        sf->addRow(new QLabel("Streaming uses UDP 32100 (fixed by the Groovy core)."));
        lay->addWidget(ssh);

        auto* btns = new QHBoxLayout;
        auto* defaults = new QPushButton("Defaults");
        auto* ok = new QPushButton("Save");
        btns->addWidget(defaults);
        btns->addStretch(1);
        btns->addWidget(ok);
        lay->addLayout(btns);
        connect(defaults, &QPushButton::clicked, m_setupDlg, [this] {
            m_colorOrder->setCurrentIndex(0); m_sshUser->setText("root"); m_sshPass->setText("1");
            m_sshPort->setValue(22); m_coreNlc->setText(kDefaultNlcCore); m_coreOrig->setText(kDefaultOrigCore);
            m_osdSize->setValue(28); m_subColor->setCurrentIndex(0); m_subBars->setChecked(true); m_pad->setChecked(true);
        });
        connect(ok, &QPushButton::clicked, m_setupDlg, &QDialog::accept);
        connect(m_setupDlg, &QDialog::accepted, this, [this] {
            m_set.setValue("colorOrder", m_colorOrder->currentIndex());
            m_set.setValue("sshUser", m_sshUser->text().trimmed());
            m_set.setValue("sshPass", m_sshPass->text());
            m_set.setValue("sshPort", m_sshPort->value());
            m_set.setValue("corePathNlc", m_coreNlc->text().trimmed());
            m_set.setValue("corePathOrig", m_coreOrig->text().trimmed());
            m_set.setValue("pad", m_pad->isChecked());
        });
        m_setupDlg->setMinimumWidth(480);
    }

    void sshDialog() {   // "Setup…" button: reload the stored values, then show
        m_colorOrder->setCurrentIndex(m_set.value("colorOrder", 0).toInt());
        m_sshUser->setText(m_set.value("sshUser", "root").toString());
        m_sshPass->setText(m_set.value("sshPass", "1").toString());
        m_sshPort->setValue(m_set.value("sshPort", 22).toInt());
        m_coreNlc->setText(m_set.value("corePathNlc", kDefaultNlcCore).toString());
        m_coreOrig->setText(m_set.value("corePathOrig", kDefaultOrigCore).toString());
        m_setupDlg->exec();
    }

    bool running() const { return m_sock.state() == QLocalSocket::ConnectedState; }

    static QString entryLabel(const QString& f) {
        return QFileInfo::exists(f) ? QFileInfo(f).fileName() : f;
    }
    void addLocal(const QString& f) {
        auto* it = new QListWidgetItem(entryLabel(f));
        it->setData(Qt::UserRole, f);
        it->setToolTip(f);
        m_list->addItem(it);
    }
    void savePlaylist() {
        QStringList l;
        for (int i = 0; i < m_list->count(); i++) l << m_list->item(i)->data(Qt::UserRole).toString();
        m_set.setValue("playlist", l);
    }
    // While mpv runs it owns the list (it reports back via the "playlist" property).
    void addEntry(const QString& f) {
        if (running()) cmd({"loadfile", f, "append"}); else addLocal(f);
        savePlaylist();
    }
    void removeEntry(int row) {
        if (running()) cmd(QJsonArray{"playlist-remove", row}); else delete m_list->takeItem(row);
    }
    void showPlaylist(const QJsonArray& pl) {
        const QSignalBlocker b(m_list);
        m_syncing = true;
        m_list->clear();
        int cur = -1;
        for (int i = 0; i < pl.size(); i++) {
            const QJsonObject o = pl[i].toObject();
            const QString f = o.value("filename").toString();
            addLocal(f);
            const QString title = o.value("title").toString();
            if (!title.isEmpty()) m_list->item(i)->setText(title);
            if (o.value("current").toBool()) cur = i;
        }
        for (int i = 0; i < m_list->count(); i++) {
            QFont f = m_list->item(i)->font();
            f.setBold(i == cur);
            m_list->item(i)->setFont(f);
            m_list->item(i)->setText((i == cur ? "▶ " : "") + m_list->item(i)->text());
        }
        if (cur >= 0) m_list->scrollToItem(m_list->item(cur));
        m_syncing = false;
        savePlaylist();
    }

    void start(int startRow = -1) {
        if (m_list->count() == 0 && !m_file->text().trimmed().isEmpty()) {
            addLocal(m_file->text().trimmed());
            m_file->clear();
        }
        if (m_list->count() == 0) return;
        savePlaylist();
        m_set.setValue("host", m_host->text());
        m_set.setValue("modeline", m_modeline->currentText());
        m_set.setValue("near", m_near->currentIndex());
        m_set.setValue("fieldOrder", m_field->currentIndex());
        m_set.setValue("pad", m_pad->isChecked());
        m_set.setValue("flicker", m_flicker->currentIndex());
        const QString ml = m_modeline->currentIndex() == 0 ? "auto" : m_modeline->currentText();
        m_sockPath = QDir::temp().filePath(QString("mpv-mister-%1.sock").arg(QCoreApplication::applicationPid()));
        QFile::remove(m_sockPath);
        QStringList a = {"--no-config", "--vo=mister", "--ao=mister",
                         "--vo-mister-host=" + m_host->text(), "--vo-mister-modeline=" + ml,
                         "--vo-mister-near=" + QString::number(m_near->currentIndex()),
                         "--osd-scale-by-window=no", "--osd-font-size=" + QString::number(m_osdSize->value()),
                         "--osd-bold=yes", "--osd-border-size=2.5", "--osd-blur=0", "--osd-shadow-offset=0",
                         "--vo-mister-color-order=" + QString(m_set.value("colorOrder", 0).toInt() ? "rgb" : "bgr"),
                         "--vo-mister-pad=" + QString(m_pad->isChecked() ? "yes" : "no"),
                         "--vo-mister-field-order=" + QString(m_field->currentIndex() ? "bff" : "tff"),
                         "--vo-mister-flicker=" + QString(m_flicker->currentIndex() == 2 ? "full"
                                                          : m_flicker->currentIndex() == 1 ? "light" : "off"),
                         "--ao-mister-host=" + m_host->text(), "--ao-mister-modeline=" + ml,
                         "--ao-mister-near=" + QString::number(m_near->currentIndex()),
                         "--input-ipc-server=" + m_sockPath, "--no-terminal", "--idle=no",
                         "--sub-auto=fuzzy",
                         "--sub-scale=" + QString::number(m_subScale->value() / 100.0),
                         "--sub-pos=" + QString::number(m_subPos->value()),
                         "--sub-use-margins=" + QString(m_subBars->isChecked() ? "yes" : "no"),
                         "--sub-ass-force-margins=" + QString(m_subBars->isChecked() ? "yes" : "no"),
                         "--sub-color=" + subColorHex(),
                         "--sub-ass-style-overrides=" + subColorAss()};
        if (m_loop->isChecked()) a << "--loop-file=inf";
        if (m_loopPl->isChecked()) a << "--loop-playlist=inf";
        if (startRow < 0) startRow = m_list->currentRow() >= 0 ? m_list->currentRow() : 0;
        a << "--playlist-start=" + QString::number(startRow);
        for (int i = 0; i < m_list->count(); i++) a << m_list->item(i)->data(Qt::UserRole).toString();
        m_proc.start(mpvPath(), a);
        m_play->setText("⏹ Stop");
        m_status->setText("starting…");
        m_tries = 0;
        m_retry.start();
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
        m_play->setText("▶ Play");
        setControlsEnabled(false);
        m_status->setText(m_proc.exitCode() == 0 ? "stopped" : "mpv exited (code " + QString::number(m_proc.exitCode()) + ")");
        m_audio->clear();
        m_sub->clear();
        QFile::remove(m_sockPath);
    }

    void onConnected() {
        setControlsEnabled(true);
        m_status->setText("playing");
        applyView();
        int id = 1;
        for (const char* p : {"time-pos", "duration", "pause", "track-list", "playlist", "media-title"})
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
    void applyView() {
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
            const QString n = o.value("name").toString();
            const QJsonValue v = o.value("data");
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
                m_pause->setText(v.toBool() ? "▶" : "⏸");
            } else if (n == "media-title") {
                if (v.isString()) m_status->setText("▶ " + v.toString());
            } else if (n == "playlist") {
                showPlaylist(v.toArray());
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
    QLineEdit *m_file, *m_host;
    QComboBox *m_modeline, *m_near, *m_field, *m_flicker, *m_audio, *m_sub, *m_aspect;
    QDoubleSpinBox *m_hsize, *m_vsize;
    QSpinBox *m_hoff, *m_voff;
    QCheckBox* m_calib;
    QComboBox *m_core, *m_subColor, *m_scanStyle;
    QCheckBox* m_subBars;
    bool m_syncing = false;
    QCheckBox* m_pad;
    QDialog* m_setupDlg;
    QComboBox* m_colorOrder;
    QLineEdit *m_sshUser, *m_sshPass, *m_coreNlc, *m_coreOrig;
    QSpinBox* m_sshPort;
    QSpinBox *m_osdSize, *m_subPos, *m_subScale, *m_bri, *m_con, *m_sat, *m_blk, *m_scn;
    QCheckBox *m_loop, *m_loopPl;
    QListWidget* m_list;
    QDialog* m_calDlg;
    QPushButton *m_prev, *m_next;
    QSlider* m_seek;
    QLabel *m_time, *m_status;
    QPushButton *m_play, *m_pause;
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
    return app.exec();
}
