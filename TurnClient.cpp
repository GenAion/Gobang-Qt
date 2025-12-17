#include "TurnClient.h"
#include <QRandomGenerator>
#include <QCryptographicHash>

static const quint32 MAGIC_COOKIE = 0x2112A442;

// message types
static const quint16 ALLOCATE_REQUEST     = 0x0003;
static const quint16 ALLOCATE_SUCCESS     = 0x0103;
static const quint16 ALLOCATE_ERROR       = 0x0113;

static const quint16 REFRESH_REQUEST      = 0x0004;
static const quint16 REFRESH_SUCCESS      = 0x0104;
static const quint16 REFRESH_ERROR        = 0x0114;

static const quint16 CREATEPERM_REQUEST   = 0x0008;
static const quint16 CREATEPERM_SUCCESS   = 0x0108;
static const quint16 CREATEPERM_ERROR     = 0x0118;

static const quint16 SEND_INDICATION      = 0x0016;
static const quint16 DATA_INDICATION      = 0x0017;

// attrs
static const quint16 ATTR_USERNAME        = 0x0006;
static const quint16 ATTR_REALM           = 0x0014;
static const quint16 ATTR_NONCE           = 0x0015;
static const quint16 ATTR_MSG_INTEG       = 0x0008;

static const quint16 ATTR_REQ_TRANSPORT   = 0x0019;
static const quint16 ATTR_XOR_RELAYED     = 0x0016;
static const quint16 ATTR_ERROR_CODE      = 0x0009;
static const quint16 ATTR_LIFETIME        = 0x000D;

static const quint16 ATTR_XOR_PEER_ADDR   = 0x0012;
static const quint16 ATTR_DATA            = 0x0013;

// ---- HMAC-SHA1 ----
static QByteArray sha1(const QByteArray &in) {
    return QCryptographicHash::hash(in, QCryptographicHash::Sha1);
}
static QByteArray hmacSha1(const QByteArray &key, const QByteArray &msg)
{
    QByteArray k = key;
    const int block = 64;
    if (k.size() > block) k = sha1(k);
    if (k.size() < block) k.append(QByteArray(block - k.size(), char(0)));

    QByteArray o_key_pad(block, char(0x5c));
    QByteArray i_key_pad(block, char(0x36));
    for (int i = 0; i < block; ++i) {
        o_key_pad[i] = char(quint8(o_key_pad[i]) ^ quint8(k[i]));
        i_key_pad[i] = char(quint8(i_key_pad[i]) ^ quint8(k[i]));
    }
    QByteArray inner = sha1(i_key_pad + msg);
    return sha1(o_key_pad + inner);
}

// ---- MESSAGE-INTEGRITY (Long-term) ----
// 规则：length 包含 MI；HMAC 输入排除 MI 整个属性（24字节）
static void addMessageIntegrity(QByteArray &msg,
                                const QString &username,
                                const QString &realm,
                                const QString &password)
{
    // append MI attr (24 bytes)
    QByteArray mi(24, 0);
    mi[0] = 0x00; mi[1] = 0x08;
    mi[2] = 0x00; mi[3] = 0x14;
    msg += mi;

    // update length include MI
    quint16 len = quint16(msg.size() - 20);
    msg[2] = char((len >> 8) & 0xFF);
    msg[3] = char(len & 0xFF);

    // key = MD5(username:realm:password)
    QByteArray key = QCryptographicHash::hash((username + ":" + realm + ":" + password).toUtf8(),
                                              QCryptographicHash::Md5);

    // HMAC input excludes whole MI attr
    QByteArray hmacInput = msg.left(msg.size() - 24);
    QByteArray h = hmacSha1(key, hmacInput);

    // fill MI value
    int valuePos = msg.size() - 20;
    for (int i = 0; i < 20; ++i) msg[valuePos + i] = h[i];
}

