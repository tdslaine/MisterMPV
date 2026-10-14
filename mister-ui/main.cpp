// Small Qt front-end for the mpv MiSTer fork: starts mpv with --vo=mister --ao=mister and
// drives it over mpv's JSON IPC (play/pause, seek, tracks, modeline).
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
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
        fileRow->addWidget(m_file);
        fileRow->addWidget(browse);
        root->addLayout(fileRow);

        auto* form = new QFormLayout;
        m_host = new QLineEdit(m_set.value("host").toString());
        m_modeline = new QComboBox;
        m_modeline->addItem("auto (match source)");
        for (const char* k : kModelineKeys) m_modeline->addItem(k);
        m_modeline->setCurrentText(m_set.value("modeline", m_modeline->itemText(0)).toString());
        m_near = new QComboBox;
        m_near->addItems({"lossless", "near 1", "near 2", "near 3"});
        m_near->setCurrentIndex(m_set.value("near", 1).toInt());
        m_audio = new QComboBox;
        m_sub = new QComboBox;
        form->addRow("MiSTer:", m_host);
        form->addRow("Modeline:", m_modeline);
        form->addRow("Codec:", m_near);
        form->addRow("Audio:", m_audio);
        form->addRow("Subtitles:", m_sub);
        root->addLayout(form);

        m_loop = new QCheckBox("Loop");
        root->addWidget(m_loop);

        m_seek = new QSlider(Qt::Horizontal);
        m_seek->setRange(0, 1000);
        m_time = new QLabel("0:00 / 0:00");
        m_time->setAlignment(Qt::AlignCenter);
        root->addWidget(m_seek);
        root->addWidget(m_time);

        auto* btns = new QHBoxLayout;
        m_play = new QPushButton("▶ Play");
        m_pause = new QPushButton("⏸");
        auto* back = new QPushButton("-10s");
        auto* fwd = new QPushButton("+10s");
        btns->addWidget(m_play, 1);
        btns->addWidget(back);
        btns->addWidget(m_pause);
        btns->addWidget(fwd);
        root->addLayout(btns);
        m_status = new QLabel;
        root->addWidget(m_status);
        setControlsEnabled(false);

        connect(browse, &QPushButton::clicked, this, [this] {
            const QString f = QFileDialog::getOpenFileName(this, "Open media");
            if (!f.isEmpty()) m_file->setText(f);
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
        for (auto* w : {(QWidget*)m_pause, (QWidget*)m_seek, (QWidget*)m_audio, (QWidget*)m_sub})
            w->setEnabled(on);
    }

    QString mpvPath() const {
        const QString env = qEnvironmentVariable("MPV_MISTER");
        if (!env.isEmpty()) return env;
        const QString rel = QCoreApplication::applicationDirPath() + "/../../build/mpv";
        return QFileInfo::exists(rel) ? rel : "mpv";
    }

    void start() {
        if (m_file->text().trimmed().isEmpty()) return;
        m_set.setValue("host", m_host->text());
        m_set.setValue("modeline", m_modeline->currentText());
        m_set.setValue("near", m_near->currentIndex());
        const QString ml = m_modeline->currentIndex() == 0 ? "auto" : m_modeline->currentText();
        m_sockPath = QDir::temp().filePath(QString("mpv-mister-%1.sock").arg(QCoreApplication::applicationPid()));
        QFile::remove(m_sockPath);
        QStringList a = {"--no-config", "--vo=mister", "--ao=mister",
                         "--vo-mister-host=" + m_host->text(), "--vo-mister-modeline=" + ml,
                         "--vo-mister-near=" + QString::number(m_near->currentIndex()),
                         "--ao-mister-host=" + m_host->text(), "--ao-mister-modeline=" + ml,
                         "--ao-mister-near=" + QString::number(m_near->currentIndex()),
                         "--input-ipc-server=" + m_sockPath, "--no-terminal", "--idle=no",
                         "--sub-auto=fuzzy"};
        if (m_loop->isChecked()) a << "--loop-file=inf";
        a << m_file->text().trimmed();
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
        int id = 1;
        for (const char* p : {"time-pos", "duration", "pause", "track-list"})
            cmd({"observe_property", id++, p});
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
    QComboBox *m_modeline, *m_near, *m_audio, *m_sub;
    QCheckBox* m_loop;
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
