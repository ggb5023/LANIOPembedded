#include "gui_client.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <cstdlib>
#include <iostream>
#include <memory>

extern "C" {
#include "lan_chat/server/server.h"
#include "lan_chat/storage/memory_storage.h"
}

namespace {

void require_true(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void require_status(lan_chat_status_t actual, lan_chat_status_t expected, const char *message)
{
    if (actual != expected) {
        std::cerr << message << ": expected " << lan_chat_status_name(expected)
                  << " got " << lan_chat_status_name(actual) << '\n';
        std::exit(1);
    }
}

class TestServer final {
public:
    TestServer()
    {
        require_status(lan_chat_storage_memory_open(&storage_), LAN_CHAT_STATUS_OK, "memory storage open");
        lan_chat_server_config_t config{};
        config.listen_host = "127.0.0.1";
        config.listen_port = 0;
        config.worker_thread_count = 1;
        config.storage = storage_;
        require_status(lan_chat_server_init(&server_, &config), LAN_CHAT_STATUS_OK, "server init");
    }

    ~TestServer()
    {
        lan_chat_server_shutdown(&server_);
        lan_chat_storage_close(storage_);
        storage_ = nullptr;
    }

    quint16 port() const
    {
        return lan_chat_server_port(&server_);
    }

    void pump()
    {
        require_status(lan_chat_server_run_once(&server_), LAN_CHAT_STATUS_OK, "server run once");
    }

private:
    lan_chat_storage_t *storage_{nullptr};
    lan_chat_server_t server_{};
};

bool wait_until(TestServer *server, int timeoutMs, const std::function<bool()> &predicate)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        server->pump();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        if (predicate()) {
            return true;
        }
        QThread::msleep(1);
    }
    return predicate();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestServer server;

    GuiClient alice;
    GuiClient bob;

    quint64 aliceId = 0;
    quint64 bobId = 0;
    QVector<OnlineUser> aliceUsers;
    quint64 aliceCallId = 0;
    quint64 bobIncomingCallId = 0;
    bool bobSawInvite = false;
    bool aliceSawAccept = false;
    bool bobSawOffer = false;
    bool aliceSawAnswer = false;
    bool bobSawIce = false;
    bool aliceSawHangup = false;
    QString failure;

    QObject::connect(&alice, &GuiClient::authenticated, [&](quint64 userId, quint64, const QString &) {
        aliceId = userId;
    });
    QObject::connect(&bob, &GuiClient::authenticated, [&](quint64 userId, quint64, const QString &) {
        bobId = userId;
    });
    QObject::connect(&alice, &GuiClient::onlineUsersUpdated, [&](const QVector<OnlineUser> &users) {
        aliceUsers = users;
    });
    QObject::connect(&alice, &GuiClient::callInviteSent, [&](quint64 callId, quint64 peerId) {
        aliceCallId = callId;
        require_true(peerId == bobId, "invite peer mismatch");
    });
    QObject::connect(&bob, &GuiClient::incomingCall, [&](const CallSignal &signal) {
        bobIncomingCallId = signal.callId;
        bobSawInvite = signal.peerId == aliceId && signal.mediaMode == QStringLiteral("audio");
    });
    QObject::connect(&alice, &GuiClient::callSignalReceived, [&](const CallSignal &signal) {
        if (signal.operation == QStringLiteral("accept")) {
            aliceSawAccept = signal.peerId == bobId;
        } else if (signal.operation == QStringLiteral("answer")) {
            aliceSawAnswer = signal.sdp == QStringLiteral("answer-sdp");
        } else if (signal.operation == QStringLiteral("hangup")) {
            aliceSawHangup = signal.peerId == bobId;
        }
    });
    QObject::connect(&bob, &GuiClient::callSignalReceived, [&](const CallSignal &signal) {
        if (signal.operation == QStringLiteral("offer")) {
            bobSawOffer = signal.sdp == QStringLiteral("offer-sdp");
        } else if (signal.operation == QStringLiteral("ice")) {
            bobSawIce = signal.iceCandidate == QStringLiteral("candidate-a") && signal.iceMid == QStringLiteral("0");
        }
    });
    QObject::connect(&alice, &GuiClient::authenticationFailed, [&](const QString &message) {
        failure = message;
    });
    QObject::connect(&bob, &GuiClient::authenticationFailed, [&](const QString &message) {
        failure = message;
    });
    QObject::connect(&alice, &GuiClient::callOperationFailed, [&](const QString &operation, quint64, const QString &message) {
        failure = operation + QStringLiteral(": ") + message;
    });
    QObject::connect(&bob, &GuiClient::callOperationFailed, [&](const QString &operation, quint64, const QString &message) {
        failure = operation + QStringLiteral(": ") + message;
    });

    const QString host = QStringLiteral("127.0.0.1");
    alice.registerAccount(host, server.port(), QStringLiteral("gui_alice"), QStringLiteral("password"), QStringLiteral("Alice"));
    bob.registerAccount(host, server.port(), QStringLiteral("gui_bob"), QStringLiteral("password"), QStringLiteral("Bob"));

    require_true(wait_until(&server, 5000, [&]() { return aliceId != 0 && bobId != 0; }), "auth timeout");
    require_true(failure.isEmpty(), failure.toUtf8().constData());

    alice.requestOnlineUsers();
    require_true(wait_until(&server, 5000, [&]() {
        for (const OnlineUser &user : aliceUsers) {
            if (user.userId == bobId && user.username == QStringLiteral("gui_bob")) {
                return true;
            }
        }
        return false;
    }), "online-list timeout");

    alice.invite(bobId);
    require_true(wait_until(&server, 5000, [&]() {
        return aliceCallId != 0 && bobSawInvite && bobIncomingCallId == aliceCallId;
    }), "invite timeout");

    bob.accept(bobIncomingCallId);
    require_true(wait_until(&server, 5000, [&]() { return aliceSawAccept; }), "accept timeout");

    alice.sendOffer(aliceCallId, QStringLiteral("offer-sdp"));
    require_true(wait_until(&server, 5000, [&]() { return bobSawOffer; }), "offer timeout");

    bob.sendAnswer(bobIncomingCallId, QStringLiteral("answer-sdp"));
    require_true(wait_until(&server, 5000, [&]() { return aliceSawAnswer; }), "answer timeout");

    alice.sendIce(aliceCallId, QStringLiteral("candidate-a"), QStringLiteral("0"), 0);
    require_true(wait_until(&server, 5000, [&]() { return bobSawIce; }), "ice timeout");

    bob.hangup(bobIncomingCallId);
    require_true(wait_until(&server, 5000, [&]() { return aliceSawHangup; }), "hangup timeout");

    alice.disconnectFromServer();
    bob.disconnectFromServer();
    std::cout << "GUI_CLIENT_E2E_OK\n";
    return 0;
}
