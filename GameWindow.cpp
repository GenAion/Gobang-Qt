#include "GameWindow.h"
#include "NetworkManager.h"

#include <QPainter>
#include <QMouseEvent>
#include <QInputDialog>
#include <QMessageBox>
#include <QDebug>
#include <QPainterPath>
#include <QDialog>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QtMath>
#include <algorithm>

static constexpr int LAST_MARK_BLINK_MS = 1600;  // 新落子后闪烁时长
static constexpr int LAST_MARK_TOGGLE_MS = 240; // 闪烁频率

GameWindow::GameWindow(QWidget *parent)
    : QWidget(parent),
    board_(BOARD_SIZE, QVector<int>(BOARD_SIZE, 0)),
    anims_(BOARD_SIZE, QVector<CellAnim>(BOARD_SIZE))
{
    resize(900, 700);

    setWindowTitle("Gomoku");
    setMouseTracking(true);

    winImg_.load(":/res/WIN.jpg");
    loseImg_.load(":/res/LOSE.jpg");

    placeSound_.setSource(QUrl(QStringLiteral("qrc:/res/LuoZi.wav")));
    placeSound_.setVolume(0.60f);
    placeSound_.setLoopCount(1);

    turnTimer_.setInterval(1000);
    connect(&turnTimer_, &QTimer::timeout, this, &GameWindow::onTurnTimerTick);

    animTimer_.setInterval(ANIM_TICK_MS);
    connect(&animTimer_, &QTimer::timeout, this, &GameWindow::onAnimTick);

    winAnimDelayTimer_.setSingleShot(true);
    connect(&winAnimDelayTimer_, &QTimer::timeout, this, [this]() {
        finalizeGame(pendingTitle_, pendingText_);
    });

    clock_.start();

    updateLayoutMetrics();
    rebuildBoardCache();
    initUiAndNetwork();
}

void GameWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateLayoutMetrics();
    rebuildBoardCache();
    update();
}

void GameWindow::updateLayoutMetrics()
{
    const int w = width();
    const int h = height();

    topExtra_ = qMax(55, int(h * 0.08));
    const int baseMargin = qMax(18, int(qMin(w, h) * 0.03));

    int availW = w - 2 * baseMargin;
    int availH = h - topExtra_ - 2 * baseMargin;

    const int segments = BOARD_SIZE - 1;
    if (segments <= 0 || availW <= 0 || availH <= 0) return;

    int cs = qMin(availW / segments, availH / segments);
    cs = qBound(MIN_CELL_SIZE, cs, MAX_CELL_SIZE);
    cellSize_ = cs;

    const int boardPix = segments * cellSize_;
    int freeW = w - boardPix;
    int freeH = h - topExtra_ - boardPix;

    int mx = qMax(8, freeW / 2);
    int my = qMax(8, freeH / 2);

    margin_ = qMin(mx, my);
}

