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
        setWindowTitle("mpv → MiSTer");
        auto* root = new QVBoxLayout(this);

        auto* fileRow = new QHBoxLayout;
        m_file = new QLineEdit;
        m_file->setPlaceholderText("file or URL");
        auto* browse = new QPushButton("Browse");
        auto* addUrl = new QPushButton("Add");
        addUrl->setToolTip("Add the file/URL in the box to the playlist");
        fileRow->addWidget(m_file);
        fileRow->addWidget(addUrl);
        fileRow->addWidget(browse);

        m_list = new QListWidget;
        m_list->setMinimumHeight(110);
        m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_list->setToolTip("Playlist - double-click to play an entry");
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
        og->setColumnStretch(1, 1);
        og->setColumnStretch(3, 1);

        // ---- Tracks group --------------------------------------------------------------------
        auto* trackBox = new QGroupBox("Audio / Subtitles");
        auto* tf = new QFormLayout(trackBox);
        tf->addRow("Audio:", m_audio);
        tf->addRow("Subtitles:", m_sub);
        tf->addRow("Subtitle size:", sliderRow(m_subScale));

        // ---- Picture group: colour sliders + CRT calibration dialog ---------------------------
        auto* picBox = new QGroupBox("Picture");
        auto* pf = new QFormLayout(picBox);
        pf->addRow("Brightness:", sliderRow(m_bri));
        pf->addRow("Contrast:", sliderRow(m_con));
        pf->addRow("Saturation:", sliderRow(m_sat));
        pf->addRow("Black level:", sliderRow(m_blk));
        pf->addRow("Scanlines:", sliderRow(m_scn));
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

        root->addLayout(fileRow);
        root->addLayout(seekRow);
        root->addLayout(btns);
        root->addWidget(m_status);
        root->addWidget(m_list, 1);
        root->addLayout(plRow);
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
        connect(m_subScale, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { applyView(); });
        connect(m_aspect, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { applyView(); });
        connect(m_loop, &QCheckBox::toggled, this, [this](bool on) {
            cmd({"set_property", "loop-file", on ? "inf" : "no"});
        });
        connect(&m_proc, &QProcess::finished, this, [this] { onStopped(); });
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
        const QString rel = QCoreApplication::applicationDirPath() + "/../../build/mpv";
        return QFileInfo::exists(rel) ? rel : "mpv";
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
        m_set.setValue("flicker", m_flicker->currentIndex());
        const QString ml = m_modeline->currentIndex() == 0 ? "auto" : m_modeline->currentText();
        m_sockPath = QDir::temp().filePath(QString("mpv-mister-%1.sock").arg(QCoreApplication::applicationPid()));
        QFile::remove(m_sockPath);
        QStringList a = {"--no-config", "--vo=mister", "--ao=mister",
                         "--vo-mister-host=" + m_host->text(), "--vo-mister-modeline=" + ml,
                         "--vo-mister-near=" + QString::number(m_near->currentIndex()),
                         "--vo-mister-field-order=" + QString(m_field->currentIndex() ? "bff" : "tff"),
                         "--vo-mister-flicker=" + QString(m_flicker->currentIndex() == 2 ? "full"
                                                          : m_flicker->currentIndex() == 1 ? "light" : "off"),
                         "--ao-mister-host=" + m_host->text(), "--ao-mister-modeline=" + ml,
                         "--ao-mister-near=" + QString::number(m_near->currentIndex()),
                         "--input-ipc-server=" + m_sockPath, "--no-terminal", "--idle=no",
                         "--sub-auto=fuzzy",
                         "--sub-scale=" + QString::number(m_subScale->value() / 100.0)};
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
        for (const char* p : {"time-pos", "duration", "pause", "track-list", "playlist"})
            cmd({"observe_property", id++, p});
    }

    // Live picture settings (sub size + aspect/format), also re-sent on every (re)connect.
    void applyView() {
        m_set.setValue("subScale", m_subScale->value());
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
                m_time->setText(fmt(t) + " / " + fmt(m_duration));
                if (m_duration > 0 && !m_seek->isSliderDown()) {
                    const QSignalBlocker b(m_seek);
                    m_seek->setValue(int(t / m_duration * 1000));
                }
            } else if (n == "pause") {
                m_pause->setText(v.toBool() ? "▶" : "⏸");
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
    QSpinBox *m_subScale, *m_bri, *m_con, *m_sat, *m_blk, *m_scn;
    QCheckBox *m_loop, *m_loopPl;
    QListWidget* m_list;
    QDialog* m_calDlg;
    QPushButton *m_prev, *m_next;
    QSlider* m_seek;
    QLabel *m_time, *m_status;
    QPushButton *m_play, *m_pause;
    QProcess m_proc;
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
