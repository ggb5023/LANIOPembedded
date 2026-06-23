#include "gui_client.hpp"

#include <QByteArray>

#include <algorithm>
#include <cstring>

extern "C" {
#include "lan_chat/core/endian.h"
#include "lan_chat/core/tlv.h"
}

namespace {

enum {
    kAuthFieldOperation = 1,
    kAuthFieldUsername = 2,
    kAuthFieldPassword = 3,
    kAuthFieldNickname = 4,
    kResponseFieldStatus = 1,
    kResponseFieldUserId = 2,
    kResponseFieldSessionId = 3,
    kResponseFieldMessage = 4,
    kUserFieldOperation = 1,
    kUserFieldCount = 2,
    kUserFieldUsersBlob = 3,
    kCallFieldOperation = 1,
    kCallFieldCallId = 2,
    kCallFieldPeerId = 3,
    kCallFieldMediaMode = 4,
    kCallFieldSdp = 5,
    kCallFieldIceCandidate = 6,
    kCallFieldIceMid = 7,
    kCallFieldIceMlineIndex = 8
};

QString statusName(lan_chat_status_t status)
{
    return QString::fromLatin1(lan_chat_status_name(status));
}

QString formatStatusMessage(lan_chat_status_t status, const QString &message)
{
    if (!message.isEmpty()) {
        return QStringLiteral("%1 (%2)").arg(message, statusName(status));
    }
    return statusName(status);
}

std::string toUtf8Std(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

QString tlvString(const lan_chat_tlv_t &tlv)
{
    return QString::fromUtf8(reinterpret_cast<const char *>(tlv.value), static_cast<qsizetype>(tlv.length));
}

quint16 tlvU16(const lan_chat_tlv_t &tlv, bool *ok)
{
    std::uint16_t value = 0;
    if (tlv.length != 2 || lan_chat_read_u16_be(tlv.value, tlv.length, 0, &value) != LAN_CHAT_STATUS_OK) {
        *ok = false;
    }
    return value;
}

quint32 tlvU32(const lan_chat_tlv_t &tlv, bool *ok)
{
    std::uint32_t value = 0;
    if (tlv.length != 4 || lan_chat_read_u32_be(tlv.value, tlv.length, 0, &value) != LAN_CHAT_STATUS_OK) {
        *ok = false;
    }
    return value;
}

quint64 tlvU64(const lan_chat_tlv_t &tlv, bool *ok)
{
    std::uint64_t value = 0;
    if (tlv.length != 8 || lan_chat_read_u64_be(tlv.value, tlv.length, 0, &value) != LAN_CHAT_STATUS_OK) {
        *ok = false;
    }
    return value;
}

bool writeString(lan_chat_tlv_writer_t *writer, quint16 field, bool required, const QString &value)
{
    const std::string bytes = toUtf8Std(value);
    return lan_chat_tlv_write_string(writer, field, required ? 1 : 0, bytes.c_str(), bytes.size()) == LAN_CHAT_STATUS_OK;
}

bool parseStatusBody(const std::uint8_t *body, size_t bodyLen, lan_chat_status_t *statusOut, QString *messageOut)
{
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    bool ok = true;
    bool hasStatus = false;

    if (lan_chat_tlv_reader_init(&reader, body, bodyLen) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    lan_chat_status_t status = LAN_CHAT_STATUS_NEED_MORE_DATA;
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        if (tlv.field_id == kResponseFieldStatus) {
            *statusOut = static_cast<lan_chat_status_t>(tlvU16(tlv, &ok));
            hasStatus = true;
        } else if (tlv.field_id == kResponseFieldMessage && messageOut != nullptr) {
            *messageOut = tlvString(tlv);
        }
    }
    return status == LAN_CHAT_STATUS_NEED_MORE_DATA && ok && hasStatus;
}

bool parseUserBlob(const std::uint8_t *blob, size_t blobLen, QVector<OnlineUser> *users)
{
    size_t offset = 0;
    users->clear();
    while (offset < blobLen) {
        if (blobLen - offset < 13) {
            return false;
        }

        OnlineUser user;
        std::uint64_t userId = 0;
        std::uint16_t usernameLen = 0;
        std::uint16_t nicknameLen = 0;
        if (lan_chat_read_u64_be(blob, blobLen, offset, &userId) != LAN_CHAT_STATUS_OK ||
            lan_chat_read_u16_be(blob, blobLen, offset + 8, &usernameLen) != LAN_CHAT_STATUS_OK ||
            lan_chat_read_u16_be(blob, blobLen, offset + 10, &nicknameLen) != LAN_CHAT_STATUS_OK) {
            return false;
        }
        offset += 12;
        if (usernameLen > blobLen - offset) {
            return false;
        }
        user.userId = userId;
        user.username = QString::fromUtf8(reinterpret_cast<const char *>(blob + offset), usernameLen);
        offset += usernameLen;
        if (nicknameLen > blobLen - offset) {
            return false;
        }
        user.nickname = QString::fromUtf8(reinterpret_cast<const char *>(blob + offset), nicknameLen);
        offset += nicknameLen;
        if (blobLen - offset < 1) {
            return false;
        }
        user.online = blob[offset] != 0;
        ++offset;
        users->push_back(user);
    }
    return true;
}

} // namespace

GuiClient::GuiClient(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<OnlineUser>("OnlineUser");
    qRegisterMetaType<CallSignal>("CallSignal");
    ioTimer_.setInterval(20);
    connect(&ioTimer_, &QTimer::timeout, this, &GuiClient::pollIo);
}

GuiClient::~GuiClient()
{
    disconnectFromServer();
}

bool GuiClient::isConnected() const
{
    return connected_;
}

bool GuiClient::isAuthenticated() const
{
    return authenticated_;
}

quint64 GuiClient::userId() const
{
    return userId_;
}

quint64 GuiClient::sessionId() const
{
    return sessionId_;
}

QString GuiClient::username() const
{
    return username_;
}

void GuiClient::registerAccount(
    const QString &host,
    quint16 port,
    const QString &username,
    const QString &password,
    const QString &nickname)
{
    if (!connectToServer(host, port)) {
        return;
    }
    pendingUsername_ = username;
    const quint32 seq = nextSeq();
    pending_.insert(seq, PendingRequest{QStringLiteral("auth"), QStringLiteral("register"), 0, 0});
    if (!queueAuth(QStringLiteral("register"), username, password, nickname, seq)) {
        pending_.remove(seq);
        failAndClose(QStringLiteral("failed to queue register request"));
    }
}

void GuiClient::login(const QString &host, quint16 port, const QString &username, const QString &password)
{
    if (!connectToServer(host, port)) {
        return;
    }
    pendingUsername_ = username;
    const quint32 seq = nextSeq();
    pending_.insert(seq, PendingRequest{QStringLiteral("auth"), QStringLiteral("login"), 0, 0});
    if (!queueAuth(QStringLiteral("login"), username, password, QString(), seq)) {
        pending_.remove(seq);
        failAndClose(QStringLiteral("failed to queue login request"));
    }
}

void GuiClient::disconnectFromServer()
{
    ioTimer_.stop();
    if (connection_.is_open) {
        lan_chat_tcp_connection_close(&connection_);
    }
    if (transportInitialized_) {
        lan_chat_transport_shutdown(&transport_);
        transportInitialized_ = false;
    }
    pending_.clear();
    resetAuth();
    if (connected_) {
        connected_ = false;
        emit connectionStateChanged(false);
        emit disconnected(QStringLiteral("disconnected"));
    }
}

void GuiClient::requestOnlineUsers()
{
    if (!authenticated_) {
        emit statusMessage(QStringLiteral("login required"));
        return;
    }
    const quint32 seq = nextSeq();
    pending_.insert(seq, PendingRequest{QStringLiteral("user"), QStringLiteral("online-list"), 0, 0});
    if (!queueOnlineList(seq)) {
        pending_.remove(seq);
        emit statusMessage(QStringLiteral("failed to queue online-list"));
    }
}

void GuiClient::invite(quint64 peerId)
{
    const quint32 seq = nextSeq();
    pending_.insert(seq, PendingRequest{QStringLiteral("call"), QStringLiteral("invite"), peerId, 0});
    if (!queueCall(QStringLiteral("invite"), 0, peerId, QStringLiteral("audio"), QString(), QString(), QString(), 0, false, seq)) {
        pending_.remove(seq);
        emit callOperationFailed(QStringLiteral("invite"), 0, QStringLiteral("failed to queue invite"));
    }
}

void GuiClient::accept(quint64 callId)
{
    const quint32 seq = nextSeq();
    pending_.insert(seq, PendingRequest{QStringLiteral("call"), QStringLiteral("accept"), 0, callId});
    if (!queueCall(QStringLiteral("accept"), callId, 0, QString(), QString(), QString(), QString(), 0, false, seq)) {
        pending_.remove(seq);
        emit callOperationFailed(QStringLiteral("accept"), callId, QStringLiteral("failed to queue accept"));
    }
}

void GuiClient::reject(quint64 callId)
{
    const quint32 seq = nextSeq();
    pending_.insert(seq, PendingRequest{QStringLiteral("call"), QStringLiteral("reject"), 0, callId});
    if (!queueCall(QStringLiteral("reject"), callId, 0, QString(), QString(), QString(), QString(), 0, false, seq)) {
        pending_.remove(seq);
        emit callOperationFailed(QStringLiteral("reject"), callId, QStringLiteral("failed to queue reject"));
    }
}

void GuiClient::hangup(quint64 callId)
{
    const quint32 seq = nextSeq();
    pending_.insert(seq, PendingRequest{QStringLiteral("call"), QStringLiteral("hangup"), 0, callId});
    if (!queueCall(QStringLiteral("hangup"), callId, 0, QString(), QString(), QString(), QString(), 0, false, seq)) {
        pending_.remove(seq);
        emit callOperationFailed(QStringLiteral("hangup"), callId, QStringLiteral("failed to queue hangup"));
    }
}

void GuiClient::sendOffer(quint64 callId, const QString &sdp)
{
    const quint32 seq = nextSeq();
    pending_.insert(seq, PendingRequest{QStringLiteral("call"), QStringLiteral("offer"), 0, callId});
    if (!queueCall(QStringLiteral("offer"), callId, 0, QString(), sdp, QString(), QString(), 0, false, seq)) {
        pending_.remove(seq);
        emit callOperationFailed(QStringLiteral("offer"), callId, QStringLiteral("failed to queue offer"));
    }
}

void GuiClient::sendAnswer(quint64 callId, const QString &sdp)
{
    const quint32 seq = nextSeq();
    pending_.insert(seq, PendingRequest{QStringLiteral("call"), QStringLiteral("answer"), 0, callId});
    if (!queueCall(QStringLiteral("answer"), callId, 0, QString(), sdp, QString(), QString(), 0, false, seq)) {
        pending_.remove(seq);
        emit callOperationFailed(QStringLiteral("answer"), callId, QStringLiteral("failed to queue answer"));
    }
}

void GuiClient::sendIce(quint64 callId, const QString &candidate, const QString &mid, quint32 mlineIndex)
{
    const quint32 seq = nextSeq();
    pending_.insert(seq, PendingRequest{QStringLiteral("call"), QStringLiteral("ice"), 0, callId});
    if (!queueCall(QStringLiteral("ice"), callId, 0, QString(), QString(), candidate, mid, mlineIndex, true, seq)) {
        pending_.remove(seq);
        emit callOperationFailed(QStringLiteral("ice"), callId, QStringLiteral("failed to queue ice"));
    }
}

void GuiClient::pollIo()
{
    if (!connected_) {
        return;
    }
    if (!flushConnection()) {
        return;
    }

    for (;;) {
        std::vector<std::uint8_t> packet(LAN_CHAT_TRANSPORT_PACKET_BUFFER_SIZE);
        size_t packetLen = 0;
        const lan_chat_status_t status = lan_chat_tcp_connection_recv_packet(
            &connection_,
            packet.data(),
            packet.size(),
            &packetLen);
        if (status == LAN_CHAT_STATUS_NEED_MORE_DATA) {
            return;
        }
        if (status != LAN_CHAT_STATUS_OK) {
            failAndClose(QStringLiteral("receive failed: %1").arg(statusName(status)));
            return;
        }
        packet.resize(packetLen);
        processPacket(packet);
    }
}

bool GuiClient::connectToServer(const QString &host, quint16 port)
{
    if (connected_ && host == connectedHost_ && port == connectedPort_) {
        return true;
    }
    disconnectFromServer();

    lan_chat_status_t status = lan_chat_transport_init(&transport_);
    if (status != LAN_CHAT_STATUS_OK) {
        emit authenticationFailed(QStringLiteral("transport init failed: %1").arg(statusName(status)));
        return false;
    }
    transportInitialized_ = true;

    const std::string hostBytes = toUtf8Std(host);
    status = lan_chat_tcp_client_connect(&connection_, hostBytes.c_str(), port);
    if (status != LAN_CHAT_STATUS_OK) {
        lan_chat_transport_shutdown(&transport_);
        transportInitialized_ = false;
        emit authenticationFailed(QStringLiteral("connect failed: %1").arg(statusName(status)));
        return false;
    }

    connected_ = true;
    connectedHost_ = host;
    connectedPort_ = port;
    nextSeq_ = 1;
    ioTimer_.start();
    emit connectionStateChanged(true);
    emit statusMessage(QStringLiteral("connected to %1:%2").arg(host).arg(port));
    return true;
}

bool GuiClient::queueAuth(
    const QString &operation,
    const QString &username,
    const QString &password,
    const QString &nickname,
    quint32 seq)
{
    std::uint8_t body[512]{};
    lan_chat_tlv_writer_t writer{};
    if (lan_chat_tlv_writer_init(&writer, body, sizeof(body)) != LAN_CHAT_STATUS_OK ||
        !writeString(&writer, kAuthFieldOperation, true, operation) ||
        !writeString(&writer, kAuthFieldUsername, true, username) ||
        !writeString(&writer, kAuthFieldPassword, true, password)) {
        return false;
    }
    if (!nickname.isEmpty() && !writeString(&writer, kAuthFieldNickname, false, nickname)) {
        return false;
    }
    return queuePacket(
        LAN_CHAT_GROUP_AUTH,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        seq,
        0,
        body,
        lan_chat_tlv_writer_size(&writer),
        operation);
}

bool GuiClient::queueOnlineList(quint32 seq)
{
    std::uint8_t body[64]{};
    lan_chat_tlv_writer_t writer{};
    if (lan_chat_tlv_writer_init(&writer, body, sizeof(body)) != LAN_CHAT_STATUS_OK ||
        !writeString(&writer, kUserFieldOperation, true, QStringLiteral("online-list"))) {
        return false;
    }
    return queuePacket(
        LAN_CHAT_GROUP_USER,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        seq,
        0,
        body,
        lan_chat_tlv_writer_size(&writer),
        QStringLiteral("online-list"));
}

bool GuiClient::queueCall(
    const QString &operation,
    quint64 callId,
    quint64 peerId,
    const QString &mediaMode,
    const QString &sdp,
    const QString &iceCandidate,
    const QString &iceMid,
    quint32 iceMlineIndex,
    bool hasIceMlineIndex,
    quint32 seq)
{
    std::vector<std::uint8_t> body(LAN_CHAT_MAX_SIGNALING_TEXT_LEN + 512);
    lan_chat_tlv_writer_t writer{};
    if (lan_chat_tlv_writer_init(&writer, body.data(), body.size()) != LAN_CHAT_STATUS_OK ||
        !writeString(&writer, kCallFieldOperation, true, operation)) {
        return false;
    }
    if (callId != 0 &&
        lan_chat_tlv_write_u64(&writer, kCallFieldCallId, 1, callId) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (peerId != 0 &&
        lan_chat_tlv_write_u64(&writer, kCallFieldPeerId, 0, peerId) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    if (!mediaMode.isEmpty() && !writeString(&writer, kCallFieldMediaMode, false, mediaMode)) {
        return false;
    }
    if (!sdp.isEmpty() && !writeString(&writer, kCallFieldSdp, false, sdp)) {
        return false;
    }
    if (!iceCandidate.isEmpty() && !writeString(&writer, kCallFieldIceCandidate, false, iceCandidate)) {
        return false;
    }
    if (!iceMid.isEmpty() && !writeString(&writer, kCallFieldIceMid, false, iceMid)) {
        return false;
    }
    if (hasIceMlineIndex &&
        lan_chat_tlv_write_u32(&writer, kCallFieldIceMlineIndex, false, iceMlineIndex) != LAN_CHAT_STATUS_OK) {
        return false;
    }
    return queuePacket(
        LAN_CHAT_GROUP_CALL,
        LAN_CHAT_MSG_REQ,
        LAN_CHAT_FLAG_REQUEST,
        seq,
        peerId,
        body.data(),
        lan_chat_tlv_writer_size(&writer),
        operation);
}

bool GuiClient::queuePacket(
    quint16 group,
    quint16 type,
    quint16 flags,
    quint32 seq,
    quint64 receiverId,
    const std::uint8_t *body,
    size_t bodyLen,
    const QString &label)
{
    if (!connected_) {
        emit statusMessage(QStringLiteral("%1 failed: not connected").arg(label));
        return false;
    }
    lan_chat_header_t header{};
    lan_chat_header_init(&header, group, type, flags, static_cast<std::uint32_t>(bodyLen));
    header.seq = seq;
    header.sender_id = userId_;
    header.receiver_id = receiverId;
    header.session_id = sessionId_;
    const lan_chat_status_t status = lan_chat_tcp_connection_queue_packet(&connection_, &header, body, bodyLen);
    if (status != LAN_CHAT_STATUS_OK) {
        emit statusMessage(QStringLiteral("%1 queue failed: %2").arg(label, statusName(status)));
        return false;
    }
    return flushConnection();
}

bool GuiClient::flushConnection()
{
    const lan_chat_status_t status = lan_chat_tcp_connection_flush(&connection_);
    if (status == LAN_CHAT_STATUS_OK || status == LAN_CHAT_STATUS_NEED_MORE_DATA) {
        return true;
    }
    failAndClose(QStringLiteral("send failed: %1").arg(statusName(status)));
    return false;
}

void GuiClient::processPacket(const std::vector<std::uint8_t> &packet)
{
    lan_chat_header_t header{};
    const std::uint8_t *body = nullptr;
    size_t bodyLen = 0;
    const lan_chat_status_t status = lan_chat_packet_unpack(&header, packet.data(), packet.size(), &body, &bodyLen);
    if (status != LAN_CHAT_STATUS_OK) {
        emit statusMessage(QStringLiteral("bad packet: %1").arg(statusName(status)));
        return;
    }

    if (header.message_group == LAN_CHAT_GROUP_AUTH && header.message_type == LAN_CHAT_MSG_RSP) {
        processAuthResponse(header, body, bodyLen);
    } else if (header.message_group == LAN_CHAT_GROUP_USER && header.message_type == LAN_CHAT_MSG_RSP) {
        processUserResponse(header, body, bodyLen);
    } else if (header.message_group == LAN_CHAT_GROUP_CALL && header.message_type == LAN_CHAT_MSG_RSP) {
        processCallResponse(header, body, bodyLen);
    } else if (header.message_group == LAN_CHAT_GROUP_CALL && header.message_type == LAN_CHAT_MSG_NOTIFY) {
        processCallNotify(header, body, bodyLen);
    }
}

void GuiClient::processAuthResponse(const lan_chat_header_t &header, const std::uint8_t *body, size_t bodyLen)
{
    PendingRequest request = pending_.take(header.seq);
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    bool ok = true;
    bool hasStatus = false;
    lan_chat_status_t responseStatus = LAN_CHAT_STATUS_INTERNAL_ERROR;
    quint64 responseUserId = 0;
    quint64 responseSessionId = 0;
    QString message;

    if (request.group != QStringLiteral("auth") ||
        lan_chat_tlv_reader_init(&reader, body, bodyLen) != LAN_CHAT_STATUS_OK) {
        emit authenticationFailed(QStringLiteral("invalid auth response"));
        return;
    }

    lan_chat_status_t status = LAN_CHAT_STATUS_NEED_MORE_DATA;
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case kResponseFieldStatus:
            responseStatus = static_cast<lan_chat_status_t>(tlvU16(tlv, &ok));
            hasStatus = true;
            break;
        case kResponseFieldUserId:
            responseUserId = tlvU64(tlv, &ok);
            break;
        case kResponseFieldSessionId:
            responseSessionId = tlvU64(tlv, &ok);
            break;
        case kResponseFieldMessage:
            message = tlvString(tlv);
            break;
        default:
            break;
        }
    }

    if (status != LAN_CHAT_STATUS_NEED_MORE_DATA || !ok || !hasStatus) {
        emit authenticationFailed(QStringLiteral("invalid auth response body"));
        return;
    }
    if (responseStatus != LAN_CHAT_STATUS_OK) {
        resetAuth();
        emit authenticationFailed(formatStatusMessage(responseStatus, message));
        return;
    }

    authenticated_ = true;
    userId_ = responseUserId;
    sessionId_ = responseSessionId;
    username_ = pendingUsername_;
    emit authenticated(userId_, sessionId_, username_);
    emit statusMessage(QStringLiteral("%1 ok: user_id=%2").arg(request.operation).arg(userId_));
    requestOnlineUsers();
}

void GuiClient::processUserResponse(const lan_chat_header_t &header, const std::uint8_t *body, size_t bodyLen)
{
    Q_UNUSED(header);
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    bool ok = true;
    bool hasStatus = false;
    lan_chat_status_t responseStatus = LAN_CHAT_STATUS_INTERNAL_ERROR;
    quint64 userCount = 0;
    QVector<OnlineUser> users;
    QString message;

    if (lan_chat_tlv_reader_init(&reader, body, bodyLen) != LAN_CHAT_STATUS_OK) {
        emit statusMessage(QStringLiteral("invalid online-list response"));
        return;
    }
    lan_chat_status_t status = LAN_CHAT_STATUS_NEED_MORE_DATA;
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case kResponseFieldStatus:
            responseStatus = static_cast<lan_chat_status_t>(tlvU16(tlv, &ok));
            hasStatus = true;
            break;
        case kUserFieldCount:
            userCount = tlvU64(tlv, &ok);
            break;
        case kUserFieldUsersBlob:
            ok = ok && parseUserBlob(tlv.value, tlv.length, &users);
            break;
        case kResponseFieldMessage:
            message = tlvString(tlv);
            break;
        default:
            break;
        }
    }
    if (status != LAN_CHAT_STATUS_NEED_MORE_DATA || !ok || !hasStatus) {
        emit statusMessage(QStringLiteral("invalid online-list response body"));
        return;
    }
    if (responseStatus != LAN_CHAT_STATUS_OK) {
        emit statusMessage(formatStatusMessage(responseStatus, message));
        return;
    }
    if (userCount != static_cast<quint64>(users.size())) {
        emit statusMessage(QStringLiteral("online-list count mismatch"));
    }
    emit onlineUsersUpdated(users);
}

