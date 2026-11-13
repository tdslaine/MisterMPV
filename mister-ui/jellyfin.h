// Jellyfin receiver: a private jellyfin-mpv-shim instance that plays through our mpv (vo/ao_mister).
//
// The shim runs with its own config directory (~/.config/mistercast/jellyfin), so it is a separate
// Jellyfin device (own client_uuid / device_id, own login) and never touches the user's normal shim.
// It starts our mpv binary itself (mpv_ext) and controls it over a JSON-IPC socket at a fixed path;
// we connect a second client to that socket to see what it plays and to push the picture settings.
//
// The fork defaults to force-window=yes (VO also for audio files). An idle receiver must not hold
// the MiSTer, so its mpv.conf sets force-window=no and only turns it on while a file is loaded
// (auto profile on idle_active) - idle means no VO, no AO, no packets to the MiSTer.
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <functional>

class JellyfinReceiver {
public:
    // What the UI wants to hear about. active = a file is loaded in the receiver's mpv.
    std::function<void(bool active, const QString& title, bool paused)> onState;
    std::function<void(const QString& text)> onStatus;      // one-line state for the Setup dialog / main window
    std::function<void(const QString& code)> onLoginCode;   // Quick Connect code to enter in Jellyfin
    std::function<void()> onConnected;                      // IPC is up: push options / picture settings