TurnClient::TurnClient(QObject *parent) : QObject(parent)
{
    udp_ = new QUdpSocket(this);
    udp_->bind(QHostAddress(QHostAddress::AnyIPv4), 0,
               QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    connect(udp_, &QUdpSocket::readyRead, this, &TurnClient::onUdpReadyRead);

    refreshTimer_.setInterval(60000);
    connect(&refreshTimer_, &QTimer::timeout, this, &TurnClient::onRefreshTimer);
}

void TurnClient::start(const QHostAddress &turnServerIp, quint16 turnPort,
                       const QString &username, const QString &password, const QString &realm)
{
    turnIp_ = turnServerIp;
    turnPort_ = turnPort;
    username_ = username;
    password_ = password;
    realm_ = realm;

    ready_ = false;
    haveNonce_ = false;
    nonce_.clear();

    permissionDone_ = false;
    permissionPeerIp_.clear();

    relayAddr_ = {};

    emit logMessage(QString("TURN start: %1:%2").arg(turnIp_.toString()).arg(turnPort_));
    txid_ = randomTxid12();
    sendTurnMessage(buildAllocateRequest(false));
}

void TurnClient::ensurePermission(const QHostAddress &peerRelayIp)
{
    if (!ready_ || !haveNonce_) return;
    if (peerRelayIp.isNull()) return;

    if (permissionDone_ && permissionPeerIp_ == peerRelayIp) return;

    permissionDone_ = false;
    permissionPeerIp_ = peerRelayIp;

    txid_ = randomTxid12();
    sendTurnMessage(buildCreatePermissionRequest(peerRelayIp, true));
}

void TurnClient::sendToPeerViaTurn(const QHostAddress &peerRelayIp, quint16 peerRelayPort, const QByteArray &data)
{
    if (!ready_) return;

    ensurePermission(peerRelayIp);

    QByteArray txid = randomTxid12();
    QByteArray msg = makeStunHeader(SEND_INDICATION, txid);

    msg += makeAttr(ATTR_XOR_PEER_ADDR, encodeXorPeerAddress(peerRelayIp, peerRelayPort, txid));
    msg += makeAttr(ATTR_DATA, data);

    quint16 len = quint16(msg.size() - 20);
    msg[2] = char((len >> 8) & 0xFF);
    msg[3] = char(len & 0xFF);

    udp_->writeDatagram(msg, turnIp_, turnPort_);
}

void TurnClient::onRefreshTimer()
{
    if (!ready_ || !haveNonce_) return;
    txid_ = randomTxid12();
    sendTurnMessage(buildRefreshRequest(true));
}

void TurnClient::onUdpReadyRead()
{
    while (udp_->hasPendingDatagrams()) {
        QHostAddress sender; quint16 sport = 0;
        QByteArray d; d.resize(int(udp_->pendingDatagramSize()));
        udp_->readDatagram(d.data(), d.size(), &sender, &sport);
        if (!isStunLike(d)) continue;
        handleTurnResponse(d);
    }
}

void TurnClient::handleTurnResponse(const QByteArray &d)
{
    if (d.size() < 20) return;

    quint16 msgType = readU16(d, 0);
    quint16 msgLen  = readU16(d, 2);
    QByteArray rxTxid = d.mid(8, 12);

    int end = 20 + msgLen;
    if (end > d.size()) end = d.size();

    QByteArray nonceRx;
    QByteArray realmRx;
    int errCode = 0;

    QHostAddress relayedIp; quint16 relayedPort = 0;
    bool gotRelayed = false;

    for (int pos = 20; pos + 4 <= end; ) {
        quint16 at = readU16(d, pos);
        quint16 al = readU16(d, pos + 2);
        pos += 4;
        if (pos + al > end) break;

        QByteArray val = d.mid(pos, al);

        if (at == ATTR_NONCE) nonceRx = val;
        else if (at == ATTR_REALM) realmRx = val;
        else if (at == ATTR_ERROR_CODE && al >= 4) {
            int cls = int(quint8(val[2]));
            int num = int(quint8(val[3]));
            errCode = cls * 100 + num;
        } else if (at == ATTR_XOR_RELAYED && al >= 8) {
            QHostAddress ip; quint16 port;
            if (decodeXorAddressAttr(val, rxTxid, ip, port)) {
                relayedIp = ip;
                relayedPort = port;
                gotRelayed = true;
            }
        }

        int pad = (4 - (al % 4)) % 4;
        pos += al + pad;
    }

    auto updateNonceRealmFrom401 = [&]() {
        if (!nonceRx.isEmpty()) { haveNonce_ = true; nonce_ = nonceRx; }
        if (!realmRx.isEmpty()) { realm_ = QString::fromUtf8(realmRx); }
    };

    if (msgType == ALLOCATE_ERROR) {
        if (errCode == 401 && !nonceRx.isEmpty()) {
            updateNonceRealmFrom401();
            emit logMessage("TURN Allocate got 401, retry with auth.");
            txid_ = randomTxid12();
            sendTurnMessage(buildAllocateRequest(true));
            return;
        }
        emit error(QString("TURN Allocate error: %1").arg(errCode));
        return;
    }

    if (msgType == CREATEPERM_ERROR) {
        if (errCode == 401 && !nonceRx.isEmpty()) {
            updateNonceRealmFrom401();
            emit logMessage("TURN CreatePermission got 401, retry with auth.");
            txid_ = randomTxid12();
            sendTurnMessage(buildCreatePermissionRequest(permissionPeerIp_, true));
            return;
        }
        emit error(QString("TURN CreatePermission error: %1").arg(errCode));
        return;
    }

    if (msgType == REFRESH_ERROR) {
        if (errCode == 401 && !nonceRx.isEmpty()) {
            updateNonceRealmFrom401();
            emit logMessage("TURN Refresh got 401, retry with auth.");
            txid_ = randomTxid12();
            sendTurnMessage(buildRefreshRequest(true));
            return;
        }
        emit error(QString("TURN Refresh error: %1").arg(errCode));
        return;
    }

    if (msgType == ALLOCATE_SUCCESS) {
        if (gotRelayed) {
            relayAddr_.ip = relayedIp;
            relayAddr_.port = relayedPort;
            ready_ = true;
            emit logMessage("TURN Allocated relay: " + relayAddr_.toString());
            emit ready(relayAddr_.ip, relayAddr_.port);
            refreshTimer_.start();
        }
        return;
    }

    if (msgType == CREATEPERM_SUCCESS) {
        permissionDone_ = true;
        emit logMessage("TURN CreatePermission success.");
        emit permissionReady(permissionPeerIp_);
        return;
    }

    if (msgType == REFRESH_SUCCESS) {
        emit logMessage("TURN Refresh success.");
        return;
    }

    if (msgType == DATA_INDICATION) {
        QHostAddress peer; quint16 pport = 0;
        QByteArray payload;

        for (int pos = 20; pos + 4 <= end; ) {
            quint16 at = readU16(d, pos);
            quint16 al = readU16(d, pos + 2);
            pos += 4;
            if (pos + al > end) break;

            QByteArray val = d.mid(pos, al);
            if (at == ATTR_XOR_PEER_ADDR) {
                decodeXorAddressAttr(val, rxTxid, peer, pport);
            } else if (at == ATTR_DATA) {
                payload = val;
            }

            int pad = (4 - (al % 4)) % 4;
            pos += al + pad;
        }

        if (!payload.isEmpty()) {
            emit peerDataReceived(peer, pport, payload);
        }
        return;
    }
}

QByteArray TurnClient::buildAllocateRequest(bool withAuth)
{
    QByteArray msg = makeStunHeader(ALLOCATE_REQUEST, txid_);

    QByteArray rt(4, 0);
    rt[0] = char(quint8(17)); // UDP
    msg += makeAttr(ATTR_REQ_TRANSPORT, rt);

    if (withAuth && haveNonce_) {
        msg += makeAttr(ATTR_USERNAME, username_.toUtf8());
        msg += makeAttr(ATTR_REALM, realm_.toUtf8());
        msg += makeAttr(ATTR_NONCE, nonce_);
        addMessageIntegrity(msg, username_, realm_, password_);
    } else {
        quint16 len = quint16(msg.size() - 20);
        msg[2] = char((len >> 8) & 0xFF);
        msg[3] = char(len & 0xFF);
    }
    return msg;
}

QByteArray TurnClient::buildCreatePermissionRequest(const QHostAddress &peerIp, bool withAuth)
{
    QByteArray msg = makeStunHeader(CREATEPERM_REQUEST, txid_);
    msg += makeAttr(ATTR_XOR_PEER_ADDR, encodeXorPeerAddress(peerIp, 0, txid_));

    if (withAuth && haveNonce_) {
        msg += makeAttr(ATTR_USERNAME, username_.toUtf8());
        msg += makeAttr(ATTR_REALM, realm_.toUtf8());
        msg += makeAttr(ATTR_NONCE, nonce_);
        addMessageIntegrity(msg, username_, realm_, password_);
    } else {
        quint16 len = quint16(msg.size() - 20);
        msg[2] = char((len >> 8) & 0xFF);
        msg[3] = char(len & 0xFF);
    }
    return msg;
}

QByteArray TurnClient::buildRefreshRequest(bool withAuth)
{
    QByteArray msg = makeStunHeader(REFRESH_REQUEST, txid_);

    QByteArray lt(4, 0);
    lt[3] = char(quint8(120));
    msg += makeAttr(ATTR_LIFETIME, lt);

    if (withAuth && haveNonce_) {
        msg += makeAttr(ATTR_USERNAME, username_.toUtf8());
        msg += makeAttr(ATTR_REALM, realm_.toUtf8());
        msg += makeAttr(ATTR_NONCE, nonce_);
        addMessageIntegrity(msg, username_, realm_, password_);
    } else {
        quint16 len = quint16(msg.size() - 20);
        msg[2] = char((len >> 8) & 0xFF);
        msg[3] = char(len & 0xFF);
    }
    return msg;
}

void TurnClient::sendTurnMessage(const QByteArray &msg)
{
    udp_->writeDatagram(msg, turnIp_, turnPort_);
}

quint32 TurnClient::readU32(const QByteArray &d, int off)
{
    return (quint32(quint8(d[off])) << 24) |
           (quint32(quint8(d[off+1])) << 16) |
           (quint32(quint8(d[off+2])) << 8) |
           quint32(quint8(d[off+3]));
}

quint16 TurnClient::readU16(const QByteArray &d, int off)
{
    return (quint16(quint8(d[off])) << 8) | quint16(quint8(d[off+1]));
}

QByteArray TurnClient::randomTxid12()
{
    QByteArray t(12, 0);
    for (int i = 0; i < 12; ++i)
        t[i] = char(quint8(QRandomGenerator::global()->generate() & 0xFF));
    return t;
}

QByteArray TurnClient::makeStunHeader(quint16 msgType, const QByteArray &txid12)
{
    QByteArray h(20, 0);
    h[0] = char((msgType >> 8) & 0xFF);
    h[1] = char(msgType & 0xFF);
    h[4] = 0x21; h[5] = 0x12; h[6] = char(0xA4); h[7] = 0x42;
    for (int i = 0; i < 12; ++i) h[8+i] = txid12[i];
    return h;
}

QByteArray TurnClient::makeAttr(quint16 type, const QByteArray &value)
{
    QByteArray a(4, 0);
    a[0] = char((type >> 8) & 0xFF);
    a[1] = char(type & 0xFF);

    quint16 len = quint16(value.size());
    a[2] = char((len >> 8) & 0xFF);
    a[3] = char(len & 0xFF);

    int pad = (4 - (value.size() % 4)) % 4;
    return a + value + QByteArray(pad, char(0));
}

bool TurnClient::isStunLike(const QByteArray &d)
{
    if (d.size() < 20) return false;
    quint8 b0 = quint8(d[0]);
    if ((b0 & 0xC0) != 0x00) return false;
    return readU32(d, 4) == MAGIC_COOKIE;
}

QByteArray TurnClient::encodeXorPeerAddress(const QHostAddress &ip, quint16 port, const QByteArray &txid12)
{
    Q_UNUSED(txid12);
    QByteArray v(8, 0);
    v[0] = 0x00;
    v[1] = 0x01; // IPv4
    quint16 xport = port ^ 0x2112;
    v[2] = char((xport >> 8) & 0xFF);
    v[3] = char(xport & 0xFF);

    quint32 addr = ip.toIPv4Address();
    quint32 xaddr = addr ^ MAGIC_COOKIE;
    v[4] = char((xaddr >> 24) & 0xFF);
    v[5] = char((xaddr >> 16) & 0xFF);
    v[6] = char((xaddr >> 8) & 0xFF);
    v[7] = char(xaddr & 0xFF);
    return v;
}

bool TurnClient::decodeXorAddressAttr(const QByteArray &value, const QByteArray &txid12,
                                      QHostAddress &outIp, quint16 &outPort)
{
    Q_UNUSED(txid12);
    if (value.size() < 8) return false;

    const quint8 reserved = quint8(value[0]);
    const quint8 family   = quint8(value[1]);
    if (reserved != 0x00) return false;
    if (family != 0x01) return false;

    const quint16 xport = (quint16(quint8(value[2])) << 8) | quint16(quint8(value[3]));
    outPort = xport ^ 0x2112;

    const quint32 xaddr =
        (quint32(quint8(value[4])) << 24) |
        (quint32(quint8(value[5])) << 16) |
        (quint32(quint8(value[6])) << 8)  |
        quint32(quint8(value[7]));

    const quint32 addr = xaddr ^ MAGIC_COOKIE;
    outIp = QHostAddress(addr);
    return true;
}
