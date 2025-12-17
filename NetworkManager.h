#ifndef NETWORKMANAGER_H
#define NETWORKMANAGER_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>
#include <QPoint>
#include <QJsonObject>

class NetworkManager : public QObject
{
    Q_OBJECT
public:
    enum Role { Host, Guest };
    explicit NetworkManager(QObject *parent = nullptr);

    void start(Role role, const QString &roomId);
    void sendMove(int x, int y, int color);
    void requestState();
    void sendReset();

signals:
    void logMessage(const QString &msg);
    void p2pConnected();

    void stateReceived(const QVector<int> &flatBoard,
                       int nextColor,
                       int lastX,
                       int lastY,
                       int seq,
                       int onlineCount,
                       bool gameOver,
                       int winner,
                       const QVector<QPoint> &winFive);

    void moveReceived(int x, int y, int color, int seq, int nextColor,
                      bool gameOver, int winner, const QVector<QPoint> &winFive);

    void moveRejected(const QString &reason);
    void presenceChanged(int onlineCount);

private slots:
    void onConnected();
    void onReadyRead();
    void onError(QAbstractSocket::SocketError);
    void onPingTimer();

private:
    void connectToServer();
    void sendJsonLine(const QJsonObject &obj);
    void handleJson(const QJsonObject &obj);
    QVector<QPoint> parseWinFive(const QJsonValue &v) const;

private:
    Role role_ = Host;
    QString roomId_;

    QTcpSocket *sock_ = nullptr;
    QByteArray  rxBuf_;
    bool connectedEmitted_ = false;

    QTimer pingTimer_;

    QString serverHost_ = "替换为服务器公网ip";
    quint16 serverPort_ = 5000;
};

#endif // NETWORKMANAGER_H
