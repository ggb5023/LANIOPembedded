#include "audio_call_session.hpp"
#include "gui_client.hpp"

#include <QApplication>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>

namespace {

class ChatGuiWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit ChatGuiWindow(QWidget *parent = nullptr)
        : QMainWindow(parent)
    {
        setWindowTitle(QStringLiteral("LAN Chat Audio"));
        resize(820, 560);

        auto *root = new QWidget(this);
        auto *layout = new QVBoxLayout(root);

        auto *connection = new QGroupBox(QStringLiteral("Server and account"), root);
        auto *connection_layout = new QFormLayout(connection);
        host_ = new QLineEdit(QStringLiteral("127.0.0.1"), connection);
        port_ = new QSpinBox(connection);
        port_->setRange(1, 65535);
        port_->setValue(7777);
        username_ = new QLineEdit(connection);
        password_ = new QLineEdit(connection);
        nickname_ = new QLineEdit(connection);
        password_->setEchoMode(QLineEdit::Password);
        connection_layout->addRow(QStringLiteral("Host"), host_);
        connection_layout->addRow(QStringLiteral("Port"), port_);
        connection_layout->addRow(QStringLiteral("Username"), username_);
        connection_layout->addRow(QStringLiteral("Password"), password_);
        connection_layout->addRow(QStringLiteral("Nickname"), nickname_);

        auto *auth_buttons = new QHBoxLayout();
        register_button_ = new QPushButton(QStringLiteral("Register"), connection);
        login_button_ = new QPushButton(QStringLiteral("Login"), connection);
        disconnect_button_ = new QPushButton(QStringLiteral("Disconnect"), connection);
        auth_buttons->addWidget(register_button_);
        auth_buttons->addWidget(login_button_);
        auth_buttons->addWidget(disconnect_button_);
        connection_layout->addRow(auth_buttons);
        auth_state_ = new QLabel(QStringLiteral("Disconnected"), connection);
        connection_layout->addRow(QStringLiteral("State"), auth_state_);
        layout->addWidget(connection);

        auto *users = new QGroupBox(QStringLiteral("Online users"), root);
        auto *users_layout = new QVBoxLayout(users);
        online_users_ = new QListWidget(users);
        refresh_button_ = new QPushButton(QStringLiteral("Refresh online users"), users);
        users_layout->addWidget(online_users_);
        users_layout->addWidget(refresh_button_);
        layout->addWidget(users, 1);

        auto *call = new QGroupBox(QStringLiteral("Audio call"), root);
        auto *call_layout = new QVBoxLayout(call);
        call_state_ = new QLabel(QStringLiteral("Idle"), call);
        media_state_ = new QLabel(
#if LAN_CHAT_GUI_ENABLE_AV
            QStringLiteral("AV media enabled")
#else
            QStringLiteral("AV media disabled in this build")
#endif
                ,
            call);
        auto *call_buttons = new QHBoxLayout();
        invite_button_ = new QPushButton(QStringLiteral("Invite"), call);
        accept_button_ = new QPushButton(QStringLiteral("Accept"), call);
        reject_button_ = new QPushButton(QStringLiteral("Reject"), call);
        hangup_button_ = new QPushButton(QStringLiteral("Hangup"), call);
        mute_button_ = new QPushButton(QStringLiteral("Mute"), call);
        mute_button_->setCheckable(true);
        call_buttons->addWidget(invite_button_);
        call_buttons->addWidget(accept_button_);
        call_buttons->addWidget(reject_button_);
        call_buttons->addWidget(hangup_button_);
        call_buttons->addWidget(mute_button_);
        call_layout->addWidget(call_state_);
        call_layout->addWidget(media_state_);
        call_layout->addLayout(call_buttons);
        layout->addWidget(call);

        setCentralWidget(root);
        statusBar()->showMessage(QStringLiteral("Ready"));
        wireSignals();
        updateControls();
    }

private:
    void wireSignals()
    {
        connect(register_button_, &QPushButton::clicked, this, [this]() {
            if (!beginAuth()) {
                return;
            }
            client_.registerAccount(
                host_->text().trimmed(),
                static_cast<quint16>(port_->value()),
                username_->text().trimmed(),
                password_->text(),
                nickname_->text().trimmed());
        });
        connect(login_button_, &QPushButton::clicked, this, [this]() {
            if (!beginAuth()) {
                return;
            }
            client_.login(
                host_->text().trimmed(),
                static_cast<quint16>(port_->value()),
                username_->text().trimmed(),
                password_->text());
        });
        connect(disconnect_button_, &QPushButton::clicked, &client_, &GuiClient::disconnectFromServer);
        connect(refresh_button_, &QPushButton::clicked, &client_, &GuiClient::requestOnlineUsers);
        connect(invite_button_, &QPushButton::clicked, this, [this]() {
            const quint64 peerId = selectedPeerId();
            if (peerId == 0) {
                statusBar()->showMessage(QStringLiteral("Select an online user first"));
                return;
            }
            activePeerId_ = peerId;
            call_state_->setText(QStringLiteral("Calling user %1").arg(peerId));
            client_.invite(peerId);
            updateControls();
        });
        connect(accept_button_, &QPushButton::clicked, this, [this]() {
            if (activeCallId_ == 0) {
                return;
            }
            audio_.startCallee();
            client_.accept(activeCallId_);
            call_state_->setText(QStringLiteral("Accepting call %1").arg(activeCallId_));
            updateControls();
        });
        connect(reject_button_, &QPushButton::clicked, this, [this]() {
            if (activeCallId_ == 0) {
                return;
            }
            client_.reject(activeCallId_);
            audio_.stop();
            resetCallState(QStringLiteral("Call rejected"));
        });
        connect(hangup_button_, &QPushButton::clicked, this, [this]() {
            if (activeCallId_ != 0) {
                client_.hangup(activeCallId_);
            }
            audio_.stop();
            resetCallState(QStringLiteral("Idle"));
        });
        connect(mute_button_, &QPushButton::toggled, this, [this](bool muted) {
            mute_button_->setText(muted ? QStringLiteral("Unmute") : QStringLiteral("Mute"));
            audio_.setMicrophoneMuted(muted);
        });

        connect(&client_, &GuiClient::connectionStateChanged, this, [this](bool connected) {
            auth_state_->setText(connected ? QStringLiteral("Connected") : QStringLiteral("Disconnected"));
            updateControls();
        });
        connect(&client_, &GuiClient::authenticated, this, [this](quint64 userId, quint64 sessionId, const QString &name) {
            auth_state_->setText(QStringLiteral("Logged in as %1 user_id=%2 session_id=%3").arg(name).arg(userId).arg(sessionId));
            statusBar()->showMessage(QStringLiteral("Login ok"));
            updateControls();
        });
        connect(&client_, &GuiClient::authenticationFailed, this, [this](const QString &message) {
            auth_state_->setText(QStringLiteral("Auth failed"));
            statusBar()->showMessage(message);
            updateControls();
        });
        connect(&client_, &GuiClient::onlineUsersUpdated, this, &ChatGuiWindow::setOnlineUsers);
        connect(&client_, &GuiClient::callInviteSent, this, [this](quint64 callId, quint64 peerId) {
            activeCallId_ = callId;
            activePeerId_ = peerId;
            incomingPending_ = false;
            call_state_->setText(QStringLiteral("Outgoing call %1 to user %2").arg(callId).arg(peerId));
            audio_.startCaller();
            updateControls();
        });
        connect(&client_, &GuiClient::callOperationSucceeded, this, [this](const QString &operation, quint64 callId) {
            if (operation == QStringLiteral("accept")) {
                call_state_->setText(QStringLiteral("Call %1 accepted").arg(callId));
            } else if (operation == QStringLiteral("hangup")) {
                resetCallState(QStringLiteral("Idle"));
            } else if (operation == QStringLiteral("reject")) {
                resetCallState(QStringLiteral("Call rejected"));
            }
            updateControls();
        });
        connect(&client_, &GuiClient::callOperationFailed, this, [this](const QString &operation, quint64, const QString &message) {
            statusBar()->showMessage(QStringLiteral("%1 failed: %2").arg(operation, message));
            if (operation == QStringLiteral("invite")) {
                resetCallState(QStringLiteral("Idle"));
            }
            updateControls();
        });
        connect(&client_, &GuiClient::incomingCall, this, [this](const CallSignal &signal) {
            activeCallId_ = signal.callId;
            activePeerId_ = signal.peerId;
            incomingPending_ = true;
            call_state_->setText(QStringLiteral("Incoming audio call %1 from user %2").arg(signal.callId).arg(signal.peerId));
            updateControls();
        });
        connect(&client_, &GuiClient::callSignalReceived, this, &ChatGuiWindow::handleCallSignal);
        connect(&client_, &GuiClient::statusMessage, this, [this](const QString &message) {
            statusBar()->showMessage(message);
        });
        connect(&client_, &GuiClient::disconnected, this, [this](const QString &reason) {
            online_users_->clear();
            audio_.stop();
            resetCallState(QStringLiteral("Idle"));
            auth_state_->setText(QStringLiteral("Disconnected"));
            statusBar()->showMessage(reason);
            updateControls();
        });

        connect(&audio_, &AudioCallSession::localOfferReady, this, [this](const QString &sdp) {
            if (activeCallId_ != 0) {
                client_.sendOffer(activeCallId_, sdp);
            }
        });
        connect(&audio_, &AudioCallSession::localAnswerReady, this, [this](const QString &sdp) {
            if (activeCallId_ != 0) {
                client_.sendAnswer(activeCallId_, sdp);
            }
        });
        connect(&audio_, &AudioCallSession::localIceReady, this, [this](const QString &candidate, const QString &mid, int mlineIndex) {
            if (activeCallId_ != 0) {
                client_.sendIce(activeCallId_, candidate, mid, static_cast<quint32>(mlineIndex));
            }
        });
        connect(&audio_, &AudioCallSession::stateChanged, this, [this](const QString &state) {
            media_state_->setText(QStringLiteral("Media: %1").arg(state));
        });
        connect(&audio_, &AudioCallSession::errorOccurred, this, [this](const QString &message) {
            media_state_->setText(QStringLiteral("Media: %1").arg(message));
        });
    }

