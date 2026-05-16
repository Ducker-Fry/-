#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"

/*
 * ============================================================
 *  Go-Back-N 协议实现
 * ============================================================
 *
 *  协议参数:
 *    发送窗口大小 = 7 (MAX_SEQ)
 *    序号空间     = 8 (0~7, 模8循环)
 *    定时器超时   = 2000ms (每个发送帧独立计时)
 *
 *  帧格式:
 *    DATA帧: [KIND(1)] [SEQ(1)] [ACK(1)] [DATA(256)] [CRC(4)]
 *    ACK 帧: [KIND(1)] [ACK(1)] [CRC(4)]
 *
 *  核心机制:
 *    1. 发送方维护滑动窗口 [ack_expected, next_frame_to_send)
 *    2. 接收方只接受按序到达的帧 (seq == frame_expected)
 *    3. 累积确认: ACK=N 表示 N 之前所有帧均已收到
 *    4. 超时回退: 定时器超时后重传窗口中所有未确认帧
 *    5. 捎带确认: DATA帧携带ACK字段，双向通信时减少ACK帧数量
 * ============================================================
 */

/* 定时器超时时间 (毫秒) */
#define DATA_TIMER   2000
/* 最大序号 (窗口大小, 序号空间 = MAX_SEQ + 1 = 8) */
#define MAX_SEQ      7
/* 发送窗口大小，GBN要求 WINDOW_SIZE <= MAX_SEQ，这里取等号达到最大吞吐 */
#define WINDOW_SIZE  MAX_SEQ

/*
 * 数据帧结构体
 *   总大小 = 3 + PKT_LEN = 259 字节 (不含CRC)
 *   padding字段用于内存对齐，无实际协议含义
 */
struct DATA_FRAME {
    unsigned char kind;          /* 帧类型: FRAME_DATA / FRAME_ACK / FRAME_NAK */
    unsigned char seq;           /* 本帧的发送序号 (0~7 模8循环) */
    unsigned char ack;           /* 捎带确认号: 接收方期望的下一个帧序号 */
    unsigned char data[PKT_LEN]; /* 上层数据载荷 (256字节) */
    unsigned int  padding;       /* 结构体填充，保证对齐 */
};

/*
 * ACK帧结构体
 *   总大小 = 2 字节 (不含CRC)
 */
struct ACK_FRAME {
    unsigned char kind;    /* 帧类型 */
    unsigned char ack;     /* 累积确认号 */
    unsigned int  padding; /* 结构体填充 */
};

/*
 * ============================================================
 *  发送方状态变量
 * ============================================================
 *  发送窗口 = [ack_expected, next_frame_to_send)，模8循环
 *  窗口大小 = nbuffered (已发未确认的帧数)
 *
 *  示意图 (WINDOW_SIZE = 7, 序号空间 = 0~7 模8):
 *    序号:   0     1     2     3     4     5     6     7
 *            [                                          )
 *            ^                                          ^
 *            ack_expected                               next_frame_to_send
 *            (窗口下界: 最早                              (窗口上界: 下一个
 *             未确认帧序号)                                待发送帧序号)
 *
 *    ←—— 发送窗口 = 7 帧 (nbuffered 最大为 7) ——→
 *    ack_expected 到 next_frame_to_send 之间的帧均已发送但未确认
 */
static unsigned char ack_expected = 0;       /* 发送窗口下界: 最早未确认帧的序号 */
static unsigned char next_frame_to_send = 0; /* 发送窗口上界: 下一个待发送帧的序号 */
static unsigned char frame_expected = 0;     /* 接收方期望的下一个帧序号 */
static unsigned char nbuffered = 0;          /* 已发送但未确认的帧数量 (窗口当前大小) */
static unsigned char out_buf[MAX_SEQ + 1][PKT_LEN]; /* 环形发送缓冲区，按序号索引 */
static int phl_ready = 0;                    /* 物理层就绪标志: 1=可发送, 0=忙 */

/*
 * last_frame_received - 计算捎带确认值
 *
 * 返回接收方已收到的最后一帧序号，即 (frame_expected - 1) mod 8
 * 等价于 (frame_expected + MAX_SEQ) % (MAX_SEQ + 1)
 *
 * 接收方只接受 seq==frame_expected 的帧，因此 frame_expected-1
 * 就是最后正确接收的帧。这个值填入每个发出的 DATA/ACK 帧的 ack 字段。
 */
