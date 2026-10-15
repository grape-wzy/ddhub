# ddhub - Data Dispatch Hub

English | [Simplified Chinese](README.zh-CN.md)

Version: **0.1.0**. Release notes: **Initial version; first release**.

**ddhub** stands for **Data Dispatch Hub**, a data distribution center: **Data** is the payload,
**Dispatch** routes messages to interested callbacks, and **Hub** is the central coordination point.
It is a C99 component with static storage, intrusive subscription nodes, and event-bitmask dispatch.

- [User guide](docs/usage.md): APIs, platform lock adapters, concurrency rules, and payload lifetime.
- [Platform examples](examples/README.md): Linux, Windows, and STM32 bare-metal projects sharing two hubs and four callbacks.
- `tests/`: automated component tests.
- `include/` and `src/`: public headers, application configuration, and implementation.

## Features

- Each hub has an independent static FIFO and application-owned subscription nodes.
- Subscriptions use 32-bit event masks; each matching subscription invokes its callback once with the event intersection.
- Lower numerical priorities run first, with registration order preserved for equal priorities.
- `send()` queues a borrowed payload; `process()` dispatches it and a completion callback signals when it can be reused.
- Different hubs can process concurrently; `process_all()` processes available hubs and skips busy ones.
- Platform lock adapters support Linux, Windows, and STM32 bare-metal integration; C++ callers can use the C API.

## Build and use

Build the standalone static library from the ddhub repository root:

```sh
cmake -G Ninja -S . -B build/library
cmake --build build/library
```

Other CMake projects can use `add_subdirectory(path/to/ddhub)` and link their application target to `ddhub`.
Bind platform lock/unlock callbacks once before any other ddhub operation.
See [CMake integration](docs/usage.md#cmake-integration) for configuration and the
[platform examples](examples/README.md) for complete demonstrations.