void GuiClient::processCallResponse(const lan_chat_header_t &header, const std::uint8_t *body, size_t bodyLen)
{
    PendingRequest request = pending_.take(header.seq);
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    bool ok = true;
    bool hasStatus = false;
    lan_chat_status_t responseStatus = LAN_CHAT_STATUS_INTERNAL_ERROR;
    quint64 callId = request.callId;
    QString message;

    if (lan_chat_tlv_reader_init(&reader, body, bodyLen) != LAN_CHAT_STATUS_OK) {
        emit callOperationFailed(request.operation, callId, QStringLiteral("invalid call response"));
        return;
    }
    lan_chat_status_t status = LAN_CHAT_STATUS_NEED_MORE_DATA;
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case kResponseFieldStatus:
            responseStatus = static_cast<lan_chat_status_t>(tlvU16(tlv, &ok));
            hasStatus = true;
            break;
        case kCallFieldCallId:
            callId = tlvU64(tlv, &ok);
            break;
        case kResponseFieldMessage:
            message = tlvString(tlv);
            break;
        default:
            break;
        }
    }
    if (status != LAN_CHAT_STATUS_NEED_MORE_DATA || !ok || !hasStatus) {
        emit callOperationFailed(request.operation, callId, QStringLiteral("invalid call response body"));
        return;
    }
    if (responseStatus != LAN_CHAT_STATUS_OK) {
        emit callOperationFailed(request.operation, callId, formatStatusMessage(responseStatus, message));
        return;
    }
    if (request.operation == QStringLiteral("invite")) {
        emit callInviteSent(callId, request.peerId);
    }
    emit callOperationSucceeded(request.operation, callId);
}