void GameWindow::rebuildBoardCache()
{
    const QSize s = size();
    if (s.isEmpty()) return;

    const qreal dpr = devicePixelRatioF();
    if (!boardCache_.isNull() && boardCache_.size() == s * dpr && qFuzzyCompare(boardCacheDpr_, dpr)) {
        return;
    }

    boardCacheDpr_ = dpr;

    QPixmap pm(int(s.width() * dpr), int(s.height() * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    QColor woodColor(210, 180, 140);
    p.fillRect(QRect(QPoint(0, 0), s), woodColor);

    const int ox = boardOriginX();
    const int oy = boardOriginY();

    QPen gridPen(QColor(80, 50, 20));
    gridPen.setWidth(qMax(1, int(cellSize_ * 0.05)));
    p.setPen(gridPen);

    for (int i = 0; i < BOARD_SIZE; ++i) {
        int y  = oy + i * cellSize_;
        int x1 = ox;
        int x2 = ox + (BOARD_SIZE - 1) * cellSize_;
        p.drawLine(x1, y, x2, y);

        int x  = ox + i * cellSize_;
        int y1 = oy;
        int y2 = oy + (BOARD_SIZE - 1) * cellSize_;
        p.drawLine(x, y1, x, y2);
    }

    {
        const QColor starColor(30, 20, 10);
        const int r = qMax(3, int(cellSize_ * 0.18));
        p.setBrush(starColor);
        p.setPen(Qt::NoPen);

        auto drawStar = [&](int bx, int by) {
            QPoint c(ox + bx * cellSize_, oy + by * cellSize_);
            p.drawEllipse(c, r, r);
        };

        drawStar(7, 7);
        drawStar(3, 3);
        drawStar(11, 3);
        drawStar(3, 11);
        drawStar(11, 11);
    }

    p.end();
    boardCache_ = pm;
}

QRect GameWindow::cellDirtyRect(int x, int y, int extraPx) const
{
    const int ox = boardOriginX();
    const int oy = boardOriginY();
    const int cx = ox + x * cellSize_;
    const int cy = oy + y * cellSize_;

    const int r  = int(cellSize_ / 2.0) + extraPx;
    QRect rc(cx - r, cy - r, 2 * r, 2 * r);
    return rc.intersected(rect());
}

QRect GameWindow::hoverDirtyRect(const QPoint &cell) const
{
    if (cell.x() < 0 || cell.y() < 0) return QRect();
    return cellDirtyRect(cell.x(), cell.y(), qMax(18, int(cellSize_ * 0.55)));
}

QRect GameWindow::countdownDirtyRect() const
{
    QRect rc(8, 6, 360, qMax(60, int(topExtra_ * 0.9)));
    return rc.intersected(rect());
}

QRect GameWindow::winFiveDirtyRect() const
{
    if (winFive_.isEmpty()) return QRect();

    int minx = 999, miny = 999, maxx = -999, maxy = -999;
    for (const QPoint &pt : winFive_) {
        minx = qMin(minx, pt.x());
        miny = qMin(miny, pt.y());
        maxx = qMax(maxx, pt.x());
        maxy = qMax(maxy, pt.y());
    }

    QRect a = cellDirtyRect(minx, miny, qMax(24, int(cellSize_ * 0.85)));
    QRect b = cellDirtyRect(maxx, maxy, qMax(24, int(cellSize_ * 0.85)));
    QRect rc = a.united(b).adjusted(-20, -20, 20, 20);
    return rc.intersected(rect());
}

void GameWindow::clearHover()
{
    if (hoverMode_ == HoverNone && hoverCell_ == QPoint(-1, -1)) return;

    const QPoint oldCell = hoverCell_;
    hoverCell_ = QPoint(-1, -1);
    hoverMode_ = HoverNone;

    const QRect dirty = hoverDirtyRect(oldCell);
    if (dirty.isValid()) update(dirty);
}

void GameWindow::setHoverCell(const QPoint &cell, HoverMode mode)
{
    if (hoverCell_ == cell && hoverMode_ == mode) return;

    const QPoint oldCell = hoverCell_;
    hoverCell_ = cell;
    hoverMode_ = mode;

    QRect dirty = hoverDirtyRect(oldCell).united(hoverDirtyRect(cell));
    dirty = dirty.intersected(rect());
    if (dirty.isValid()) update(dirty);
}

void GameWindow::initUiAndNetwork()
{
    QStringList roles;
    roles << "Host" << "Guest";

    bool ok = false;
    QString roleStr = QInputDialog::getItem(this, "Choose Role", "Role:", roles, 0, false, &ok);
    if (!ok) { close(); return; }

    QString roomId = QInputDialog::getText(this, "Room ID",
                                           "Enter room ID (e.g. room1):",
                                           QLineEdit::Normal,
                                           "room1",
                                           &ok);
    if (!ok || roomId.isEmpty()) { close(); return; }

    NetworkManager::Role role =
        (roleStr == "Host") ? NetworkManager::Host : NetworkManager::Guest;

    net_ = new NetworkManager(this);

    connect(net_, &NetworkManager::moveReceived, this, &GameWindow::onMoveReceived);
    connect(net_, &NetworkManager::stateReceived, this, &GameWindow::onStateReceived);
    connect(net_, &NetworkManager::presenceChanged, this, &GameWindow::onPresenceChanged);
    connect(net_, &NetworkManager::moveRejected, this, &GameWindow::onMoveRejected);

    connect(net_, &NetworkManager::p2pConnected, this, &GameWindow::onP2PConnected);
    connect(net_, &NetworkManager::logMessage, this, &GameWindow::onLogMessage);

    net_->start(role, roomId);

    amHost_  = (role == NetworkManager::Host);
    myColor_ = amHost_ ? 1 : 2;

    opponentPresent_ = false;
    serverNextColor_ = 1;

    serverGameOver_ = false;
    serverWinner_ = 0;

    lastSeqApplied_ = 0;
    state_ = GameState::WaitingConnection;

    if (amHost_) setWindowTitle(windowTitle() + " - Host(Black)");
    else         setWindowTitle(windowTitle() + " - Guest(White)");
}

void GameWindow::startMyTurn()
{
    if (serverGameOver_) {
        state_ = GameState::GameOver;
        turnTimer_.stop();
        clearHover();
        update(countdownDirtyRect());
        return;
    }

    if (!opponentPresent_) {
        state_ = GameState::WaitingOpponent;
        turnTimer_.stop();
        clearHover();
        update(countdownDirtyRect());
        return;
    }

    state_ = GameState::MyTurn;
    timeLeft_ = TURN_TIME_SECONDS;
    turnTimer_.start();
    update(countdownDirtyRect());
}

void GameWindow::endMyTurn()
{
    turnTimer_.stop();
    clearHover();
    update(countdownDirtyRect());
}

void GameWindow::onPresenceChanged(int onlineCount)
{
    const bool nowPresent = (onlineCount >= 2);
    if (opponentPresent_ == nowPresent) return;

    opponentPresent_ = nowPresent;
    update(countdownDirtyRect());

    if (!isConnected() || isGameOver()) return;

    if (!opponentPresent_ && state_ == GameState::MyTurn) {
        state_ = GameState::WaitingOpponent;
        turnTimer_.stop();
        clearHover();
        update(countdownDirtyRect());
        return;
    }

    if (opponentPresent_ && state_ == GameState::WaitingOpponent) {
        if (serverNextColor_ == myColor_) startMyTurn();
        else {
            state_ = GameState::OpponentTurn;
            update(countdownDirtyRect());
        }
    }
}

void GameWindow::onTurnTimerTick()
{
    if (!isConnected() || isGameOver() || !isMyTurn()) return;
    if (!opponentPresent_) return;
    if (serverGameOver_) return;

    timeLeft_--;
    update(countdownDirtyRect());

    if (timeLeft_ <= 0) autoMoveBest();
}

void GameWindow::onAnimTick()
{
    bool anyActive = false;
    const qint64 now = clock_.elapsed();

    QRect dirty;
    const int animExtra = qMax(16, int(cellSize_ * 0.60));
    const int markExtra = qMax(20, int(cellSize_ * 0.75));

    for (int y = 0; y < BOARD_SIZE; ++y) {
        for (int x = 0; x < BOARD_SIZE; ++x) {
            auto &a = anims_[y][x];
            if (!a.active) continue;

            qint64 dt = now - a.startMs;
            if (dt >= a.durationMs) {
                a.active = false;
                dirty = dirty.united(cellDirtyRect(x, y, animExtra));
                anyActive = true;
            } else if (dt >= 0) {
                dirty = dirty.united(cellDirtyRect(x, y, animExtra));
                anyActive = true;
            }
        }
    }

    // 最后一手红圈/红点闪烁期间刷新
    if (lastMove_.x() >= 0 && lastMove_.y() >= 0) {
        if (lastMarkBlinking_) {
            if (now - lastMarkStartMs_ >= LAST_MARK_BLINK_MS) {
                lastMarkBlinking_ = false;
            } else {
                dirty = dirty.united(cellDirtyRect(lastMove_.x(), lastMove_.y(), markExtra));
                anyActive = true;
            }
        }
    }

    if (winAnimActive_ || !winFive_.isEmpty()) {
        const int totalMs = WIN_STEP_MS * 5 + WIN_TAIL_MS;
        qint64 dt = now - winAnimStartMs_;
        if (winAnimActive_ && dt >= totalMs) winAnimActive_ = false;

        dirty = dirty.united(winFiveDirtyRect());
        anyActive = true;
    }

    if (!anyActive) {
        animTimer_.stop();
        return;
    }

    dirty = dirty.intersected(rect());
    if (dirty.isValid()) update(dirty);
}

void GameWindow::autoMoveBest()
{
    if (!isMyTurn() || isGameOver()) return;
    if (!opponentPresent_) return;
    if (serverGameOver_) return;

    QPoint best = findBestMove(myColor_);
    if (best.x() < 0) {
        endGameAndAskRestart("Game Over", "Draw!");
        return;
    }

    endMyTurn();
    state_ = GameState::OpponentTurn;
    update(countdownDirtyRect());

    if (net_) net_->sendMove(best.x(), best.y(), myColor_);
}

void GameWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    if (!qFuzzyCompare(boardCacheDpr_, devicePixelRatioF()) || boardCache_.isNull()) {
        rebuildBoardCache();
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.drawPixmap(0, 0, boardCache_);

    const int ox = boardOriginX();
    const int oy = boardOriginY();
    const qint64 now = clock_.elapsed();

    const qreal stoneRadius = cellSize_ / 2.0 - qMax(2.0, cellSize_ * 0.06);
    const int   stonePenW   = qMax(1, int(cellSize_ * 0.03));

    // ===== 棋子（含落子动画）=====
    for (int y = 0; y < BOARD_SIZE; ++y) {
        for (int x = 0; x < BOARD_SIZE; ++x) {
            int color = board_[y][x];
            if (color == 0) continue;

            QPointF center(ox + x * cellSize_, oy + y * cellSize_);

            qreal scale = 1.0;
            qreal alpha = 1.0;

            auto &a = anims_[y][x];
            if (a.active) {
                qint64 dt = now - a.startMs;
                if (dt >= a.durationMs) {
                    a.active = false;
                } else if (dt >= 0) {
                    qreal t = qreal(dt) / qreal(a.durationMs);
                    qreal e = 1.0 - (1.0 - t) * (1.0 - t);
                    scale = 0.80 + 0.20 * e;
                    alpha = e;
                }
            }

            p.save();
            p.translate(center);
            p.scale(scale, scale);
            p.setOpacity(alpha);

            QRadialGradient grad(QPointF(-stoneRadius / 3.0, -stoneRadius / 3.0), stoneRadius);
            if (color == 1) {
                grad.setColorAt(0.0, QColor(110, 110, 110));
                grad.setColorAt(0.35, QColor(50, 50, 50));
                grad.setColorAt(1.0, QColor(0, 0, 0));
                p.setPen(QPen(QColor(0, 0, 0), stonePenW));
            } else {
                grad.setColorAt(0.0, QColor(255, 255, 255));
                grad.setColorAt(0.5, QColor(245, 245, 245));
                grad.setColorAt(1.0, QColor(200, 200, 200));
                p.setPen(QPen(QColor(40, 40, 40), stonePenW));
            }

            p.setBrush(grad);
            p.drawEllipse(QPointF(0, 0), stoneRadius, stoneRadius);

            p.restore();
            p.setOpacity(1.0);
        }
    }

    // ===== Hover（半透明预览 + 禁止符号）=====
    if (isConnected()
        && !isGameOver()
        && !serverGameOver_
        && isMyTurn()
        && opponentPresent_
        && hoverCell_.x() >= 0 && hoverCell_.y() >= 0
        && hoverCell_.x() < BOARD_SIZE && hoverCell_.y() < BOARD_SIZE
        && hoverMode_ != HoverNone)
    {
        QPointF center(ox + hoverCell_.x() * cellSize_,
                       oy + hoverCell_.y() * cellSize_);

        if (hoverMode_ == HoverEmpty) {
            p.save();
            p.translate(center);
            p.setOpacity(0.38);
            p.scale(0.98, 0.98);

            QRadialGradient grad(QPointF(-stoneRadius / 3.0, -stoneRadius / 3.0), stoneRadius);
            if (myColor_ == 1) {
                grad.setColorAt(0.0, QColor(110, 110, 110));
                grad.setColorAt(0.35, QColor(50, 50, 50));
                grad.setColorAt(1.0, QColor(0, 0, 0));
                p.setPen(QPen(QColor(0, 0, 0), stonePenW));
            } else {
                grad.setColorAt(0.0, QColor(255, 255, 255));
                grad.setColorAt(0.5, QColor(245, 245, 245));
                grad.setColorAt(1.0, QColor(200, 200, 200));
                p.setPen(QPen(QColor(40, 40, 40), stonePenW));
            }
            p.setBrush(grad);
            p.drawEllipse(QPointF(0, 0), stoneRadius, stoneRadius);

            p.restore();
            p.setOpacity(1.0);
        } else if (hoverMode_ == HoverBlocked) {
            p.save();
            p.setOpacity(0.55);

            const qreal r = stoneRadius * 0.78;
            const qreal slash = r * 0.90;

            QPen pen(QColor(210, 30, 30));
            pen.setWidth(qMax(2, int(cellSize_ * 0.10)));
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);

            p.drawEllipse(center, r, r);
            p.drawLine(QPointF(center.x() - slash, center.y() + slash),
                       QPointF(center.x() + slash, center.y() - slash));

            QPen halo(QColor(255, 120, 120));
            halo.setWidth(qMax(1, int(cellSize_ * 0.05)));
            halo.setCapStyle(Qt::RoundCap);
            p.setPen(halo);
            p.drawEllipse(center, r + 2, r + 2);

            p.restore();
            p.setOpacity(1.0);
        }
    }

    // ===== 最后一手落子标记（红圈 + 红点）=====
    if (lastMove_.x() >= 0 && lastMove_.y() >= 0
        && lastMove_.x() < BOARD_SIZE && lastMove_.y() < BOARD_SIZE
        && board_[lastMove_.y()][lastMove_.x()] != 0)
    {
        const QPointF c(ox + lastMove_.x() * cellSize_,
                        oy + lastMove_.y() * cellSize_);

        bool show = true;
        if (lastMarkBlinking_ && (now - lastMarkStartMs_ < LAST_MARK_BLINK_MS)) {
            const int phase = int((now - lastMarkStartMs_) / LAST_MARK_TOGGLE_MS);
            show = (phase % 2 == 0);
        } else {
            lastMarkBlinking_ = false;
        }

        if (show) {
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);

            // 外圈
            QPen ringPen(QColor(220, 30, 30));
            ringPen.setWidth(qMax(2, int(cellSize_ * 0.10)));
            ringPen.setCapStyle(Qt::RoundCap);
            p.setPen(ringPen);
            p.setBrush(Qt::NoBrush);

            const qreal ringR = stoneRadius * 0.92;
            p.drawEllipse(c, ringR, ringR);

            // 内点
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(220, 30, 30));
            const qreal dotR = qMax(2.0, cellSize_ * 0.10);
            p.drawEllipse(c, dotR, dotR);

            // 轻微光晕（更醒目）
            QPen haloPen(QColor(255, 140, 140));
            haloPen.setWidth(qMax(1, int(cellSize_ * 0.05)));
            p.setPen(haloPen);
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, ringR + 2.0, ringR + 2.0);

            p.restore();
        }
    }

    // ===== 五连闪光 =====
    if (!winFive_.isEmpty()) {
        int litCount = 5;
        qreal glowAlpha = 0.55;

        if (winAnimActive_) {
            qint64 dt = now - winAnimStartMs_;
            litCount = qBound(0, int(dt / WIN_STEP_MS) + 1, 5);

            if (dt > WIN_STEP_MS * 5) {
                qreal t = qreal(dt - WIN_STEP_MS * 5) / qreal(WIN_TAIL_MS);
                t = qBound(0.0, t, 1.0);
                qreal breath = 0.65 + 0.35 * qSin(t * 3.1415926 * 2.0);
                glowAlpha = qBound(0.15, 0.75 * breath, 0.85);
            } else {
                qreal pulse = 0.70 + 0.30 * qSin(qreal(dt) / 140.0);
                glowAlpha = qBound(0.20, 0.60 * pulse, 0.85);
            }
        } else {
            glowAlpha = 0.45;
        }

        auto cellCenter = [&](const QPoint &pt) -> QPointF {
            return QPointF(ox + pt.x() * cellSize_, oy + pt.y() * cellSize_);
        };

        if (litCount >= 2) {
            p.save();
            QColor lineColor(255, 215, 0);
            lineColor.setAlphaF(glowAlpha);

            QPen linePen(lineColor);
            linePen.setWidth(qMax(6, int(cellSize_ * 0.25)));
            linePen.setCapStyle(Qt::RoundCap);
            linePen.setJoinStyle(Qt::RoundJoin);
            p.setPen(linePen);
            p.setBrush(Qt::NoBrush);

            QPainterPath path;
            path.moveTo(cellCenter(winFive_[0]));
            for (int i = 1; i < litCount; ++i) path.lineTo(cellCenter(winFive_[i]));
            p.drawPath(path);
            p.restore();
        }

        for (int i = 0; i < 5; ++i) {
            if (i >= litCount) break;

            QPointF cc = cellCenter(winFive_[i]);
            qreal r = stoneRadius + qMax(1.0, cellSize_ * 0.02);

            qreal localBoost = 1.0;
            if (winAnimActive_) {
                qint64 dt = now - winAnimStartMs_;
                int curIdx = qBound(0, int(dt / WIN_STEP_MS), 4);
                if (i == curIdx) localBoost = 1.35;
            }

            p.save();

            QColor ring(255, 215, 0);
            ring.setAlphaF(qBound(0.10, glowAlpha * 0.95 * localBoost, 0.95));

            QPen pen(ring);
            pen.setWidth(qMax(3, int(cellSize_ * 0.15 * localBoost)));
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(cc, r, r);

            QColor halo(255, 240, 170);
            halo.setAlphaF(qBound(0.05, glowAlpha * 0.35 * localBoost, 0.60));
            QPen haloPen(halo);
            haloPen.setWidth(qMax(6, int(cellSize_ * 0.35 * localBoost)));
            haloPen.setCapStyle(Qt::RoundCap);
            p.setPen(haloPen);
            p.drawEllipse(cc, r + 2, r + 2);

            p.restore();
        }
    }

    // ===== 倒计时 UI =====
    if (isConnected() && !isGameOver()) {
        const int ringSize = qMax(38, int(cellSize_ * 1.05));
        QRectF ringRect(12, 10, ringSize, ringSize);

        QPen ringPen(QColor(40, 40, 40));
        ringPen.setWidth(qMax(2, int(cellSize_ * 0.07)));
        p.setPen(ringPen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(ringRect);

        if (state_ == GameState::MyTurn) {
            qreal frac = qBound(0.0, qreal(timeLeft_) / qreal(TURN_TIME_SECONDS), 1.0);
            int startAngle = 90 * 16;
            int spanAngle  = int(-360.0 * frac * 16.0);

            QPen arcPen(timeLeft_ <= 5 ? QColor(200, 30, 30) : QColor(20, 120, 20));
            arcPen.setWidth(qMax(3, int(cellSize_ * 0.09)));
            arcPen.setCapStyle(Qt::RoundCap);
            p.setPen(arcPen);
            p.drawArc(ringRect.adjusted(4, 4, -4, -4), startAngle, spanAngle);
        }

        QFont tf = p.font();
        tf.setPointSize(qMax(10, int(cellSize_ * 0.30)));
        tf.setBold(true);
        p.setFont(tf);
        p.setPen((state_ == GameState::MyTurn && timeLeft_ <= 5) ? QColor(200, 30, 30) : QColor(20, 20, 20));

        if (state_ == GameState::MyTurn) p.drawText(ringRect, Qt::AlignCenter, QString::number(qMax(0, timeLeft_)));
        else                             p.drawText(ringRect, Qt::AlignCenter, "-");

        QFont sf = p.font();
        sf.setPointSize(qMax(10, int(cellSize_ * 0.30)));
        sf.setBold(true);
        p.setFont(sf);

        QString side = (myColor_ == 1) ? "Black" : "White";
        QString status;
        if (!opponentPresent_) status = "Waiting for opponent...";
        else if (state_ == GameState::MyTurn) status = QString("%1 turn").arg(side);
        else status = "Waiting for opponent move...";

        p.setPen(QColor(30, 30, 30));
        p.drawText(12 + ringSize + 10, 10 + ringSize - 14, status);
    }
}

