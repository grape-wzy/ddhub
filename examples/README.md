# ddhub platform examples

English | [Simplified Chinese](README.zh-CN.md)

**ddhub = Data Dispatch Hub**, a data distribution center.
Version **0.2.0**: **Sync/async dispatch, global notification, and live subscriptions**.

The three platform projects share the business code and configuration in `common/` without changing
the component's default configuration:

```text
examples/
├── common/                 Two hub IDs, four callbacks, static messages, and dispatch flow
├── linux/                  pthread mutex adapter and console entry point
├── windows/                Win32 SRWLOCK adapter and console entry point
└── stm32_baremetal/         CMSIS interrupt-state adapter and board integration entry point
```

| hub_id | Subscribed event | Callback | Message source | Message content |
| --- | --- | --- | --- | --- |
| `EXAMPLE_HUB_SENSOR` | `EVENT_TEMPERATURE`, bit 0 | `on_temperature` | `temperature_sensor` | `temperature=25 C` |
| `EXAMPLE_HUB_SENSOR` | `EVENT_HUMIDITY`, bit 1 | `on_humidity` | `humidity_sensor` | `humidity=60 percent` |
| `EXAMPLE_HUB_CONTROL` | `EVENT_COMMAND`, bit 2 | `on_command` | `operator` | `command=start` |
| `EXAMPLE_HUB_CONTROL` | `EVENT_ALARM`, bit 3 | `on_alarm` | `alarm_monitor` | `alarm=clear` |

After binding the platform lock adapter, call `ddhub_example_run()` once. It registers four static nodes,
publishes four static messages, calls `ddhub_process()` once in asynchronous mode, and unregisters the nodes.
Synchronous mode dispatches each message directly in send and compiles out the process call.
The example uses polling on the calling thread/main loop and creates no worker threads.
Nodes and payloads use static storage. See the usage guide for global notification and RTOS binary-wakeup integration.

Set `-DDDHUB_ASYNC_PROCESS_ENABLE=0` in a separate example build directory to exercise synchronous mode;
the CMake definition propagates to both the component and its callers.

Expected output from the Windows example follows. Other platforms replace only the bracketed platform label:

```text
[Windows] source=temperature_sensor message="temperature=25 C" hub=SENSOR callback=on_temperature event=0x00000001
[Windows] source=humidity_sensor message="humidity=60 percent" hub=SENSOR callback=on_humidity event=0x00000002
[Windows] source=operator message="command=start" hub=CONTROL callback=on_command event=0x00000004
[Windows] source=alarm_monitor message="alarm=clear" hub=CONTROL callback=on_alarm event=0x00000008
```

Run the following commands from the ddhub repository root.

## Linux

Requirements: a C99 compiler, CMake, Ninja, and pthread. The adapter uses a static `PTHREAD_MUTEX_INITIALIZER`
and aborts the process if a mutex operation fails.

```sh
cmake -G Ninja -S examples/linux -B build/linux
cmake --build build/linux
./build/linux/ddhub_linux_example
```

## Windows

The adapter uses Win32 `SRWLOCK_INIT` in exclusive mode and does not require pthread.
See the [Microsoft SRWLOCK API](https://learn.microsoft.com/en-us/windows/win32/Sync/slim-reader-writer--srw--locks).
With MinGW GCC and Ninja available on PATH:

```powershell
cmake -G Ninja -S examples/windows -B build/windows
cmake --build build/windows
& .\build\windows\ddhub_windows_example.exe
```

Alternatively, use the Visual Studio 2022 C compiler:

```powershell
cmake -G "Visual Studio 17 2022" -A x64 -S examples/windows -B build/windows-msvc
cmake --build build/windows-msvc --config Debug
& .\build\windows-msvc\Debug\ddhub_windows_example.exe
```

Both the Linux and Windows adapters may wait for their lock. They target ordinary threads, not interrupt handlers.

## STM32 bare metal

The entry point is `ddhub_stm32_example_run()`. The default device header is `stm32h723xx.h`;
set `DDHUB_STM32_DEVICE_HEADER` to the CMSIS device header for your MCU if needed.
The adapter uses CMSIS `__get_PRIMASK()`, `__disable_irq()`, and `__set_PRIMASK()` to save, mask, and restore interrupts,
with `__DMB()` providing memory ordering. It restores the previous state rather than unconditionally enabling interrupts.
See [CMSIS Core Register Access](https://arm-software.github.io/CMSIS_6/main/Core/group__Core__Register__gr.html)
and [CMSIS DMB](https://arm-software.github.io/CMSIS_5/Core/html/group__intrinsic__CPU__gr.html).

This adapter protects one Cortex-M core. NMI, HardFault, and other cores must not access this ddhub instance concurrently.
Callbacks run from the main loop; printf executes outside the interrupt-masked region.

Add these three source files to the STM32 bare-metal project:

- `src/ddhub.c`
- `examples/common/ddhub_example.c`
- `examples/stm32_baremetal/ddhub_stm32_example.c`

Add include paths for `include/`, `examples/common/`, `examples/stm32_baremetal/`, CMSIS Core, and the device headers.
Define `DDHUB_CONFIG_HEADER="ddhub_example_config.h"` consistently for all three source files.
Board code supplies startup files, the linker script, clock initialization, and printf/UART retargeting.
After that initialization, call the example from the existing `main()`:

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

Call the entry point once. It binds the platform adapter and runs the complete demonstration without an RTOS
or HAL synchronization dependency. Keep the existing CubeMX, Keil, or GCC startup and UART configuration.

The supplied CMake project cross-compiles static example libraries and can also provide a target to a board application.
For example, with a GCC ARM toolchain:

```sh
cmake -G Ninja -S examples/stm32_baremetal -B build/stm32 \
  -DCMAKE_SYSTEM_NAME=Generic \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER=arm-none-eabi-gcc \
  -DCMAKE_C_FLAGS="-mcpu=cortex-m7 -mthumb" \
  -DDDHUB_STM32_CMSIS_INCLUDE_DIRS="/path/to/CMSIS/Core/Include;/path/to/STM32H7xx/Include"
cmake --build build/stm32
```

Replace `DDHUB_STM32_CMSIS_INCLUDE_DIRS` with real directories.
The build produces the platform example library and the shared business/component library.
Link the `ddhub_stm32_example` CMake target to propagate the shared dependency, or link both static libraries manually.
This project does not produce standalone board firmware; startup and printf output remain the board project's responsibility.
