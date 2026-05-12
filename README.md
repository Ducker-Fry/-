# 计算机网络实验一：数据链路层协议

本仓库用于完成计算机网络实验一。实验基于课程提供的数据链路层仿真环境，在给定的 `datalink.c` 框架上实现可靠传输协议，并通过日志与性能数据验证协议在正常、满负载和误码信道下的行为。

## 任务分工入口

小组三人的高效推进分工、每个人要做什么、最终要交付什么，都写在 [docs/efficient-division.md](docs/efficient-division.md)。

开始实验前请先阅读该文档，再按各自角色推进代码实现、测试验证和报告整理。

## 实验目标

1. 理解数据链路层在不可靠信道上提供可靠传输的基本方法。
2. 掌握帧格式设计、CRC 校验、确认帧、序号、超时重传和滑动窗口机制。
3. 在课程仿真环境中实现 Go-Back-N 协议，并分析吞吐率、误码率和重传行为。
4. 形成可复现的代码、测试记录和实验报告，方便三名成员并行推进。

## 实验内容概述

课程环境模拟 A、B 两个站点之间的全双工链路。物理信道参数默认为：

- 带宽：`8000 bps`
- 单向传播时延：`270 ms`
- 默认误码率：`1.0E-5`
- 网络层分组长度：`256 bytes`

程序通过 `protocol.h` 暴露网络层、物理层、事件、计时器和调试日志接口。实验的核心工作是在 `datalink.c` 中完成数据链路层协议逻辑：

- 从网络层取包：`get_packet()`
- 向物理层发送帧：`send_frame()`
- 从物理层接收帧：`recv_frame()`
- 正确交付数据：`put_packet()`
- 使用 `crc32()` 检测帧损坏
- 使用 `start_timer()` / `stop_timer()` 管理数据帧超时
- 使用 ACK 和序号控制发送窗口与接收确认

## 实验逻辑

实验代码按事件驱动方式运行。主循环持续调用 `wait_for_event()`，根据事件类型推进协议状态：

1. `NETWORK_LAYER_READY`：网络层有新分组可取，发送方在窗口未满时取包、封装 DATA 帧并发送。
2. `PHYSICAL_LAYER_READY`：物理层发送队列可继续接收帧，协议可恢复发送。
3. `FRAME_RECEIVED`：收到帧后先做 CRC 校验，再根据 DATA/ACK/NAK 类型更新接收窗口、交付数据或滑动发送窗口。
4. `DATA_TIMEOUT`：指定序号的数据帧超时，Go-Back-N 需要从最早未确认帧开始重传。
5. `ACK_TIMEOUT`：可用于延迟 ACK 或捎带确认策略。

当前提供的 `datalink.c` 是停止等待协议风格的基础实现，窗口大小为 1。后续实现 Go-Back-N 时，需要扩展发送缓存、发送窗口、累计确认和批量重传逻辑。

## 项目框架

```text
.
├── Lab1-linux/                 # Linux/GCC 版本实验框架，推荐作为主要开发目录
│   ├── datalink.c              # 数据链路层协议实现入口
│   ├── datalink.h              # 帧类型与帧格式说明
│   ├── protocol.h              # 课程仿真环境 API
│   ├── protocol.c              # 信道、事件、计时器和网络层仿真
│   ├── crc32.c                 # CRC-32 校验实现
│   ├── lprintf.c/.h            # 带时间戳的日志输出
│   └── Makefile                # Linux 构建脚本
├── Lab1-Windows-VS2017/        # Windows/Visual Studio 2017 工程
├── Lab1-Windows-VS2013/        # Windows/Visual Studio 2013 工程
├── docs/                       # 实验设计、协作计划和问题记录
├── reports/                    # 实验报告、截图、性能测试记录
├── logs/                       # 运行日志归档，生成的 .log 默认不入库
├── tests/                      # 测试说明、命令记录和复现实验脚本
├── rfc1662.txt                 # PPP/CRC 相关参考资料
└── 计算机网络实验一.pdf          # 实验指导书
```

## 三人分工

> 将“成员A/成员B/成员C”替换为小组真实姓名即可。分工按模块拆开，三个人可以同时推进，最后在 `datalink.c` 汇合。

| 成员 | 主要职责 | 可并行产出 | 交付物 |
| --- | --- | --- | --- |
| 成员A | 协议核心实现 | 设计 Go-Back-N 帧结构、发送窗口、缓存数组、累计 ACK 处理和超时重传逻辑 | `Lab1-linux/datalink.c` 中的协议实现；必要时同步 Windows 版本 |
| 成员B | 测试与性能验证 | 设计无误码、默认误码、`-f` flood、`-u` utopia、`-b 1e-4` 等场景，收集吞吐率和错误率 | `tests/` 测试记录、`reports/` 性能表、关键日志摘要 |
| 成员C | 文档与报告整合 | 整理实验目的、原理、流程图、关键代码说明、问题分析和截图 | `docs/` 设计文档、最终实验报告和答辩材料 |

协作建议：

- 成员A 优先在 `Lab1-linux/datalink.c` 开发，保持函数边界清晰。
- 成员B 先基于当前停止等待版本建立测试基线，再用同一套命令验证 Go-Back-N 版本。
- 成员C 在实现过程中同步记录协议状态机、帧格式、窗口滑动条件和测试结论，避免最后补报告时信息丢失。
- 修改同一文件前先沟通，跨平台同步时以 Linux 版本为主，Windows 版本只做必要移植。

## 构建与运行

Linux 版本：

```bash
cd Lab1-linux
make
./datalink -d3 A
./datalink -d3 B
```

常用实验命令：

```bash
# 理想无误码信道
./datalink -u A
./datalink -u B

# flood 模式，观察满负载吞吐
./datalink -f A
./datalink -f B

# 指定误码率
./datalink -f -b 1e-4 A
./datalink -f -b 1e-4 B
```

Windows 版本可打开 `Lab1-Windows-VS2017/datalink.sln` 或 `Lab1-Windows-VS2013/datalink.sln` 编译运行。

## 后续实现检查清单

- [ ] 设计 DATA/ACK 帧结构，确认 CRC 覆盖范围。
- [ ] 扩展发送缓存和窗口变量，支持多个未确认帧。
- [ ] 实现累计 ACK，收到 ACK 后滑动发送窗口。
- [ ] 实现 Go-Back-N 超时重传，从最早未确认帧开始重发。
- [ ] 控制 `enable_network_layer()` / `disable_network_layer()`，保证窗口满时暂停取包。
- [ ] 验证无误码、默认误码、较高误码和 flood 场景。
- [ ] 记录吞吐率、误码率、重传次数和典型日志片段。
