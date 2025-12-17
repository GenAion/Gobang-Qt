# Gobang-Qt 项目详细说明

本项目是一个 **基于 Qt / C++ 的联机五子棋（Gomoku）客户端**，采用 **中心信令服务器 + P2P（可扩展 TURN）** 的网络架构，适合作为：

* Qt / C++ 综合实战项目
* 棋牌游戏客户端范例
* 网络同步 + UI 渲染 + 动画状态机的学习样例

本文档结合项目源码，对**整体架构、核心模块、关键流程与扩展方式**进行系统说明，便于他人学习、理解与二次开发。

---

## 一、项目整体结构

```
Gobang-Qt/
├── main.cpp                # 程序入口
├── GameWindow.h / .cpp     # 主界面 + 游戏逻辑（核心）
├── NetworkManager.h / .cpp # TCP 信令客户端
├── TurnClient.h / .cpp     # TURN 客户端（NAT 穿透，进阶）
├── resources.qrc           # Qt 资源文件（图片 / 音效）
├── Gobang.pro              # QMake 工程文件
└── README.md / 说明文档
```

**职责划分原则**：

* `GameWindow`：只关心“游戏与显示”，不直接处理底层网络协议
* `NetworkManager`：只负责“和服务器通信、同步状态”
* `TurnClient`：提供“P2P/NAT 穿透能力”，与游戏逻辑解耦

---

## 二、程序入口（main.cpp）

```cpp
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    GameWindow w;
    w.show();
    return a.exec();
}
```

特点：

* 使用 `QApplication`（Widgets 程序）
* **GameWindow 即整个程序的主窗口与核心控制器**
* 没有额外 Controller / Manager，结构清晰，适合教学

---

## 三、GameWindow：核心模块详解

`GameWindow` 同时承担三类职责：

1. **棋盘与棋子渲染（UI）**
2. **本地游戏规则与状态机**
3. **与 NetworkManager 的事件交互**

### 3.1 棋盘与数据结构

```cpp
static constexpr int BOARD_SIZE = 15;
QVector<QVector<int>> board_; // 0=空, 1=黑, 2=白
```

* 使用二维数组表示棋盘
* 所有网络状态、重连同步，最终都会还原到 `board_`

### 3.2 游戏状态机（非常关键）

```cpp
enum class GameState {
    WaitingConnection,
    WaitingOpponent,
    MyTurn,
    OpponentTurn,
    GameOver
};
```

**这是整个程序最重要的“隐性主线”**：

* UI 显示
* 鼠标是否可落子
* 倒计时是否运行
* Hover 是否显示

全部由 `state_` 决定。

> 学习建议：先完全读懂 `startMyTurn()` / `endMyTurn()` / `onStateReceived()`

---

## 四、绘制系统（paintEvent）

`paintEvent()` 中完成了 **分层绘制**：

1. **棋盘背景（缓存 QPixmap）**
2. 棋子（带缩放 + 渐变 + 动画）
3. Hover 预览（半透明 / 禁止符号）
4. 最后一手红圈红点（可闪烁）
5. 五连高亮动画（路径 + 光晕）
6. 回合倒计时 UI

### 4.1 为什么要用 boardCache_？

```cpp
QPixmap boardCache_;
```

* 棋盘格线 **不会频繁变化**
* 使用缓存显著降低 repaint 开销
* resize / DPI 变化时才重建

这是 Qt 图形性能优化的一个**标准范式**。

---

## 五、交互逻辑（鼠标 & Hover）

### 5.1 鼠标移动（预览落子）

```cpp
void mouseMoveEvent(QMouseEvent *event)
```

逻辑：

1. 当前是否 `MyTurn`
2. 对手是否在线
3. 坐标是否合法
4. 格子是否为空

→ 决定 `HoverEmpty / HoverBlocked / HoverNone`

### 5.2 鼠标释放（真正落子）

```cpp
void mouseReleaseEvent(QMouseEvent *event)
```

**注意**：

* 本地并不直接 `placeStone`
* 而是通过 `NetworkManager::sendMove()`
* 最终以服务器返回的 move/state 为准

这是 **联机游戏一致性设计的关键点**。

---

## 六、NetworkManager：信令通信设计

### 6.1 网络模型

* 协议：TCP
* 数据格式：JSON（一行一条）
* 架构：中心信令服务器

```json
{
  "type": "move",
  "x": 7,
  "y": 7,
  "color": 1
}
```

### 6.2 核心信号

```cpp
stateReceived(...)
moveReceived(...)
moveRejected(...)
presenceChanged(int)
```

GameWindow **完全通过 Qt signal/slot 与网络解耦**，这是本项目在架构上的一个亮点。

---

## 七、断线重连与状态同步

当客户端重连或新玩家加入时：

```cpp
NetworkManager::requestState();
```

服务器返回：

* 完整棋盘
* lastMove
* nextColor
* 胜负状态

客户端通过：

```cpp
onStateReceived(...)
```

**完整重建 UI 状态**，而不是增量修补。

> 这是“可恢复联机棋类”的标准做法。

---

## 八、TurnClient（进阶模块说明）

`TurnClient` 是一个 **纯 C++ 实现的 TURN 协议客户端**：

* UDP
* STUN/TURN 消息编码
* Allocate / Refresh / Permission

当前项目中：

* 已实现完整功能
* 尚未强耦合进对局流程

适合：

* 学习 NAT 穿透原理
* 后续扩展为真正的 P2P 对战

> 对初学者：可先忽略该模块，不影响主流程理解。

---

## 九、如何编译与运行

### 9.1 环境要求

* Qt 5.15.x
* MSVC 2019（Windows）
* Qt Creator

### 9.2 运行步骤

1. 启动信令服务器（Python，端口 5000）
2. 修改 `NetworkManager.h` 中服务器 IP
3. Qt Creator 打开 `Gobang.pro`
4. 构建并运行
5. 选择 Host / Guest，输入同一 Room ID

---

## 十、适合的学习路径建议

### 初级（Qt 入门）

* 看懂 paintEvent + mouseEvent
* 理解 QWidget 自绘

### 中级（工程能力）

* 理解 GameState 状态机
* 学习 signal/slot 解耦设计

### 高级（网络 / 实时系统）

* 阅读 NetworkManager 协议
* 理解断线重连一致性
* 扩展 TurnClient → P2P

---

## 十一、可扩展方向

* 单机 AI（已预留 findBestMove）
* 棋谱保存 / 回放
* 观战模式
* 聊天系统
* 真正的 P2P 对战（TurnClient）

---

## 十二、总结

这是一个 **完整度很高、结构清晰、工程味道很强的 Qt/C++ 项目**，非常适合作为：

* 课程设计 / 毕业设计参考
* Qt 联机项目模板
* 游戏客户端架构学习样例

如果你是本项目的学习者，**强烈建议按模块逐个精读，而不是一次性通读全部源码**。