    bool beginAuth()
    {
        if (username_->text().trimmed().isEmpty() || password_->text().isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Username and password are required"));
            return false;
        }
        auth_state_->setText(QStringLiteral("Connecting"));
        statusBar()->showMessage(QStringLiteral("Connecting"));
        return true;
    }

    void setOnlineUsers(const QVector<OnlineUser> &users)
    {
        online_users_->clear();
        for (const OnlineUser &user : users) {
            const QString displayName = user.nickname.isEmpty() ? user.username : user.nickname;
            auto *item = new QListWidgetItem(
                QStringLiteral("%1  user_id=%2  username=%3").arg(displayName).arg(user.userId).arg(user.username),
                online_users_);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(user.userId));
        }
        statusBar()->showMessage(QStringLiteral("Online users: %1").arg(users.size()));
        updateControls();
    }

    void handleCallSignal(const CallSignal &signal)
    {
        if (signal.operation == QStringLiteral("invite")) {
            return;
        }
        if (signal.operation == QStringLiteral("accept")) {
            activeCallId_ = signal.callId;
            activePeerId_ = signal.peerId;
            incomingPending_ = false;
            call_state_->setText(QStringLiteral("Call %1 accepted by user %2").arg(signal.callId).arg(signal.peerId));
        } else if (signal.operation == QStringLiteral("reject")) {
            audio_.stop();
            resetCallState(QStringLiteral("Call rejected by peer"));
        } else if (signal.operation == QStringLiteral("hangup")) {
            audio_.stop();
            resetCallState(QStringLiteral("Peer hung up"));
        } else if (signal.operation == QStringLiteral("offer")) {
            activeCallId_ = signal.callId;
            activePeerId_ = signal.peerId;
            audio_.applyRemoteOffer(signal.sdp);
            call_state_->setText(QStringLiteral("Applying remote offer from user %1").arg(signal.peerId));
        } else if (signal.operation == QStringLiteral("answer")) {
            audio_.applyRemoteAnswer(signal.sdp);
            call_state_->setText(QStringLiteral("Applying remote answer from user %1").arg(signal.peerId));
        } else if (signal.operation == QStringLiteral("ice")) {
            audio_.applyRemoteIce(signal.iceCandidate, signal.iceMid, static_cast<int>(signal.iceMlineIndex));
        }
        updateControls();
    }

    quint64 selectedPeerId() const
    {
        const QListWidgetItem *item = online_users_->currentItem();
        if (item == nullptr) {
            return 0;
        }
        return item->data(Qt::UserRole).toULongLong();
    }

    void resetCallState(const QString &state)
    {
        activeCallId_ = 0;
        activePeerId_ = 0;
        incomingPending_ = false;
        call_state_->setText(state);
        updateControls();
    }

    void updateControls()
    {
        const bool authed = client_.isAuthenticated();
        register_button_->setEnabled(!authed);
        login_button_->setEnabled(!authed);
        disconnect_button_->setEnabled(client_.isConnected());
        refresh_button_->setEnabled(authed);
        invite_button_->setEnabled(authed && activeCallId_ == 0 && selectedPeerId() != 0);
        accept_button_->setEnabled(authed && incomingPending_ && activeCallId_ != 0);
        reject_button_->setEnabled(authed && incomingPending_ && activeCallId_ != 0);
        hangup_button_->setEnabled(authed && activeCallId_ != 0);
        mute_button_->setEnabled(authed && activeCallId_ != 0);
    }

    GuiClient client_;
    AudioCallSession audio_;
    QLineEdit *host_{nullptr};
    QSpinBox *port_{nullptr};
    QLineEdit *username_{nullptr};
    QLineEdit *password_{nullptr};
    QLineEdit *nickname_{nullptr};
    QLabel *auth_state_{nullptr};
    QPushButton *register_button_{nullptr};
    QPushButton *login_button_{nullptr};
    QPushButton *disconnect_button_{nullptr};
    QPushButton *refresh_button_{nullptr};
    QListWidget *online_users_{nullptr};
    QLabel *call_state_{nullptr};
    QLabel *media_state_{nullptr};
    QPushButton *invite_button_{nullptr};
    QPushButton *accept_button_{nullptr};
    QPushButton *reject_button_{nullptr};
    QPushButton *hangup_button_{nullptr};
    QPushButton *mute_button_{nullptr};
    quint64 activeCallId_{0};
    quint64 activePeerId_{0};
    bool incomingPending_{false};
};

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    ChatGuiWindow window;
    window.show();
    return app.exec();
}

#include "main.moc"