QPoint GameWindow::coordFromMouse(const QPoint &pos) const
{
    const int ox = boardOriginX();
    const int oy = boardOriginY();

    int x = (pos.x() - ox + cellSize_ / 2) / cellSize_;
    int y = (pos.y() - oy + cellSize_ / 2) / cellSize_;

    if (x < 0 || x >= BOARD_SIZE || y < 0 || y >= BOARD_SIZE)
        return QPoint(-1, -1);
    return QPoint(x, y);
}

bool GameWindow::placeStone(int x, int y, int color)
{
    if (x < 0 || x >= BOARD_SIZE || y < 0 || y >= BOARD_SIZE) return false;
    if (board_[y][x] != 0) return false;

    board_[y][x] = color;

    if (placeSound_.source().isValid()) {
        if (placeSound_.isPlaying()) placeSound_.stop();
        placeSound_.play();
    }

    lastMove_ = QPoint(x, y);

    anims_[y][x].active = true;
    anims_[y][x].startMs = clock_.elapsed();
    anims_[y][x].durationMs = STONE_ANIM_MS;

    // 新落子：最后手标记闪烁
    lastMarkBlinking_ = true;
    lastMarkStartMs_  = clock_.elapsed();

    if (!animTimer_.isActive()) animTimer_.start();
    update();
    return true;
}