    JellyfinReceiver() {
        m_retry.setInterval(1000);
        QObject::connect(&m_retry, &QTimer::timeout, [this] {
            if (m_proc.state() != QProcess::Running) { m_retry.stop(); return; }
            if (m_sock.state() == QLocalSocket::UnconnectedState) m_sock.connectToServer(sockPath());
        });
        QObject::connect(&m_sock, &QLocalSocket::connected, [this] {
            m_retry.stop();
            m_restart.setInterval(5000);   // healthy again: next crash restarts quickly
            int id = 1;
            for (const char* p : {"idle-active", "media-title", "pause"}) cmd({"observe_property", id++, p});
            m_ipcUp = true;
            if (!m_loggingIn) readyStatus();   // during sign-in the Quick Connect code stays on screen
            if (onConnected) onConnected();
        });
        QObject::connect(&m_sock, &QLocalSocket::disconnected, [this] {
            m_ipcUp = false;
            setActive(false);
            if (m_proc.state() == QProcess::Running) m_retry.start();   // shim restarts its mpv
        });
        QObject::connect(&m_sock, &QLocalSocket::readyRead, [this] { onData(); });
        m_proc.setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(&m_proc, &QProcess::readyRead, [this] { onLog(); });
        QObject::connect(&m_proc, &QProcess::errorOccurred, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) status("jellyfin-mpv-shim not found - install it (pacman -S jellyfin-mpv-shim)");
        });
        QObject::connect(&m_proc, &QProcess::finished, [this](int code) {
            m_retry.stop();
            m_ipcUp = false;
            m_sock.abort();
            setActive(false);
            if (m_stopping) { m_stopping = false; status("off"); return; }
            if (!m_enabled) return;
            if (m_loggingIn) { m_loggingIn = false; status(signedIn() ? "signed in" : "sign-in failed or cancelled"); }
            else status("receiver exited (code " + QString::number(code) + ") - server unreachable? retrying in "
                        + QString::number(m_restart.interval() / 1000) + " s");
            if (signedIn()) {
                m_restart.start();
                m_restart.setInterval(qMin(m_restart.interval() * 2, 60000));   // back off while it keeps failing
            }
        });
        m_restart.setSingleShot(true);
        m_restart.setInterval(5000);
        QObject::connect(&m_restart, &QTimer::timeout, [this] { if (m_enabled) launch({}); });
    }

    static QString confDir() {
        return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/mistercast/jellyfin";
    }
    // Shim output of the current run (rewritten on every start), for when something does not work.
    static QString logPath() {
        return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/mistercast/jellyfin-shim.log";
    }
    static QString sockPath() {
        QString rt = qEnvironmentVariable("XDG_RUNTIME_DIR");
        if (rt.isEmpty()) rt = QDir::tempPath();
        return rt + "/misterMPV-jellyfin.sock";
    }
    // users.json holds the login (token); we only look whether one exists.
    static bool signedIn() {
        QFile f(confDir() + "/users.json");
        if (!f.open(QIODevice::ReadOnly)) return false;
        for (const auto& u : QJsonDocument::fromJson(f.readAll()).object().value("users").toArray())
            if (!u.toObject().value("credentials").toArray().isEmpty()) return true;
        return false;
    }
    // Server address of the user's normal shim, to prefill the sign-in field (address only).
    static QString mainShimServer() {
        QFile f(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/jellyfin-mpv-shim/users.json");
        if (!f.open(QIODevice::ReadOnly)) return {};
        for (const auto& u : QJsonDocument::fromJson(f.readAll()).object().value("users").toArray())
            for (const auto& c : u.toObject().value("credentials").toArray()) {
                const QString a = c.toObject().value("address").toString();
                if (!a.isEmpty()) return a;
            }
        return {};
    }

    bool active() const { return m_active; }
    bool paused() const { return m_paused; }
    QString title() const { return m_title; }
    bool running() const { return m_proc.state() != QProcess::NotRunning; }

    // mpvOptions: "key=value" lines for the output (host, modeline, OSD, subs) - the same set the
    // UI passes to its own mpv, so both play identically.
    // shimSettings: conf.json keys the shim re-applies on every file (subtitle_color/size/position).
    void configure(const QString& mpvPath, const QString& name, const QStringList& mpvOptions,
                   const QJsonObject& shimSettings) {
        m_mpvPath = QFileInfo(mpvPath).absoluteFilePath();
        m_name = name.trimmed().isEmpty() ? QString("MiSTer") : name.trimmed();
        m_options = mpvOptions;
        m_shimSettings = shimSettings;
    }

    void setEnabled(bool on) {
        m_enabled = on;
        if (!on) { m_restart.stop(); shutdown(); status("off"); return; }
        if (running()) return;
        if (!signedIn()) { status("not signed in - Setup… → Sign in"); return; }
        launch({});
    }

    // Quick Connect: the shim prints a code, the user enters it in Jellyfin (user menu → Quick Connect).
    // It then keeps running as the receiver.
    void signIn(const QString& server) {
        m_enabled = true;
        m_restart.stop();
        shutdown();
        m_loggingIn = true;
        launch({"--server", server.trimmed(), "--quick-connect"});
        status("contacting " + server.trimmed() + "…");
    }

    // Settings changed: new mpv.conf for the next mpv start, and the live mpv gets the values now
    // (vo/ao options are read when the next file opens the VO).
    // Only changed values are sent: re-setting e.g. the modeline would make the VO resync.
    void updateOptions(const QStringList& mpvOptions, const QJsonObject& shimSettings) {
        if (shimSettings != m_shimSettings) {   // read by the shim at its next start
            m_shimSettings = shimSettings;
            if (running()) writeConf();
        }
        if (mpvOptions == m_options) return;
        const QStringList old = m_options;
        m_options = mpvOptions;
        if (!running()) return;
        writeMpvConf();
        for (const QString& l : m_options) {
            const int eq = l.indexOf('=');
            if (eq > 0 && !old.contains(l)) cmd({"set_property", l.left(eq), l.mid(eq + 1)});
        }
    }

    void cmd(const QJsonArray& args) {
        if (m_sock.state() != QLocalSocket::ConnectedState) return;
        m_sock.write(QJsonDocument(QJsonObject{{"command", args}}).toJson(QJsonDocument::Compact) + "\n");
    }
    void stopPlayback() { cmd({"stop"}); }

    // Shim first (it would restart a vanished mpv), then its mpv if it outlived the shim.
    void shutdown() {
        if (!running()) return;
        m_stopping = true;
        m_retry.stop();
        m_proc.terminate();
        if (!m_proc.waitForFinished(4000)) { m_proc.kill(); m_proc.waitForFinished(1000); }
        m_sock.abort();
        setActive(false);
        QLocalSocket s;   // the shim leaves its mpv behind when it is terminated
        s.connectToServer(sockPath());
        if (s.waitForConnected(500)) {
            s.write("{\"command\":[\"quit\"]}\n");
            s.waitForBytesWritten(500);
            s.waitForDisconnected(2000);
        }
    }

private:
    void status(const QString& s) { m_status = s; if (onStatus) onStatus(s); }
    // Only signed in is it a device in Jellyfin; a running mpv alone says nothing about that.
    void readyStatus() {
        if (signedIn()) status("ready - cast to \"" + m_name + "\" from Jellyfin");
        else status("not signed in - Setup… → Sign in");
    }

    void launch(const QStringList& extra) {
        if (m_mpvPath.isEmpty() || !QFileInfo::exists(m_mpvPath)) { status("mpv binary not found: " + m_mpvPath); return; }
        writeConf();
        writeMpvConf();
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("PYTHONUNBUFFERED", "1");   // log lines (Quick Connect code) arrive at once
        m_proc.setProcessEnvironment(env);
        m_proc.setStandardInputFile(QProcess::nullDevice());
        m_log.clear();
        QDir().mkpath(QFileInfo(logPath()).absolutePath());
        m_logFile.close();
        m_logFile.setFileName(logPath());
        if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
            m_logFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        m_proc.start("jellyfin-mpv-shim", QStringList{"--config", confDir(), "--no-gui"} + extra);
        if (!m_loggingIn) status("starting…");
        m_retry.start();
    }

    // conf.json: our keys on top of whatever the shim keeps there (client_uuid, its defaults).
    void writeConf() {
        QDir().mkpath(confDir());
        const QString path = confDir() + "/conf.json";
        QJsonObject c;
        {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) c = QJsonDocument::fromJson(f.readAll()).object();
        }
        const QJsonObject ours{
            {"player_name", m_name},
            {"mpv_ext", true}, {"mpv_ext_path", m_mpvPath}, {"mpv_ext_ipc", sockPath()},
            {"mpv_ext_start", true}, {"mpv_ext_no_ovr", false},
            {"mpv_idle_quit", false},          // keep mpv (and our IPC view of it) alive while idle
            {"enable_gui", false}, {"osc_style", "none"},   // no library browser (it would draw on the CRT)
            {"start_minimized", true}, {"raise_mpv", false}, {"fullscreen", false},
            {"mpris", false}, {"media_keys", false}, {"discord_presence", false},   // leave those to the main shim
            {"check_updates", false}, {"auto_download_enable", false}, {"thumbnail_enable", false},
            {"shader_pack_enable", false}, {"motion_interpolation", "off"}, {"deband", "off"},
            {"svp_enable", false}, {"input_gamepad", false}, {"hwdec", "no"},
            {"segment_intro", "off"}, {"segment_outro", "off"},   // "ask" would wait for a key on the CRT
        };
        for (auto it = ours.begin(); it != ours.end(); ++it) c.insert(it.key(), it.value());
        for (auto it = m_shimSettings.begin(); it != m_shimSettings.end(); ++it) c.insert(it.key(), it.value());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(c).toJson());
    }

    void writeMpvConf() {
        QDir().mkpath(confDir());
        QFile f(confDir() + "/mpv.conf");
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
        QByteArray s = "# written by misterMPV on every receiver start - edit the settings in misterMPV instead\n"
                       "vo=mister\nao=mister\n";
        for (const QString& l : m_options) {   // %len%value: values may hold '#' (sub-color) or spaces
            const int eq = l.indexOf('=');
            if (eq <= 0) continue;
            const QByteArray v = l.mid(eq + 1).toUtf8();
            s += l.left(eq).toUtf8() + "=%" + QByteArray::number(v.size()) + "%" + v + "\n";
        }
        s += "# idle receiver: no VO, so nothing is sent to the MiSTer until a file plays\n"
             "force-window=no\n"
             "[mister-active]\nprofile-cond=not idle_active\nprofile-restore=copy\nforce-window=yes\n";
        f.write(s);
    }

    void onLog() {
        const QByteArray chunk = m_proc.readAll();
        if (m_logFile.isOpen()) { m_logFile.write(chunk); m_logFile.flush(); }
        m_log += chunk;
        int nl;
        static const QRegularExpression codeRe("^\\s{4}(\\S{4,12})\\s*$");
        while ((nl = m_log.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(m_log.left(nl));
            m_log.remove(0, nl + 1);
            if (line.trimmed().isEmpty()) continue;
            if (m_loggingIn) {
                const auto m = codeRe.match(line);
                if (m.hasMatch()) { status("Quick Connect code: " + m.captured(1)); if (onLoginCode) onLoginCode(m.captured(1)); }
                else if (line.contains("Successfully added server")) { m_loggingIn = false; readyStatus(); }
                else if (line.contains("Quick Connect") && line.contains("WARNING")) status(line.section("] ", 1).left(160));
            }
        }
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
            if (n == "idle-active") setActive(!v.toBool(true));
            else if (n == "media-title") { m_title = v.toString(); if (m_active && onState) onState(true, m_title, m_paused); }
            else if (n == "pause") { m_paused = v.toBool(); if (m_active && onState) onState(true, m_title, m_paused); }
        }
    }

    void setActive(bool a) {
        if (a == m_active) return;
        m_active = a;
        if (!a) { m_title.clear(); m_paused = false; }
        if (onState) onState(a, m_title, m_paused);
    }

    QProcess m_proc;
    QLocalSocket m_sock;
    QTimer m_retry, m_restart;
    QByteArray m_buf, m_log;
    QString m_mpvPath, m_name = "MiSTer", m_title, m_status;
    QStringList m_options;
    QJsonObject m_shimSettings;
    QFile m_logFile;
    bool m_ipcUp = false;
    bool m_enabled = false, m_active = false, m_paused = false, m_stopping = false, m_loggingIn = false;
};
