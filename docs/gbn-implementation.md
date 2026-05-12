# Go-Back-N 实现说明

本次实现将原始 `datalink.c` 的停止等待协议扩展为 Go-Back-N 滑动窗口协议，并同步到了以下三个目录：

- `Lab1-linux/datalink.c`
- `Lab1-Windows-VS2017/datalink.c`
- `Lab1-Windows-VS2013/datalink.c`

## 关键变量

- `MAX_SEQ`：最大序号，本实现使用 `0~7` 循环序号。
- `WINDOW_SIZE`：发送窗口大小，设置为 `MAX_SEQ`，也就是最多允许 7 个未确认数据帧。
- `ack_expected`：发送方当前等待确认的最早数据帧序号。
- `next_frame_to_send`：下一个要发送的数据帧序号。
- `frame_expected`：接收方当前期望收到的数据帧序号。
- `nbuffered`：发送窗口中尚未被确认的数据帧数量。
- `out_buf`：发送缓存，用来保存窗口内未确认的数据包，支持超时后重传。

## 发送逻辑

当收到 `NETWORK_LAYER_READY` 事件时，协议从网络层取出一个分组，放入 `out_buf[next_frame_to_send]`，然后封装为 DATA 帧发送。

每发送一个 DATA 帧：

1. 填入当前帧序号 `seq`。
2. 捎带最近已正确接收的数据帧确认号 `ack`。
3. 追加 CRC。
4. 启动该序号对应的数据帧计时器。
5. 推进 `next_frame_to_send`。

当 `nbuffered < WINDOW_SIZE` 且物理层可发送时，继续打开网络层；窗口满时关闭网络层，避免继续取包。

## 接收与确认逻辑

收到帧后先进行 CRC 校验，校验失败则丢弃。

如果收到 DATA 帧：

1. 若 `seq == frame_expected`，说明数据按序到达，调用 `put_packet()` 交付网络层。
2. 推进 `frame_expected`。
3. 发送 ACK，确认最近一个已按序接收的数据帧。
4. 同时处理 DATA 帧中捎带的累计 ACK。

如果收到 ACK 帧：

1. 读取 ACK 号。
2. 从 `ack_expected` 开始累计确认所有已经被覆盖的数据帧。
3. 停止已确认帧的计时器。
4. 滑动发送窗口。

## 超时重传逻辑

当收到 `DATA_TIMEOUT` 事件时，协议执行 Go-Back-N 重传：

1. 从当前 `ack_expected` 开始。
2. 按发送窗口中尚未确认的帧数量 `nbuffered` 逐个重发。
3. 每个重发的数据帧都会重新启动对应计时器。

这符合 Go-Back-N 的基本特征：只要窗口内某个未确认帧超时，就从最早未确认帧开始回退重传。

## 基础验证

已使用 Windows/MinGW 对 `Lab1-Windows-VS2017` 版本完成一次完整编译：

```bash
gcc -O2 datalink.c protocol.c lprintf.c crc32.c getopt.c -o datalink_gbn.exe -lm -lwsock32
```

并执行了 5 秒无误码双端运行测试：

```bash
datalink_gbn.exe -u -t 5 -p 59145 A
datalink_gbn.exe -u -t 5 -p 59145 B
```

测试结果：A、B 两端均正常退出，退出码均为 `0`。

随后执行了 5 秒满负载无误码测试：

```bash
datalink_gbn.exe -f -u -t 5 -p 59146 A
datalink_gbn.exe -f -u -t 5 -p 59146 B
```

测试结果：A、B 两端均正常退出，退出码均为 `0`。
