#ifndef GAMEWINDOW_H
#define GAMEWINDOW_H

#include <QWidget>
#include <QVector>
#include <QPoint>
#include <QPixmap>
#include <QTimer>
#include <QElapsedTimer>
#include <QRect>
#include <QSoundEffect>
#include <QUrl>

class QLabel;
class QPushButton;
class QComboBox;
class QLineEdit;

class NetworkManager;

class GameWindow : public QWidget
{
    Q_OBJECT
public:
    explicit GameWindow(QWidget *parent = nullptr);

private slots:
    void onConnectClicked();

    void onMoveReceived(int x, int y, int color, int seq, int nextColor,
                        bool gameOver, int winner, const QVector<QPoint> &winFive);

    void onStateReceived(const QVector<int> &flatBoard, int nextColor, int lastX, int lastY, int seq,
                         int onlineCount, bool gameOver, int winner, const QVector<QPoint> &winFive);

    void onPresenceChanged(int onlineCount);
    void onMoveRejected(const QString &reason);

    void onP2PConnected();
    void onLogMessage(const QString &msg);

    void onTurnTimerTick();
    void onAnimTick();

private:
    class BoardWidget : public QWidget
    {
    public:
        explicit BoardWidget(GameWindow *game);

    protected:
        void paintEvent(QPaintEvent *event) override;
        void mouseReleaseEvent(QMouseEvent *event) override;
        void mouseMoveEvent(QMouseEvent *event) override;
        void leaveEvent(QEvent *event) override;
        void resizeEvent(QResizeEvent *event) override;

    private:
        GameWindow *game_ = nullptr;
    };

    friend class BoardWidget;

    // UI
    void setupUi();
    void setUiEnabled(bool enabled);
    void updateStatusText(const QString &text);

    // Network start
    void startNetwork();

    // Board repaint helpers
    void updateBoardAll();
    void updateBoardRect(const QRect &rc);

    // Game state
    enum class GameState {
        WaitingConnection,
        WaitingOpponent,
        MyTurn,
        OpponentTurn,
        GameOver
    };
    GameState state_ = GameState::WaitingConnection;

    bool isConnected() const { return state_ != GameState::WaitingConnection; }
    bool isMyTurn()   const { return state_ == GameState::MyTurn; }
    bool isGameOver() const { return state_ == GameState::GameOver; }

    // Board layout/cache
    int  boardW() const;
    int  boardH() const;
    QRect boardRect() const;

    void updateLayoutMetrics();
    void rebuildBoardCache();

    int boardOriginX() const { return margin_; }
    int boardOriginY() const { return margin_ + topExtra_; }

    QPoint coordFromMouse(const QPoint &pos) const;

    QRect cellDirtyRect(int x, int y, int extraPx = 18) const;
    QRect hoverDirtyRect(const QPoint &cell) const;
    QRect countdownDirtyRect() const;
    QRect winFiveDirtyRect() const;

    // Painting (called by BoardWidget)
    void paintBoard(QPainter &p);

    // Board input (called by BoardWidget)
    void handleBoardMouseRelease(QMouseEvent *event);
    void handleBoardMouseMove(QMouseEvent *event);
    void handleBoardLeave();
    void handleBoardResize();

    // Core game logic
    bool placeStone(int x, int y, int color);

    bool checkWinAt(int x, int y, int color) const;
    int  countInDirection(int x, int y, int dx, int dy, int color) const;
    bool isBoardFull() const;

    void startMyTurn();
    void endMyTurn();
    void autoMoveBest();

    QPoint findBestMove(int color) const;
    int evaluateCellScore(int x, int y, int color) const;
    void countLineInfo(int x, int y, int dx, int dy, int color,
                       int &countForward, int &countBackward, int &openEnds) const;

    void startWinFiveAnim(const QVector<QPoint> &five, const QString &title, const QString &text, int winnerColor);
    void finalizeGame(const QString &title, const QString &text);

    void endGameAndAskRestart(const QString &title, const QString &text);
    void resetBoardToNewGame();
    bool showResultDialog(bool iWin, const QString &title, const QString &text);
    void doServerRestart();

    enum HoverMode { HoverNone = 0, HoverEmpty, HoverBlocked };
    void clearHover();
    void setHoverCell(const QPoint &cell, HoverMode mode);

    // 将常见英文日志翻译为中文（未知内容原样返回，避免误译）
    QString translateLogToChinese(const QString &msg) const;

private:
    // UI Widgets
    QWidget     *leftPanel_   = nullptr;
    BoardWidget *boardView_   = nullptr;

    QComboBox   *roleCombo_   = nullptr;
    QLineEdit   *roomEdit_    = nullptr;
    QPushButton *connectBtn_  = nullptr;
    QLabel      *statusLabel_ = nullptr;

private:
    // Game data
    static constexpr int BOARD_SIZE = 15;

    int cellSize_ = 40;
    int margin_   = 30;
    int topExtra_ = 55;

    static constexpr int MIN_CELL_SIZE = 24;
    static constexpr int MAX_CELL_SIZE = 72;

    static constexpr int TURN_TIME_SECONDS = 30;
    static constexpr int STONE_ANIM_MS = 300;
    static constexpr int ANIM_TICK_MS  = 16;

    struct CellAnim {
        bool active = false;
        qint64 startMs = 0;
        int durationMs = STONE_ANIM_MS;
    };

    QVector<QVector<int>> board_;
    QVector<QVector<CellAnim>> anims_;

    int  myColor_ = 1;
    bool amHost_  = false;

    NetworkManager *net_ = nullptr;

    QPixmap winImg_;
    QPixmap loseImg_;

    QPoint lastMove_{-1, -1};
    bool   lastMarkBlinking_ = false;
    qint64 lastMarkStartMs_  = 0;

    QTimer turnTimer_;
    int    timeLeft_ = TURN_TIME_SECONDS;

    QTimer animTimer_;
    QElapsedTimer clock_;

    QVector<QPoint> winFive_;
    bool   winAnimActive_ = false;
    qint64 winAnimStartMs_ = 0;
    int    winAnimWinnerColor_ = 0;

    QTimer winAnimDelayTimer_;
    QString pendingTitle_;
    QString pendingText_;

    static constexpr int WIN_STEP_MS = 220;
    static constexpr int WIN_TAIL_MS = 650;

    QPoint hoverCell_{-1, -1};
    HoverMode hoverMode_ = HoverNone;

    QPixmap boardCache_;
    qreal   boardCacheDpr_ = 0.0;

    QSoundEffect placeSound_;

    int lastSeqApplied_ = 0;

    bool opponentPresent_ = false;
    int  serverNextColor_ = 1;

    bool serverGameOver_ = false;
    int  serverWinner_ = 0;
};

#endif // GAMEWINDOW_H