void GuiClient::processCallNotify(const lan_chat_header_t &header, const std::uint8_t *body, size_t bodyLen)
{
    Q_UNUSED(header);
    lan_chat_tlv_reader_t reader{};
    lan_chat_tlv_t tlv{};
    bool ok = true;
    CallSignal signal;

    if (lan_chat_tlv_reader_init(&reader, body, bodyLen) != LAN_CHAT_STATUS_OK) {
        emit statusMessage(QStringLiteral("invalid call notify"));
        return;
    }
    lan_chat_status_t status = LAN_CHAT_STATUS_NEED_MORE_DATA;
    while ((status = lan_chat_tlv_next(&reader, &tlv)) == LAN_CHAT_STATUS_OK) {
        switch (tlv.field_id) {
        case kCallFieldOperation:
            signal.operation = tlvString(tlv);
            break;
        case kCallFieldCallId:
            signal.callId = tlvU64(tlv, &ok);
            break;
        case kCallFieldPeerId:
            signal.peerId = tlvU64(tlv, &ok);
            break;
        case kCallFieldMediaMode:
            signal.mediaMode = tlvString(tlv);
            break;
        case kCallFieldSdp:
            signal.sdp = tlvString(tlv);
            break;
        case kCallFieldIceCandidate:
            signal.iceCandidate = tlvString(tlv);
            break;
        case kCallFieldIceMid:
            signal.iceMid = tlvString(tlv);
            break;
        case kCallFieldIceMlineIndex:
            signal.iceMlineIndex = tlvU32(tlv, &ok);
            signal.hasIceMlineIndex = true;
            break;
        default:
            break;
        }
    }
    if (status != LAN_CHAT_STATUS_NEED_MORE_DATA || !ok || signal.operation.isEmpty()) {
        emit statusMessage(QStringLiteral("invalid call notify body"));
        return;
    }
    if (signal.operation == QStringLiteral("invite")) {
        emit incomingCall(signal);
    }
    emit callSignalReceived(signal);
}

void GuiClient::failAndClose(const QString &reason)
{
    const bool wasConnected = connected_;
    ioTimer_.stop();
    if (connection_.is_open) {
        lan_chat_tcp_connection_close(&connection_);
    }
    if (transportInitialized_) {
        lan_chat_transport_shutdown(&transport_);
        transportInitialized_ = false;
    }
    pending_.clear();
    resetAuth();
    connected_ = false;
    if (wasConnected) {
        emit connectionStateChanged(false);
    }
    emit disconnected(reason);
}

void GuiClient::resetAuth()
{
    authenticated_ = false;
    userId_ = 0;
    sessionId_ = 0;
}

quint32 GuiClient::nextSeq()
{
    return nextSeq_++;
}
