#include "NetworkManager.h"

#include <QJsonDocument>
#include <QJsonArray>

NetworkManager::NetworkManager(QObject *parent)
    : QObject(parent),
    sock_(new QTcpSocket(this))
{
    connect(sock_, &QTcpSocket::connected, this, &NetworkManager::onConnected);
    connect(sock_, &QTcpSocket::readyRead, this, &NetworkManager::onReadyRead);
    connect(sock_,
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this,
            &NetworkManager::onError);

    pingTimer_.setInterval(10000);
    connect(&pingTimer_, &QTimer::timeout, this, &NetworkManager::onPingTimer);
}

void NetworkManager::start(Role role, const QString &roomId)
{
    role_ = role;
    roomId_ = roomId;

    connectedEmitted_ = false;
    rxBuf_.clear();

    emit logMessage(QString("Connecting to signaling server %1:%2 ...").arg(serverHost_).arg(serverPort_));
    connectToServer();
}

void NetworkManager::connectToServer()
{
    sock_->abort();
    sock_->connectToHost(serverHost_, serverPort_);
}

void NetworkManager::onConnected()
{
    emit logMessage("Signaling connected.");

    QJsonObject obj;
    obj["type"] = "join";
    obj["room"] = roomId_;
    sendJsonLine(obj);

    pingTimer_.start();
}

void NetworkManager::onError(QAbstractSocket::SocketError)
{
    emit logMessage("Signaling socket error: " + sock_->errorString());
}

void NetworkManager::onPingTimer()
{
    if (sock_->state() != QAbstractSocket::ConnectedState) return;
    QJsonObject obj;
    obj["type"] = "ping";
    sendJsonLine(obj);
}

void NetworkManager::sendJsonLine(const QJsonObject &obj)
{
    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    data.append('\n');
    sock_->write(data);
    sock_->flush();
}

void NetworkManager::requestState()
{
    if (sock_->state() != QAbstractSocket::ConnectedState) return;
    QJsonObject obj;
    obj["type"] = "state_get";
    sendJsonLine(obj);
}

void NetworkManager::sendMove(int x, int y, int color)
{
    if (sock_->state() != QAbstractSocket::ConnectedState) {
        emit logMessage("sendMove: socket not connected.");
        return;
    }
    QJsonObject obj;
    obj["type"] = "move";
    obj["x"] = x;
    obj["y"] = y;
    obj["color"] = color;
    sendJsonLine(obj);
}

void NetworkManager::sendReset()
{
    if (sock_->state() != QAbstractSocket::ConnectedState) {
        emit logMessage("sendReset: socket not connected.");
        return;
    }
    QJsonObject obj;
    obj["type"] = "reset";
    sendJsonLine(obj);
}

QVector<QPoint> NetworkManager::parseWinFive(const QJsonValue &v) const
{
    QVector<QPoint> out;
    QJsonArray arr = v.toArray();
    for (auto it : arr) {
        QJsonArray xy = it.toArray();
        if (xy.size() == 2) out.push_back(QPoint(xy[0].toInt(), xy[1].toInt()));
    }
    if (out.size() != 5) out.clear();
    return out;
}

void NetworkManager::onReadyRead()
{
    rxBuf_.append(sock_->readAll());

    while (true) {
        int idx = rxBuf_.indexOf('\n');
        if (idx < 0) break;

        QByteArray line = rxBuf_.left(idx);
        rxBuf_.remove(0, idx + 1);
        line = line.trimmed();
        if (line.isEmpty()) continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;

        handleJson(doc.object());
    }
}

void NetworkManager::handleJson(const QJsonObject &obj)
{
    const QString type = obj.value("type").toString();

    if (type == "join_ok") {
        emit logMessage("join_ok.");
        if (!connectedEmitted_) {
            connectedEmitted_ = true;
            emit p2pConnected();
        }
        requestState();
        return;
    }

    if (type == "presence") {
        const int count = obj.value("count").toInt(0);
        emit presenceChanged(count);
        return;
    }

    if (type == "state") {
        QJsonArray board = obj.value("board").toArray();
        if (board.size() != 15 * 15) return;

        QVector<int> flat;
        flat.reserve(board.size());
        for (auto v : board) flat.push_back(v.toInt());

        const int nextC = obj.value("next").toInt(1);
        const int lastX = obj.value("lastX").toInt(-1);
        const int lastY = obj.value("lastY").toInt(-1);
        const int seq   = obj.value("seq").toInt(0);
        const int count = obj.value("count").toInt(0);

        const bool gameOver = obj.value("gameOver").toBool(false);
        const int winner = obj.value("winner").toInt(0);
        const QVector<QPoint> winFive = parseWinFive(obj.value("winFive"));

        emit stateReceived(flat, nextC, lastX, lastY, seq, count, gameOver, winner, winFive);
        emit presenceChanged(count);
        return;
    }

    if (type == "move") {
        const int x = obj.value("x").toInt(-1);
        const int y = obj.value("y").toInt(-1);
        const int c = obj.value("color").toInt(0);
        const int seq = obj.value("seq").toInt(0);
        const int nextC = obj.value("next").toInt(1);

        const bool gameOver = obj.value("gameOver").toBool(false);
        const int winner = obj.value("winner").toInt(0);
        const QVector<QPoint> winFive = parseWinFive(obj.value("winFive"));

        emit moveReceived(x, y, c, seq, nextC, gameOver, winner, winFive);
        return;
    }

    if (type == "move_reject") {
        const QString reason = obj.value("reason").toString("reject");
        emit logMessage("move_reject: " + reason);
        emit moveRejected(reason);
        return;
    }

    if (type == "err") {
        emit logMessage("server err: " + obj.value("reason").toString());
        return;
    }

    if (type == "pong") return;
}
