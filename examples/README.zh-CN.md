# ddhub 多平台示例

[English](README.md) | 简体中文

**ddhub = Data Dispatch Hub（数据分发中心）**。版本 **0.1.0**：**初版，首次发布**。

三个平台工程共用 `common/` 中的业务代码与示例配置，组件的默认配置不变：

```text
examples/
├── common/                 双 hub 配置、四个回调、静态消息及注册/发送流程
├── linux/                  pthread mutex 适配和控制台入口
├── windows/                Win32 SRWLOCK 适配和控制台入口
└── stm32_baremetal/         CMSIS 中断状态适配和裸机工程入口
```

| hub_id | 订阅事件 | 回调函数 | 消息来源 | 消息内容 |
| --- | --- | --- | --- | --- |
| `EXAMPLE_HUB_SENSOR` | `EVENT_TEMPERATURE`，bit 0 | `on_temperature` | `temperature_sensor` | `temperature=25 C` |
| `EXAMPLE_HUB_SENSOR` | `EVENT_HUMIDITY`，bit 1 | `on_humidity` | `humidity_sensor` | `humidity=60 percent` |
| `EXAMPLE_HUB_CONTROL` | `EVENT_COMMAND`，bit 2 | `on_command` | `operator` | `command=start` |
| `EXAMPLE_HUB_CONTROL` | `EVENT_ALARM`，bit 3 | `on_alarm` | `alarm_monitor` | `alarm=clear` |

平台绑定 lock/unlock 后调用一次 `ddhub_example_run()`：注册四个静态节点，发送四条静态消息，
先调用 `ddhub_process(EXAMPLE_HUB_SENSOR)`，再调用 `ddhub_process_all()` 处理另一个 hub，最后注销节点。
示例只在当前线程或主循环中执行，不额外创建线程；消息来源是业务发布者的标识。
数据与节点使用静态存储，不需要动态分配。平台互斥操作仍可保护后续的并发调用。

Windows 示例的预期输出如下，其他平台只替换方括号中的平台名称：

```text
[Windows] source=temperature_sensor message="temperature=25 C" hub=SENSOR callback=on_temperature event=0x00000001
[Windows] source=humidity_sensor message="humidity=60 percent" hub=SENSOR callback=on_humidity event=0x00000002
[Windows] source=operator message="command=start" hub=CONTROL callback=on_command event=0x00000004
[Windows] source=alarm_monitor message="alarm=clear" hub=CONTROL callback=on_alarm event=0x00000008
```

下面命令从 ddhub 仓库根目录执行。

## Linux

需要 C99 编译器、CMake、Ninja 和 pthread。使用静态 `PTHREAD_MUTEX_INITIALIZER`，锁失败时终止进程。

```sh
cmake -G Ninja -S examples/linux -B build/linux
cmake --build build/linux
./build/linux/ddhub_linux_example
```

## Windows

使用 Win32 `SRWLOCK_INIT` 和独占模式，不需要 pthread。锁适配按
[Microsoft SRWLOCK 接口](https://learn.microsoft.com/en-us/windows/win32/Sync/slim-reader-writer--srw--locks)实现。
推荐使用已加入 PATH 的 MinGW GCC 配合 Ninja：

```powershell
cmake -G Ninja -S examples/windows -B build/windows
cmake --build build/windows
& .\build\windows\ddhub_windows_example.exe
```

也可以使用 Visual Studio 2022 的 C 编译器：

```powershell
cmake -G "Visual Studio 17 2022" -A x64 -S examples/windows -B build/windows-msvc
cmake --build build/windows-msvc --config Debug
& .\build\windows-msvc\Debug\ddhub_windows_example.exe
```

Linux 和 Windows 的互斥适配都可能等待，面向普通线程，不用于中断上下文。

## STM32 裸机

入口为 `ddhub_stm32_example_run()`。默认使用 `stm32h723xx.h`，可通过
`DDHUB_STM32_DEVICE_HEADER` 改为实际芯片的 CMSIS 设备头文件。
适配器用 CMSIS 的 `__get_PRIMASK()`、`__disable_irq()`、`__set_PRIMASK()` 保存、屏蔽并恢复中断，
配合 `__DMB()` 保证共享状态的内存顺序。退出时恢复进入前的状态，不无条件开中断。
接口语义参考 [CMSIS Core Register Access](https://arm-software.github.io/CMSIS_6/main/Core/group__Core__Register__gr.html)
和 [CMSIS DMB](https://arm-software.github.io/CMSIS_5/Core/html/group__intrinsic__CPU__gr.html)。

此适配面向单个 Cortex-M 内核；NMI、HardFault 及其他内核不能并发访问这份 ddhub。
示例回调由主循环执行，printf 不在中断屏蔽区内运行。

将以下三个源码加入自己的 STM32 裸机工程：

- `src/ddhub.c`
- `examples/common/ddhub_example.c`
- `examples/stm32_baremetal/ddhub_stm32_example.c`

增加包含路径：`include/`、`examples/common/`、`examples/stm32_baremetal/`、CMSIS Core 及设备头文件目录。
三个源码统一定义 `DDHUB_CONFIG_HEADER="ddhub_example_config.h"`。板级工程提供启动文件、链接脚本、
系统时钟和 printf 串口重定向，在这些初始化完成后，从 `main()` 调用：

```c
#include "ddhub.h"
#include "ddhub_stm32_example.h"
#include <stdio.h>

/* Place after board initialization and printf retargeting in the existing main(). */
ddhub_status_t status = ddhub_stm32_example_run();
if (status != DDHUB_OK) {
    printf("STM32 ddhub example failed: status=%d\n", (int)status);
}
```

只调用一次入口；它会绑定平台锁并运行完整演示，不需要 RTOS，也不依赖 HAL 同步接口。
可以继续使用 CubeMX/Keil/GCC 工程原有的启动与 UART 配置。

提供的 CMake 工程可独立交叉编译示例静态库，或以 CMake target 方式链接进裸机应用。
例如使用 GCC ARM 工具链构建静态库：

```sh
cmake -G Ninja -S examples/stm32_baremetal -B build/stm32 \
  -DCMAKE_SYSTEM_NAME=Generic \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER=arm-none-eabi-gcc \
  -DCMAKE_C_FLAGS="-mcpu=cortex-m7 -mthumb" \
  -DDDHUB_STM32_CMSIS_INCLUDE_DIRS="/path/to/CMSIS/Core/Include;/path/to/STM32H7xx/Include"
cmake --build build/stm32
```

上例中的 `DDHUB_STM32_CMSIS_INCLUDE_DIRS` 应填写真实目录。输出为平台示例库和共用业务/组件库；
链接最终固件时使用 `ddhub_stm32_example` target（会传递共用库依赖），或同时链接这两个静态库。
该工程不生成独立的板级固件，板级启动和 printf 输出由现有裸机工程负责。
