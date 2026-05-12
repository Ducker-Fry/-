# datalink.c 代码详解与 Go-Back-N 改造指南

## 一、代码逐段详解

### 1. 头文件与宏定义

```c
#include <stdio.h>
#include <string.h>

#include "protocol.h"   // 仿真环境 API（事件、网络层、物理层、计时器、CRC）
#include "datalink.h"   // 帧类型定义：FRAME_DATA=1, FRAME_ACK=2, FRAME_NAK=3

#define DATA_TIMER  2000   // 数据帧超时时间：2000ms = 2秒
```

### 2. 帧结构体定义

```c
struct FRAME {
    unsigned char kind;    // 帧类型：DATA / ACK / NAK
    unsigned char ack;     // 捎带确认号（ACK 帧中就是确认号）
    unsigned char seq;     // 发送序号
    unsigned char data[PKT_LEN];  // 数据载荷，256 字节（PKT_LEN=256）
    unsigned int  padding;  // 填充字段，用于 CRC 时保证结构体大小
};
```

对应帧格式：`kind(1) + seq(1) + ack(1) + data(256) = 259字节`，发送时会在尾部加 4 字节 CRC，共 263 字节。

### 3. 全局状态变量

```c
static unsigned char frame_nr = 0, buffer[PKT_LEN], nbuffered;
//     frame_nr  — 发送方：当前要发送的帧序号（0 或 1）
//     buffer[]  — 发送缓冲区，存一份当前待确认的数据包副本
//     nbuffered — 已从网络层取出但尚未确认的包数量（当前最大为 1）

static unsigned char frame_expected = 0;
//     frame_expected — 接收方：期望收到的下一帧序号（0 或 1）

static int phl_ready = 0;
//     phl_ready — 物理层是否就绪（PHYSICAL_LAYER_READY 事件触发后置 1）
```

核心设计思想：序号空间只有 0/1 两个值（1 位），所以这是典型的**停止等待协议**——发一帧等一帧确认，`nbuffered` 最多为 1。

### 4. 帧发送辅助函数 `put_frame()`

```c
static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);  // 在帧尾部计算并填入 4 字节 CRC
    send_frame(frame, len + 4);  // 发送帧 + 4 字节 CRC
    phl_ready = 0;  // 发送后标记物理层为"忙"，防止连续调用 send_frame
}
```

关键点：CRC 覆盖帧的全部内容（kind + seq + ack + data），CRC 放在帧尾。接收方收到后对整个帧（含 CRC）再做一次 CRC，结果应为 0 才表示校验通过。

### 5. 发送数据帧 `send_data_frame()`

```c
static void send_data_frame(void)
{
    struct FRAME s;

    s.kind = FRAME_DATA;              // 帧类型：数据帧
    s.seq = frame_nr;                 // 填入当前发送序号
    s.ack = 1 - frame_expected;       // 捎带确认：告诉对方我期望的下一帧序号
    memcpy(s.data, buffer, PKT_LEN);  // 从缓冲区拷贝数据载荷

    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

    put_frame((unsigned char *)&s, 3 + PKT_LEN);  // 发送帧头(3字节)+数据(256字节)=259字节
    start_timer(frame_nr, DATA_TIMER);  // 启动定时器，监控该序号的帧是否超时
}
```

执行时机：当网络层就绪（`NETWORK_LAYER_READY`）且取到新包时调用。

### 6. 发送确认帧 `send_ack_frame()`

```c
static void send_ack_frame(void)
{
    struct FRAME s;

    s.kind = FRAME_ACK;               // 帧类型：ACK 帧
    s.ack = 1 - frame_expected;       // 确认号：等于我已正确接收到的帧序号

    dbg_frame("Send ACK  %d\n", s.ack);

    put_frame((unsigned char *)&s, 2);  // ACK 帧只发送 kind(1) + ack(1) = 2 字节
}
```

注意：ACK 帧只有 kind 和 ack 两个字段，没有 seq 字段。结构体中的 `data` 和 `padding` 不计入发送长度。

### 7. 主函数 — 协议初始化

```c
int main(int argc, char **argv)
{
    int event, arg;
    struct FRAME f;   // 接收帧用的临时变量
    int len = 0;

    protocol_init(argc, argv);   // 初始化仿真环境（建立 TCP 连接、配置参数等）
    lprintf("Designed by Jiang Yanjun, build: " __DATE__"  "__TIME__"\n");

    disable_network_layer();     // 初始暂停网络层，等物理层就绪后再启动
```

初始必须 `disable_network_layer()`，因为物理层还没准备好发送。等 `PHYSICAL_LAYER_READY` 事件到来后才会 `enable_network_layer()`。

### 8. 主循环 — 事件驱动

```c
    for (;;) {
        event = wait_for_event(&arg);  // 阻塞等待事件，arg 在 DATA_TIMEOUT 时保存超时帧序号
```

事件驱动模型：`wait_for_event()` 内部做了大量工作：
1. 从套接字接收数据，组装成帧放入接收队列
2. 检查网络层是否有新包可发
3. 检查所有定时器是否超时
4. 检查物理层是否可继续发送
5. 所有这些检查后，返回一个事件类型 + 可能的参数

