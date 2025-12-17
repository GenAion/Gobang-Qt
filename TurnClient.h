#ifndef TURNCLIENT_H
#define TURNCLIENT_H

#include <QObject>
#include <QUdpSocket>
#include <QHostAddress>
#include <QByteArray>
#include <QTimer>

class TurnClient : public QObject
{
    Q_OBJECT
public:
    struct RelayAddr {
        QHostAddress ip;
        quint16 port = 0;
        bool valid() const { return !ip.isNull() && port != 0; }
        QString toString() const { return ip.toString() + ":" + QString::number(port); }
    };

    explicit TurnClient(QObject *parent = nullptr);

    void start(const QHostAddress &turnServerIp, quint16 turnPort,
               const QString &username, const QString &password, const QString &realm);

    bool isReady() const { return ready_; }
    RelayAddr relayAddr() const { return relayAddr_; }

    void ensurePermission(const QHostAddress &peerRelayIp);
    bool isPermissionReady() const { return permissionDone_; }

    void sendToPeerViaTurn(const QHostAddress &peerRelayIp, quint16 peerRelayPort, const QByteArray &data);

signals:
    void logMessage(const QString &msg);
    void ready(const QHostAddress &relayIp, quint16 relayPort);
    void error(const QString &err);
    void peerDataReceived(const QHostAddress &peer, quint16 peerPort, const QByteArray &data);
    void permissionReady(const QHostAddress &peerRelayIp);

private slots:
    void onUdpReadyRead();
    void onRefreshTimer();

private:
    QByteArray buildAllocateRequest(bool withAuth);
    QByteArray buildCreatePermissionRequest(const QHostAddress &peerIp, bool withAuth);
    QByteArray buildRefreshRequest(bool withAuth);

    void sendTurnMessage(const QByteArray &msg);
    void handleTurnResponse(const QByteArray &d);

    static quint32 readU32(const QByteArray &d, int off);
    static quint16 readU16(const QByteArray &d, int off);

    QByteArray makeStunHeader(quint16 msgType, const QByteArray &txid12);
    static QByteArray makeAttr(quint16 type, const QByteArray &value);

    static QByteArray encodeXorPeerAddress(const QHostAddress &ip, quint16 port, const QByteArray &txid12);
    static bool decodeXorAddressAttr(const QByteArray &value, const QByteArray &txid12,
                                     QHostAddress &outIp, quint16 &outPort);

    static bool isStunLike(const QByteArray &d);
    static QByteArray randomTxid12();

private:
    QUdpSocket *udp_ = nullptr;

    QHostAddress turnIp_;
    quint16 turnPort_ = 3478;

    QString username_;
    QString password_;
    QString realm_;

    QByteArray nonce_;
    QByteArray txid_;
    bool haveNonce_ = false;
    bool ready_ = false;

    RelayAddr relayAddr_;
    QTimer refreshTimer_;

    bool permissionDone_ = false;
    QHostAddress permissionPeerIp_;
};

#endif
