# ddhub - Data Dispatch Hub

[English](README.md) | 简体中文

版本：**0.1.0**，说明：**初版，首次发布**。

ddhub 是 **Data Dispatch Hub** 的缩写，中文为**数据分发中心**：Data 表示数据，Dispatch 表示分发，Hub 表示中心。

静态内存、侵入式订阅链表、按事件位图分发的 C99 组件。

- [使用说明](docs/usage.zh-CN.md)：接口、平台锁移植、并发规则及数据生命周期。
- [平台示例](examples/README.zh-CN.md)：Linux、Windows、STM32 裸机工程，共用双 hub、四回调演示。
- `tests/`：行为测试、平台锁次数检查及多线程并发测试，可独立构建，也接入工程主机测试。
- `include/`、`src/`：公共头文件、用户配置及组件实现。

全部 C 源码和头文件（包括示例及测试）采用英文注释。所有说明文档分别提供英文和简体中文版本，
文件头统一标明 0.1.0 版本及首次发布说明。

独立运行组件测试（在 ddhub 仓库根目录执行）：

```sh
cmake -G Ninja -S tests -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

主机测试使用 pthread：Linux 可直接使用系统线程库，Windows 可使用带 pthread 的 MinGW 工具链。
Windows 原生示例独立使用 Win32 SRWLOCK，不依赖 pthread。

组件不使用堆内存，不调用标准原子接口或编译器原子接口。其他 ddhub 操作开始前，先一次性绑定平台 lock/unlock。