### 9. 事件处理 — NETWORK_LAYER_READY（网络层就绪）

```c
        case NETWORK_LAYER_READY:
            get_packet(buffer);   // 从网络层取 256 字节数据包，存入 buffer
            nbuffered++;          // 缓冲区中未确认包数 +1
            send_data_frame();    // 封装成 DATA 帧并发送
            break;
```

停止等待的局限：`nbuffered` 最多到 1，所以取了一个包后就会 `disable_network_layer()`（见末尾逻辑），无法连续取包发送。

### 10. 事件处理 — PHYSICAL_LAYER_READY（物理层就绪）

```c
        case PHYSICAL_LAYER_READY:
            phl_ready = 1;   // 标记物理层可发送
            break;
```

物理层发送队列有空位时触发，此时可以安全地调用 `send_frame()`。

### 11. 事件处理 — FRAME_RECEIVED（收到帧）

```c
        case FRAME_RECEIVED:
            len = recv_frame((unsigned char *)&f, sizeof f);  // 接收帧到结构体 f
```

**第一步：CRC 校验**

```c
            if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
                dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                break;   // CRC 不通过，直接丢弃该帧
            }
```

`crc32(data, len)` 对整个帧计算 CRC，包括帧尾的 4 字节 CRC 字段。如果传输无错误，结果应为 0。非 0 则说明帧在传输中被损坏（模拟信道按误码率翻转比特位）。

**第二步：处理 ACK 帧**

```c
            if (f.kind == FRAME_ACK)
                dbg_frame("Recv ACK  %d\n", f.ack);
```

仅打印日志，ACK 的真正处理逻辑在后面。

**第三步：处理 DATA 帧**

```c
            if (f.kind == FRAME_DATA) {
                dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
                if (f.seq == frame_expected) {
                    // 序号匹配！正是我期待的帧
                    put_packet(f.data, len - 7);  // 向上交付数据
                    frame_expected = 1 - frame_expected;  // 翻转期望序号
                }
                send_ack_frame();  // 无论是否期望的帧，都回复 ACK
            }
```

重要机制：
- 收到的帧序号 == `frame_expected` → 向上交付，翻转期望序号
- 收到的帧序号 != `frame_expected` → 重复帧，丢弃数据但**仍然回复 ACK**
- `send_ack_frame()` 中 `ack = 1 - frame_expected`，即告诉对方"我已正确收到的是这个序号"

**第四步：处理捎带确认（滑动发送窗口）**

```c
            if (f.ack == frame_nr) {
                stop_timer(frame_nr);  // 停止该帧的超时定时器
                nbuffered--;           // 未确认包数 -1
                frame_nr = 1 - frame_nr;  // 翻转发送序号：可以发下一帧了
            }
```

确认逻辑：收到的帧（无论是 DATA 还是 ACK）中的 `ack` 字段如果等于我当前发送的 `frame_nr`，说明对方已经收到了我发的帧，我可以：取消定时器、清空缓冲区、翻转序号准备发下一个。

### 12. 事件处理 — DATA_TIMEOUT（超时重传）

```c
        case DATA_TIMEOUT:
            dbg_event("---- DATA %d timeout\n", arg);
            send_data_frame();  // 重新发送当前帧（buffer 中还保存着副本）
            break;
```

`arg` 是超时的帧序号。停止等待协议只有一个未确认帧，所以直接重发。

### 13. 流控逻辑（主循环末尾）

```c
        if (nbuffered < 1 && phl_ready)
            enable_network_layer();   // 没有未确认包 且 物理层就绪 → 可以取新包
        else
            disable_network_layer();  // 否则暂停网络层，防止取包后无法发送
    }
}
```

停止等待的关键约束：`nbuffered < 1` 意味着只有当前没有待确认包时才能取新包。Go-Back-N 需要把这里改为 `nbuffered < WINDOW_SIZE`。

---

## 二、当前协议的数据流

```
【停止等待协议 — 窗口大小 = 1】

发送方 A                              接收方 B
--------                              --------
NETWORK_LAYER_READY
  → get_packet → buffer
  → nbuffered = 1
  → send_data_frame(seq=0)  ──────→  FRAME_RECEIVED
  → start_timer(0)                     → CRC 校验通过
  → disable_network_layer()            → seq == frame_expected(0) ✓
                                       → put_packet → 向上交付
                                       → frame_expected = 1
                                       → send_ack_frame(ack=0)
                                       （ack=0 表示"确认收到了0号帧"）

FRAME_RECEIVED  ←────────  ACK 帧 ────
  → f.ack(0) == frame_nr(0) ✓
  → stop_timer(0), nbuffered=0
  → frame_nr = 1
  → 末尾 enable_network_layer() → 可以取下一个包
```