int GameWindow::countInDirection(int x, int y, int dx, int dy, int color) const
{
    int cnt = 0;
    int cx = x + dx;
    int cy = y + dy;
    while (cx >= 0 && cx < BOARD_SIZE && cy >= 0 && cy < BOARD_SIZE) {
        if (board_[cy][cx] != color) break;
        ++cnt;
        cx += dx;
        cy += dy;
    }
    return cnt;
}

bool GameWindow::checkWinAt(int x, int y, int color) const
{
    static const int dirs[4][2] = { {1,0},{0,1},{1,1},{1,-1} };
    for (auto &d : dirs) {
        int dx = d[0], dy = d[1];
        int total = 1
                    + countInDirection(x, y,  dx,  dy, color)
                    + countInDirection(x, y, -dx, -dy, color);
        if (total >= 5) return true;
    }
    return false;
}

bool GameWindow::isBoardFull() const
{
    for (int y = 0; y < BOARD_SIZE; ++y)
        for (int x = 0; x < BOARD_SIZE; ++x)
            if (board_[y][x] == 0) return false;
    return true;
}

void GameWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (!isConnected()) {
        QMessageBox::information(this, "Info", "Not connected yet. Please wait.");
        return;
    }
    if (serverGameOver_) {
        QMessageBox::information(this, "Info", "Game is over. Please restart.");
        return;
    }
    if (isGameOver()) return;
    if (!isMyTurn()) {
        QMessageBox::information(this, "Info", "Not your turn.");
        return;
    }
    if (!opponentPresent_) return;

    QPoint c = coordFromMouse(event->pos());
    if (c.x() < 0) return;
    if (board_[c.y()][c.x()] != 0) return;

    clearHover();
    endMyTurn();
    state_ = GameState::OpponentTurn;
    update(countdownDirtyRect());

    if (net_) net_->sendMove(c.x(), c.y(), myColor_);
}

void GameWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!isConnected() || isGameOver() || !isMyTurn() || !opponentPresent_ || serverGameOver_) {
        clearHover();
        return;
    }

    QPoint c = coordFromMouse(event->pos());
    if (c.x() < 0) { clearHover(); return; }

    if (board_[c.y()][c.x()] == 0) setHoverCell(c, HoverEmpty);
    else                           setHoverCell(c, HoverBlocked);
}

void GameWindow::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    clearHover();
}

void GameWindow::onMoveRejected(const QString &reason)
{
    QMessageBox::information(this, "Move Rejected", "Reason: " + reason);
    if (reason == "game_over") {
        serverGameOver_ = true;
        state_ = GameState::GameOver;
        endMyTurn();
        clearHover();
        update();
    } else {
        if (serverNextColor_ == myColor_) startMyTurn();
        else { state_ = GameState::OpponentTurn; update(countdownDirtyRect()); }
    }
}

void GameWindow::onStateReceived(const QVector<int> &flatBoard, int nextColor, int lastX, int lastY, int seq,
                                 int onlineCount, bool gameOver, int winner, const QVector<QPoint> &winFive)
{
    if (seq < lastSeqApplied_) return;
    lastSeqApplied_ = seq;

    opponentPresent_ = (onlineCount >= 2);
    serverNextColor_ = nextColor;

    serverGameOver_ = gameOver;
    serverWinner_ = winner;

    endMyTurn();
    clearHover();

    if (flatBoard.size() == BOARD_SIZE * BOARD_SIZE) {
        int k = 0;
        for (int y = 0; y < BOARD_SIZE; ++y)
            for (int x = 0; x < BOARD_SIZE; ++x)
                board_[y][x] = flatBoard[k++];
    }

    for (auto &row : anims_)
        for (auto &a : row) { a.active = false; a.startMs = 0; a.durationMs = STONE_ANIM_MS; }

    lastMove_ = QPoint(lastX, lastY);
    // 重连时不闪烁，但必须显示红圈红点
    lastMarkBlinking_ = false;
    lastMarkStartMs_  = clock_.elapsed();

    if (serverGameOver_) {
        QString text = (serverWinner_ == 1) ? "Black wins!" : (serverWinner_ == 2) ? "White wins!" : "Game Over";
        if (winFive.size() == 5) {
            startWinFiveAnim(winFive, "Game Over", text, serverWinner_);
        } else {
            state_ = GameState::GameOver;
            update();
        }
        return;
    }

    winFive_.clear();
    winAnimActive_ = false;
    winAnimStartMs_ = 0;
    winAnimWinnerColor_ = 0;
    pendingTitle_.clear();
    pendingText_.clear();
    winAnimDelayTimer_.stop();

    if (nextColor == myColor_) startMyTurn();
    else state_ = opponentPresent_ ? GameState::OpponentTurn : GameState::WaitingOpponent;

    update();
}

