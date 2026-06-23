#include <QApplication>
#include <QComboBox>
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
        resize(760, 520);

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
        password_->setEchoMode(QLineEdit::Password);
        connection_layout->addRow(QStringLiteral("Host"), host_);
        connection_layout->addRow(QStringLiteral("Port"), port_);
        connection_layout->addRow(QStringLiteral("Username"), username_);
        connection_layout->addRow(QStringLiteral("Password"), password_);

        auto *auth_buttons = new QHBoxLayout();
        register_button_ = new QPushButton(QStringLiteral("Register"), connection);
        login_button_ = new QPushButton(QStringLiteral("Login"), connection);
        auth_buttons->addWidget(register_button_);
        auth_buttons->addWidget(login_button_);
        connection_layout->addRow(auth_buttons);
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

        connect(register_button_, &QPushButton::clicked, this, [this]() {
            statusBar()->showMessage(QStringLiteral("Register flow is reserved for Phase 7 protocol wiring"));
        });
        connect(login_button_, &QPushButton::clicked, this, [this]() {
            statusBar()->showMessage(QStringLiteral("Login flow is reserved for Phase 7 protocol wiring"));
        });
        connect(refresh_button_, &QPushButton::clicked, this, [this]() {
            online_users_->clear();
            statusBar()->showMessage(QStringLiteral("Online user list protocol is implemented on the server; GUI wiring follows"));
        });
        connect(invite_button_, &QPushButton::clicked, this, [this]() {
            call_state_->setText(QStringLiteral("Outgoing invite pending"));
        });
        connect(accept_button_, &QPushButton::clicked, this, [this]() {
            call_state_->setText(QStringLiteral("Call accepted"));
        });
        connect(reject_button_, &QPushButton::clicked, this, [this]() {
            call_state_->setText(QStringLiteral("Call rejected"));
        });
        connect(hangup_button_, &QPushButton::clicked, this, [this]() {
            call_state_->setText(QStringLiteral("Idle"));
        });
        connect(mute_button_, &QPushButton::toggled, this, [this](bool muted) {
            mute_button_->setText(muted ? QStringLiteral("Unmute") : QStringLiteral("Mute"));
        });
    }

private:
    QLineEdit *host_{nullptr};
    QSpinBox *port_{nullptr};
    QLineEdit *username_{nullptr};
    QLineEdit *password_{nullptr};
    QPushButton *register_button_{nullptr};
    QPushButton *login_button_{nullptr};
    QPushButton *refresh_button_{nullptr};
    QListWidget *online_users_{nullptr};
    QLabel *call_state_{nullptr};
    QLabel *media_state_{nullptr};
    QPushButton *invite_button_{nullptr};
    QPushButton *accept_button_{nullptr};
    QPushButton *reject_button_{nullptr};
    QPushButton *hangup_button_{nullptr};
    QPushButton *mute_button_{nullptr};
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