关于 ACK 语义的关键说明：
- `send_ack_frame()` 中 `s.ack = 1 - frame_expected`
- 如果接收方刚收到了 seq=0 的帧，则 `frame_expected` 变成 1
- 此时 `s.ack = 1 - 1 = 0`，回复的 ACK 帧 ack 字段是 `0`，意思是"我确认收到了 0 号帧"
- 确认规则：`f.ack == frame_nr` — 如果收到帧中的 ack 字段等于我当前发送的序号，说明对方确认收到了该帧

---

## 三、如何改造为 Go-Back-N 协议

只需改动 `datalink.c` 一个文件，具体改动如下：

### 改动一：扩展常量

```c
#define WINDOW_SIZE  8    // 发送窗口大小
#define MAX_SEQ      15   // 序号空间：0~15（4 位序号，窗口 ≤ 序号空间一半）
```

### 改动二：扩展全局变量

原来的：
```c
static unsigned char frame_nr = 0, buffer[PKT_LEN], nbuffered;
static unsigned char frame_expected = 0;
static int phl_ready = 0;
```

改为：
```c
static unsigned char frame_expected = 0;       // 接收方：期望收到的帧序号
static int phl_ready = 0;

// 发送方新增变量
static unsigned char ack_expected = 0;         // 最早未确认帧的序号（窗口下沿）
static unsigned char next_frame_to_send = 0;   // 下一个要发送的帧序号（窗口上沿）
static unsigned char nbuffered = 0;            // 窗口内已缓冲的包数量
static unsigned char send_buf[MAX_SEQ + 1][PKT_LEN];  // 发送缓冲区，每个未确认帧存一份
```

### 改动三：修改 `send_data_frame()` — 支持指定序号

```c
static void send_data_frame(unsigned char seq_nr)
{
    struct FRAME s;
    s.kind = FRAME_DATA;
    s.seq = seq_nr;                          // 使用参数指定的序号
    s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);  // 捎带确认
    memcpy(s.data, send_buf[seq_nr], PKT_LEN);
    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);
    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(seq_nr, DATA_TIMER);
}
```

### 改动四：修改 `send_ack_frame()` — 累计确认号

```c
static void send_ack_frame(void)
{
    struct FRAME s;
    s.kind = FRAME_ACK;
    s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);  // 最后一个正确收到的帧序号
    dbg_frame("Send ACK  %d\n", s.ack);
    put_frame((unsigned char *)&s, 2);
}
```

### 改动五：NETWORK_LAYER_READY — 支持连续取包

```c
case NETWORK_LAYER_READY:
    nbuffered++;
    get_packet(send_buf[next_frame_to_send]);  // 数据存入对应序号的缓冲区
    send_data_frame(next_frame_to_send);
    next_frame_to_send = (next_frame_to_send + 1) % (MAX_SEQ + 1);
    break;
```

### 改动六：FRAME_RECEIVED — 累计 ACK 滑动窗口

**累计确认处理（合并 ACK 帧和 DATA 帧的捎带确认）**：
```c
// 累计确认：ack 字段表示"所有 ≤ 该序号的帧都已收到"
// 用 while 循环逐个确认从 ack_expected 到 f.ack 的所有帧
while (ack_expected != f.ack) {
    stop_timer(ack_expected);
    nbuffered--;
    ack_expected = (ack_expected + 1) % (MAX_SEQ + 1);
}
```

**DATA 帧处理**：
```c
if (f.kind == FRAME_DATA) {
    if (f.seq == frame_expected) {
        put_packet(f.data, len - 7);
        frame_expected = (frame_expected + 1) % (MAX_SEQ + 1);
        send_ack_frame();
    } else {
        send_ack_frame();  // 收到乱序帧也要回复累计 ACK
    }
}
```

### 改动七：DATA_TIMEOUT — Go-Back-N 批量重传

```c
case DATA_TIMEOUT:
    dbg_event("---- DATA %d timeout\n", arg);
    // 从最早未确认帧开始，重传窗口内所有已发未确认帧
    unsigned char i;
    for (i = ack_expected; i != next_frame_to_send; i = (i + 1) % (MAX_SEQ + 1)) {
        send_data_frame(i);
    }
    break;
```

### 改动八：流控逻辑

```c
// 原来的 if (nbuffered < 1 && phl_ready)
// 改为
if (nbuffered < WINDOW_SIZE && phl_ready)
    enable_network_layer();
else
    disable_network_layer();
```

---

## 四、改动汇总对照表

| 项目 | 停止等待（原来） | Go-Back-N（改后） |
|---|---|---|
| 序号空间 | 0~1（1位） | 0~15（4位），`MAX_SEQ = 15` |
| 发送窗口 | 1 | 8（`WINDOW_SIZE`） |
| 缓冲区 | `buffer[PKT_LEN]` 一个 | `send_buf[16][PKT_LEN]` 二维数组 |
| 取包时机 | 无待确认包时 | 窗口未满时 |
| 确认方式 | 逐帧确认 | 累计确认 |
| 重传方式 | 重发当前帧 | 从 `ack_expected` 开始重发所有已发未确认帧 |
| 发送函数 | `send_data_frame()` 无参 | `send_data_frame(seq)` 指定序号 |
