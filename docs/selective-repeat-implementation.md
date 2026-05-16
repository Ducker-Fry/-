# Selective Repeat 实现说明

本分支新增选择重传协议实现，文件为：

- `Lab1-Windows-VS2017/datalink_selective.c`

该文件不覆盖现有 `datalink.c` 的 Go-Back-N 实现，测试同学可以把它单独编译为 `datalink_selective.exe`，再与 GBN 在同样信道条件下对比性能。

## 核心设计

- `MAX_SEQ = 7`：序号空间为 `0~7`。
- `NR_BUFS = 4`：发送/接收窗口大小为序号空间的一半，避免新旧帧序号混淆。
- `out_buf`：发送缓存，保存未确认数据帧。
- `in_buf`：接收缓存，保存已到达但暂时不能按序交付的数据帧。
- `arrived`：标记接收窗口内某个序号是否已经到达。
- `acked`：标记发送窗口内某个序号是否已经被确认。
- `ack_expected`：发送窗口左边界。
- `next_frame_to_send`：下一个发送序号。
- `frame_expected`：接收窗口左边界。
- `too_far`：接收窗口右边界外一位。

## 与 Go-Back-N 的区别

Go-Back-N 接收方只接受 `seq == frame_expected` 的数据帧，乱序帧会被丢弃；一旦超时，发送方从最早未确认帧开始重传整个窗口。

Selective Repeat 接收方会缓存窗口内的乱序帧，并对每个正确收到的数据帧单独 ACK；发送方收到 ACK 后只标记对应帧已确认，某一帧超时时只重传该帧。

## 编译命令

在 `Lab1-Windows-VS2017` 目录下执行：

```bash
gcc -O2 datalink_selective.c protocol.c lprintf.c crc32.c getopt.c -o datalink_selective.exe -lm -lwsock32
```

## 基础测试命令

无误码短测：

```bash
datalink_selective.exe -u -t 5 -p 59155 -l sr-A.log A
datalink_selective.exe -u -t 5 -p 59155 -l sr-B.log B
```

满负载无误码短测：

```bash
datalink_selective.exe -f -u -t 5 -p 59156 -l sr-flood-A.log A
datalink_selective.exe -f -u -t 5 -p 59156 -l sr-flood-B.log B
```

高误码性能测试：

```bash
datalink_selective.exe -f -b 1e-4 -t 30 -p 59157 -l sr-highber-A.log A
datalink_selective.exe -f -b 1e-4 -t 30 -p 59157 -l sr-highber-B.log B
```

## 测试同学的独立交付

测试同学可以独立得到：

- `datalink_selective.exe`
- `sr-*.log`
- 不同信道条件下的吞吐率、误码率、重传日志
- SR 与 GBN 的性能对比表

对比时使用同样参数分别运行 GBN 和 SR，例如都使用 `-f -b 1e-4 -t 30`，只改变可执行文件和日志名。这样可以清楚区分 GBN 的整窗重传和 SR 的单帧重传效果。

## 已完成的成员A基础验证

已在 `Lab1-Windows-VS2017` 目录下完成编译：

```bash
gcc -O2 datalink_selective.c protocol.c lprintf.c crc32.c getopt.c -o datalink_selective.exe -lm -lwsock32
```

已完成 5 秒无误码双端短测：

```bash
datalink_selective.exe -u -t 5 -p 59155 -l sr-A.log A
datalink_selective.exe -u -t 5 -p 59155 -l sr-B.log B
```

测试结果：A、B 两端均正常退出，退出码均为 `0`。

已完成 5 秒满负载无误码双端短测：

```bash
datalink_selective.exe -f -u -t 5 -p 59156 -l sr-flood-A.log A
datalink_selective.exe -f -u -t 5 -p 59156 -l sr-flood-B.log B
```

测试结果：A、B 两端均正常退出，退出码均为 `0`。
