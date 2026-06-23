#pragma once

#include <QObject>
#include <QHash>
#include <QTimer>
#include <QString>
#include <QVector>

#include <cstdint>
#include <vector>

extern "C" {
#include "lan_chat/core/protocol.h"
#include "lan_chat/core/status.h"
#include "lan_chat/transport/transport.h"
}

struct OnlineUser {
    quint64 userId{0};
    QString username;
    QString nickname;
    bool online{false};
};

struct CallSignal {
    QString operation;
    quint64 callId{0};
    quint64 peerId{0};
    QString mediaMode;
    QString sdp;
    QString iceCandidate;
    QString iceMid;
    quint32 iceMlineIndex{0};
    bool hasIceMlineIndex{false};
};

class GuiClient final : public QObject {
    Q_OBJECT

public:
    explicit GuiClient(QObject *parent = nullptr);
    ~GuiClient() override;

    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] bool isAuthenticated() const;
    [[nodiscard]] quint64 userId() const;
    [[nodiscard]] quint64 sessionId() const;
    [[nodiscard]] QString username() const;

public slots:
    void registerAccount(
        const QString &host,
        quint16 port,
        const QString &username,
        const QString &password,
        const QString &nickname);
    void login(const QString &host, quint16 port, const QString &username, const QString &password);
    void disconnectFromServer();
    void requestOnlineUsers();
    void invite(quint64 peerId);
    void accept(quint64 callId);
    void reject(quint64 callId);
    void hangup(quint64 callId);
    void sendOffer(quint64 callId, const QString &sdp);
    void sendAnswer(quint64 callId, const QString &sdp);
    void sendIce(quint64 callId, const QString &candidate, const QString &mid, quint32 mlineIndex);

signals:
    void connectionStateChanged(bool connected);
    void authenticated(quint64 userId, quint64 sessionId, const QString &username);
    void authenticationFailed(const QString &message);
    void onlineUsersUpdated(const QVector<OnlineUser> &users);
    void callInviteSent(quint64 callId, quint64 peerId);
    void callOperationSucceeded(const QString &operation, quint64 callId);
    void callOperationFailed(const QString &operation, quint64 callId, const QString &message);
    void incomingCall(const CallSignal &signal);
    void callSignalReceived(const CallSignal &signal);
    void statusMessage(const QString &message);
    void disconnected(const QString &reason);

private slots:
    void pollIo();

private:
    struct PendingRequest {
        QString group;
        QString operation;
        quint64 peerId{0};
        quint64 callId{0};
    };

    bool connectToServer(const QString &host, quint16 port);
    bool queueAuth(
        const QString &operation,
        const QString &username,
        const QString &password,
        const QString &nickname,
        quint32 seq);
    bool queueOnlineList(quint32 seq);
    bool queueCall(
        const QString &operation,
        quint64 callId,
        quint64 peerId,
        const QString &mediaMode,
        const QString &sdp,
        const QString &iceCandidate,
        const QString &iceMid,
        quint32 iceMlineIndex,
        bool hasIceMlineIndex,
        quint32 seq);
    bool queuePacket(
        quint16 group,
        quint16 type,
        quint16 flags,
        quint32 seq,
        quint64 receiverId,
        const std::uint8_t *body,
        size_t bodyLen,
        const QString &label);
    bool flushConnection();
    void processPacket(const std::vector<std::uint8_t> &packet);
    void processAuthResponse(const lan_chat_header_t &header, const std::uint8_t *body, size_t bodyLen);
    void processUserResponse(const lan_chat_header_t &header, const std::uint8_t *body, size_t bodyLen);
    void processCallResponse(const lan_chat_header_t &header, const std::uint8_t *body, size_t bodyLen);
    void processCallNotify(const lan_chat_header_t &header, const std::uint8_t *body, size_t bodyLen);
    void failAndClose(const QString &reason);
    void resetAuth();
    quint32 nextSeq();

    lan_chat_transport_t transport_{};
    lan_chat_tcp_connection_t connection_{};
    bool transportInitialized_{false};
    bool connected_{false};
    bool authenticated_{false};
    quint64 userId_{0};
    quint64 sessionId_{0};
    QString username_;
    QString pendingUsername_;
    QString connectedHost_;
    quint16 connectedPort_{0};
    quint32 nextSeq_{1};
    QHash<quint32, PendingRequest> pending_;
    QTimer ioTimer_;
};

Q_DECLARE_METATYPE(OnlineUser)
Q_DECLARE_METATYPE(CallSignal)