void GameWindow::onMoveReceived(int x, int y, int color, int seq, int nextColor,
                                bool gameOver, int winner, const QVector<QPoint> &winFive)
{
    if (seq <= lastSeqApplied_) return;
    lastSeqApplied_ = seq;

    if (!placeStone(x, y, color)) return;

    serverNextColor_ = nextColor;
    serverGameOver_ = gameOver;
    serverWinner_ = winner;

    if (serverGameOver_) {
        QString text = (serverWinner_ == 1) ? "Black wins!" : "White wins!";
        if (winFive.size() == 5) startWinFiveAnim(winFive, "Game Over", text, serverWinner_);
        else endGameAndAskRestart("Game Over", text);
        return;
    }

    if (isBoardFull()) {
        endGameAndAskRestart("Game Over", "Draw!");
        return;
    }

    if (nextColor == myColor_) startMyTurn();
    else state_ = opponentPresent_ ? GameState::OpponentTurn : GameState::WaitingOpponent;

    update(countdownDirtyRect());
}

void GameWindow::onP2PConnected()
{
    QMessageBox::information(this, "Connected", "Room joined. Waiting opponent / state sync.");
    state_ = GameState::WaitingOpponent;
    update(countdownDirtyRect());
}

void GameWindow::onLogMessage(const QString &msg)
{
    qDebug("%s", qPrintable(msg));
}

