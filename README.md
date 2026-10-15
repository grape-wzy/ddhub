# ddhub - Data Dispatch Hub

English | [Simplified Chinese](README.zh-CN.md)

Version: **0.1.0**. Release notes: **Initial version; first release**.

**ddhub** stands for **Data Dispatch Hub**, a data distribution center: **Data** is the payload,
**Dispatch** routes messages to interested callbacks, and **Hub** is the central coordination point.
It is a C99 component with static storage, intrusive subscription nodes, and event-bitmask dispatch.

- [User guide](docs/usage.md): APIs, platform lock adapters, concurrency rules, and payload lifetime.
- [Platform examples](examples/README.md): Linux, Windows, and STM32 bare-metal projects sharing two hubs and four callbacks.
- `tests/`: deterministic behavior tests, platform lock cost checks, and concurrent host tests.
- `include/` and `src/`: public headers, application configuration, and implementation.

All C source files and headers, including examples and tests, use English documentation.
Each explanatory document has an English version and a corresponding Simplified Chinese version.
The file headers identify version 0.1.0 and its initial-release status.

Run standalone component tests from the ddhub repository root:

```sh
cmake -G Ninja -S tests -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

Host tests use pthread: Linux can use its system thread library; Windows can use a MinGW toolchain with pthread support.
The native Windows example uses Win32 SRWLOCK and has no pthread dependency.

The component uses no heap allocation and calls no standard or compiler-specific atomic APIs.
Bind platform lock/unlock callbacks once before any other ddhub operation.