static unsigned char last_frame_received(void)
{
    return (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
}

/*
 * inc - 序号递增 (模8循环)
 * *nr = (*nr + 1) % 8
 */
static void inc(unsigned char *nr)
{
    *nr = (*nr + 1) % (MAX_SEQ + 1);
}

/*
 * between - 判断序号b是否在环形区间(a, c)内
 *
 * 这是一个经典的模运算区间判断函数，考虑序号回绕的三种情况:
 *   Case 1: a <= b < c         正常递增区间，未回绕
 *   Case 2: c < a <= b         b在回绕点之后（区间跨越了模边界）
 *   Case 3: b < c < a          b在回绕点之前（区间跨越了模边界）
 *
 * 在GBN中用于累积确认: 判断收到的ACK是否落在待确认的窗口范围内，
 * 防止过期或重复的ACK错误地推进窗口。
 */
static int between(unsigned char a, unsigned char b, unsigned char c)
{
    return ((a <= b) && (b < c)) ||
           ((c < a) && (a <= b)) ||
           ((b < c) && (c < a));
}

/*
 * put_frame - 为帧附加CRC校验码并发送到物理层
 *
 * @frame: 待发送的帧数据
 * @len:   帧的有效长度 (不含CRC)
 *
 * CRC-32 被填充在帧末尾4字节 (frame + len 位置)，
 * 发送时总长度 = len + 4。
 * 发送后将 phl_ready 清零，防止过快发送导致物理层溢出。
 */
static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

/*
 * send_data_frame - 构造并发送一个数据帧
 *
 * @frame_nr: 本帧的发送序号
 *
 * 帧内容:
 *   kind = FRAME_DATA (数据帧类型)
 *   seq  = frame_nr    (本帧序号)
 *   ack  = last_frame_received()  (捎带确认，告知对方已正确接收的最后一帧)
 *   data = out_buf[frame_nr]      (从环形缓冲区取出对应序号的数据)
 *
 * 发送后立即启动该序号的定时器，超时后触发重传。
 */
static void send_data_frame(unsigned char frame_nr)
{
    struct DATA_FRAME s;

    s.kind = FRAME_DATA;
    s.seq = frame_nr;
    s.ack = last_frame_received();
    memcpy(s.data, out_buf[frame_nr], PKT_LEN);

    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(frame_nr, DATA_TIMER);
}

/*
 * send_ack_frame - 发送纯ACK确认帧
 *
 * 当收到对方数据帧后调用。ACK帧不含数据载荷，仅携带累积确认号。
 * ack = last_frame_received()，表示"我已正确收到此序号及之前所有帧"。
 */
static void send_ack_frame(void)
{
    struct ACK_FRAME s;

    s.kind = FRAME_ACK;
    s.ack = last_frame_received();

    dbg_frame("Send ACK  %d\n", s.ack);

    put_frame((unsigned char *)&s, 2);
}

/*
 * acknowledge_frames - 处理累积确认，滑动发送窗口
 *
 * @ack: 收到的累积确认号 (ACK=N 表示"序号 < N 的帧均已收到")
 *
 * 累积确认的核心语义: 收到 ack=3 意味着帧 0,1,2 都已被对方正确接收。
 * 此函数从 ack_expected 开始，逐一释放直到 ack，每释放一帧:
 *   - nbuffered--  (窗口缩小)
 *   - 停止该帧的定时器
 *   - ack_expected 前移 (窗口下界滑动)
 *
 * between() 校验确保只处理合法范围内的ACK，忽略重复/过期ACK。
 */
static void acknowledge_frames(unsigned char ack)
{
    while (nbuffered > 0 && between(ack_expected, ack, next_frame_to_send)) {
        nbuffered--;
        stop_timer(ack_expected);
        inc(&ack_expected);
    }
}

/*
 * retransmit_window - GBN核心: 超时后重传整个发送窗口
 *
 * 当定时器超时触发时调用。从 ack_expected 开始，
 * 重传窗口中所有 nbuffered 个未确认帧。
 * 这就是"Go-Back-N"名字的由来——回到最早的未确认帧，
 * 重传它及之后的所有帧 (共N帧)。
 *
 * 每帧重新发送时都会重新启动各自的定时器。
 */
static void retransmit_window(void)
{
    unsigned char frame_nr = ack_expected;
    unsigned char i;

    for (i = 0; i < nbuffered; i++) {
        send_data_frame(frame_nr);
        inc(&frame_nr);
    }
}

/*
 * main - 协议主循环 (事件驱动)
 *
 * 初始化后进入无限循环，等待框架投递事件:
 *
 *   NETWORK_LAYER_READY  : 网络层有数据要发送
 *     → 从上层取包存入缓冲区 → nbuffered++ → 发送数据帧 → next_frame_to_send++
 *
 *   PHYSICAL_LAYER_READY : 物理层空闲，可以发送
 *     → 设置 phl_ready = 1 (用于流量控制)
 *
 *   FRAME_RECEIVED       : 物理层收到帧
 *     → CRC校验 (校验失败则丢弃)
 *     → ACK帧: 调用 acknowledge_frames 滑动窗口
 *     → DATA帧: 若 seq==frame_expected 则上交网络层 (GBN拒绝乱序帧)
 *              处理捎带确认 → 发送ACK回复
 *
 *   DATA_TIMEOUT         : 某帧定时器超时
 *     → retransmit_window() 回退重传整个窗口
 *
 * 流量控制逻辑 (每次事件循环末尾):
 *   nbuffered < WINDOW_SIZE (窗口未满) 且 phl_ready (物理层就绪)
 *     → enable_network_layer()  通知上层可以继续提交数据
 *  否则
 *     → disable_network_layer() 阻止上层提交 (防止窗口溢出/物理层拥塞)
 */
int main(int argc, char **argv)
{
    int event, arg;
    unsigned char frame[sizeof(struct DATA_FRAME)];
    unsigned char kind, seq, ack;
    int len = 0;

    protocol_init(argc, argv);
    lprintf("Go-Back-N protocol, build: " __DATE__"  "__TIME__"\n");

    /* 初始关闭网络层，等物理层就绪且窗口有空位后才打开 */
    disable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);

        switch (event) {
        case NETWORK_LAYER_READY:
            /* 上层有数据要发送: 取出包，存入缓冲，发送数据帧 */
            get_packet(out_buf[next_frame_to_send]);
            nbuffered++;
            send_data_frame(next_frame_to_send);
            inc(&next_frame_to_send);
            break;

        case PHYSICAL_LAYER_READY:
            /* 物理层报告空闲，记录就绪状态 */
            phl_ready = 1;
            break;

        case FRAME_RECEIVED:
            /* 收到帧: 先做CRC校验，校验失败直接丢弃 */
            len = recv_frame(frame, sizeof frame);
            if (len < 5 || crc32(frame, len) != 0) {
                dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                break;
            }
            kind = frame[0];

            if (kind == FRAME_ACK) {
                /* 收到ACK帧: 提取确认号，滑动发送窗口 */
                ack = frame[1];
                dbg_frame("Recv ACK  %d\n", ack);
                acknowledge_frames(ack);
            }

            if (kind == FRAME_DATA) {
                /* 收到数据帧 */
                seq = frame[1];
                ack = frame[2];
                dbg_frame("Recv DATA %d %d, ID %d\n", seq, ack, *(short *)(frame + 3));

                /*
                 * GBN接收规则: 只有序号匹配期望值才接受
                 * seq == frame_expected → 按序到达，上交网络层，更新期望值
                 * seq != frame_expected → 乱序到达，静默丢弃 (不缓存)
                 */
                if (seq == frame_expected) {
                    put_packet(frame + 3, len - 7);
                    inc(&frame_expected);
                }

                /* 处理对方捎带的确认 → 滑动发送窗口 */
                acknowledge_frames(ack);
                /* 发送ACK确认 (纯ACK帧或捎带确认) */
                send_ack_frame();
            }
            break;

        case DATA_TIMEOUT:
            /* 定时器超时: 回退到最早未确认帧，重传整个窗口 */
            dbg_event("---- DATA %d timeout\n", arg);
            retransmit_window();
            break;
        }

        /*
         * 流量控制: 根据窗口状态和物理层状态控制网络层
         * 窗口未满 + 物理层空闲 → 允许上层继续提交数据
         * 窗口已满 或 物理层忙   → 阻止上层提交
         */
        if (nbuffered < WINDOW_SIZE && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
   }
}