void GameWindow::resetBoardToNewGame()
{
    endMyTurn();

    for (auto &row : board_) row.fill(0);
    for (auto &row : anims_)
        for (auto &a : row) { a.active = false; a.startMs = 0; a.durationMs = STONE_ANIM_MS; }

    lastMove_ = QPoint(-1, -1);
    lastMarkBlinking_ = false;
    lastMarkStartMs_ = 0;

    timeLeft_ = TURN_TIME_SECONDS;

    winFive_.clear();
    winAnimActive_ = false;
    winAnimStartMs_ = 0;
    winAnimWinnerColor_ = 0;

    pendingTitle_.clear();
    pendingText_.clear();
    winAnimDelayTimer_.stop();

    hoverCell_ = QPoint(-1, -1);
    hoverMode_ = HoverNone;

    serverGameOver_ = false;
    serverWinner_ = 0;

    update();
}

bool GameWindow::showResultDialog(bool iWin, const QString &title, const QString &text)
{
    const QPixmap &img = iWin ? winImg_ : loseImg_;

    QDialog dlg(this);
    dlg.setModal(true);
    dlg.setWindowTitle(title);

    QVBoxLayout *root = new QVBoxLayout(&dlg);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    QLabel *imageLabel = new QLabel(&dlg);
    imageLabel->setAlignment(Qt::AlignCenter);

    if (!img.isNull()) {
        QSize target = img.size();
        if (target.width() > 1100 || target.height() > 700)
            target.scale(1100, 700, Qt::KeepAspectRatio);
        imageLabel->setPixmap(img.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        imageLabel->setText(text);
        QFont f = imageLabel->font();
        f.setPointSize(f.pointSize() + 4);
        f.setBold(true);
        imageLabel->setFont(f);
    }
    root->addWidget(imageLabel, 1);

    QHBoxLayout *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);

    QPushButton *btnRestart = new QPushButton(QStringLiteral("再来一局"), &dlg);
    QPushButton *btnQuit    = new QPushButton(QStringLiteral("退出"), &dlg);
    btnRestart->setDefault(true);

    btnRow->addWidget(btnRestart);
    btnRow->addSpacing(8);
    btnRow->addWidget(btnQuit);
    btnRow->addStretch(1);
    root->addLayout(btnRow);

    bool restart = false;
    connect(btnRestart, &QPushButton::clicked, &dlg, [&]() { restart = true; dlg.accept(); });
    connect(btnQuit,    &QPushButton::clicked, &dlg, [&]() { restart = false; dlg.reject(); });

    dlg.resize(900, 520);
    dlg.exec();
    return restart;
}

