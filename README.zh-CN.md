# ddhub - Data Dispatch Hub

[English](README.md) | 简体中文

版本：**0.2.0**，说明：**同步/异步分发、全局通知与动态订阅**。

ddhub 是 **Data Dispatch Hub** 的缩写，中文为**数据分发中心**：Data 表示数据，Dispatch 表示分发，Hub 表示中心。

静态内存、侵入式订阅链表、按事件位图分发的 C99 组件。

- [版本变更](CHANGELOG.md)：发布说明与接口迁移步骤。
- [使用说明](docs/usage.zh-CN.md)：接口、平台锁移植、并发规则及数据生命周期。
- [平台示例](examples/README.zh-CN.md)：Linux、Windows、STM32 裸机工程，共用双 hub、四回调演示。
- `tests/`：组件自动化测试。
- `include/`、`src/`：公共头文件、用户配置及组件实现。

## 功能特性

- 异步模式下每个 hub 拥有独立的静态 FIFO，两种模式的订阅节点都由业务提供。
- 使用 32 位事件掩码订阅，每条消息对每条匹配订阅调用一次回调，传入事件交集。
- 数值越小优先级越高，同优先级按注册顺序执行。
- `DDHUB_ASYNC_PROCESS_ENABLE` 选择同步 send 直接分发或异步入队；异步 `process()` 统一排空所有 hub。
- 异步模式全库单消费者，使用一个可选全局通知接入平台调度；同步模式不同 hub 可并行分发。
- 处理中允许动态注册和注销；新订阅从下一条消息生效，活动节点注销返回 BUSY。
- 平台锁适配支持 Linux、Windows 和 STM32 裸机接入，C++ 调用方可使用公共 C 接口。

## 构建与使用

独立构建静态库（在 ddhub 仓库根目录执行）：

```sh
cmake -G Ninja -S . -B build/library
cmake --build build/library
```

其他 CMake 工程可使用 `add_subdirectory(path/to/ddhub)` 引入，再将应用目标链接到 `ddhub`。
其他 ddhub 操作开始前，先一次性绑定平台 lock/unlock。
配置方式见[使用说明](docs/usage.zh-CN.md)，完整演示见[平台示例](examples/README.zh-CN.md)。