void GameWindow::doServerRestart()
{
    resetBoardToNewGame();
    state_ = opponentPresent_ ? GameState::OpponentTurn : GameState::WaitingOpponent;
    update(countdownDirtyRect());
    if (net_) net_->sendReset();
}

void GameWindow::finalizeGame(const QString &title, const QString &text)
{
    if (!isGameOver()) return;

    bool iWin = false;
    if (text.contains("Black") && myColor_ == 1) iWin = true;
    if (text.contains("White") && myColor_ == 2) iWin = true;

    const bool restart = showResultDialog(iWin, title, text);
    if (restart) { doServerRestart(); return; }
    close();
}

void GameWindow::endGameAndAskRestart(const QString &title, const QString &text)
{
    endMyTurn();
    state_ = GameState::GameOver;
    clearHover();
    finalizeGame(title, text);
}

void GameWindow::startWinFiveAnim(const QVector<QPoint> &five, const QString &title, const QString &text, int winnerColor)
{
    endMyTurn();
    state_ = GameState::GameOver;

    clearHover();

    winFive_ = five;
    winAnimWinnerColor_ = winnerColor;

    winAnimActive_ = true;
    winAnimStartMs_ = clock_.elapsed();

    pendingTitle_ = title;
    pendingText_  = text;

    if (!animTimer_.isActive()) animTimer_.start();

    const int totalMs = WIN_STEP_MS * 5 + WIN_TAIL_MS;
    winAnimDelayTimer_.start(totalMs);

    update(winFiveDirtyRect());
}

// ===== AI（保持原样）=====
void GameWindow::countLineInfo(int x, int y, int dx, int dy, int color,
                               int &countForward, int &countBackward, int &openEnds) const
{
    countForward = 0; countBackward = 0; openEnds = 0;

    int cx = x + dx, cy = y + dy;
    while (cx >= 0 && cx < BOARD_SIZE && cy >= 0 && cy < BOARD_SIZE && board_[cy][cx] == color) {
        countForward++; cx += dx; cy += dy;
    }
    if (cx >= 0 && cx < BOARD_SIZE && cy >= 0 && cy < BOARD_SIZE && board_[cy][cx] == 0) openEnds++;

    cx = x - dx; cy = y - dy;
    while (cx >= 0 && cx < BOARD_SIZE && cy >= 0 && cy < BOARD_SIZE && board_[cy][cx] == color) {
        countBackward++; cx -= dx; cy -= dy;
    }
    if (cx >= 0 && cx < BOARD_SIZE && cy >= 0 && cy < BOARD_SIZE && board_[cy][cx] == 0) openEnds++;
}

int GameWindow::evaluateCellScore(int x, int y, int color) const
{
    if (board_[y][x] != 0) return -1;

    static const int dirs[4][2] = { {1,0},{0,1},{1,1},{1,-1} };

    auto patternScore = [](int total, int openEnds) -> int {
        if (total >= 5) return 10000000;
        if (total == 4 && openEnds == 2) return 1000000;
        if (total == 4 && openEnds == 1) return 100000;
        if (total == 3 && openEnds == 2) return 20000;
        if (total == 3 && openEnds == 1) return 2000;
        if (total == 2 && openEnds == 2) return 500;
        if (total == 2 && openEnds == 1) return 80;
        if (total == 1 && openEnds == 2) return 20;
        return 5;
    };

    int bestDir = 0;
    for (auto &d : dirs) {
        int f, b, open;
        countLineInfo(x, y, d[0], d[1], color, f, b, open);
        int total = 1 + f + b;
        bestDir = qMax(bestDir, patternScore(total, open));
    }
    return bestDir;
}

QPoint GameWindow::findBestMove(int color) const
{
    int enemy = (color == 1) ? 2 : 1;

    int bestScore = -1;
    QPoint best(-1, -1);

    auto centerBias = [&](int x, int y) -> int {
        int cx = BOARD_SIZE / 2;
        int cy = BOARD_SIZE / 2;
        return -(qAbs(x - cx) + qAbs(y - cy));
    };

    for (int y = 0; y < BOARD_SIZE; ++y) {
        for (int x = 0; x < BOARD_SIZE; ++x) {
            if (board_[y][x] != 0) continue;

            int attack = evaluateCellScore(x, y, color);
            int defend = evaluateCellScore(x, y, enemy);

            int score = attack + defend * 12 / 10;
            score = score * 10 + centerBias(x, y);

            if (score > bestScore) {
                bestScore = score;
                best = QPoint(x, y);
            }
        }
    }
    return best;
}
