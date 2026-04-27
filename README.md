# i_CAN_hack — Embedded CAN Logging Platform

i_CAN_hack is a robust, field-ready CAN logging platform for STM32F4 microcontrollers. It is designed for
reliable message capture, deterministic throughput measurement, and controlled frame injection. The codebase
is intentionally compact and easy to extend for integration in test benches, verification rigs, or deployed
diagnostic appliances.

Overview
--------
This project provides:
- Interrupt-driven CAN reception into a preallocated circular buffer for deterministic capture.
- An interactive UART operator console on `USART2` (115200) for mode control, diagnostics and data export.
- Modes for full capture, software-filtered capture by CAN ID, frames-per-second measurement, idle, and
  manual frame transmission.

Key features
------------
- Deterministic, low-latency frame capture (CAN RX via `HAL_CAN_RxFifo0MsgPendingCallback`).
- Ring buffer storage (`LOG_BUFFER_SIZE = 32`) for bounded memory usage and predictable behavior.
- Non-blocking, interrupt-driven UART command parsing for operator control (`HAL_UART_RxCpltCallback`).
- Simple, extensible C code based on STM32 HAL suitable for production hardening.

Quick start
-----------
Requirements
- STM32CubeIDE (recommended) or GNU ARM toolchain (`arm-none-eabi-*`).
- ST-Link or compatible debug probe for flashing.
- External CAN transceiver (e.g., TJA1050, MCP2551) connected to MCU CAN pins.

Hardware wiring
- Connect USB-UART adapter to the board's `USART2` TX/RX to use the operator console (115200 8N1).
- Connect CAN_H and CAN_L to an external transceiver and ensure the bus is terminated (120Ω) on both ends.



Usage
-----
After boot the UART console prints the menu. Use a terminal (115200 8N1) to interact.

Menu options
- `1` — Read All Frames: prints and drains logged frames.
- `2` — Read Filtered Frames: prompts for a HEX CAN ID (e.g. `65D`), then stores only matching frames.
- `3` — Speed Test: measures incoming frames per second and prints the value once per second.
- `4` — Idle: stop capture and enter low-activity state.
- `5` — Write CAN Frame: prompts for `ID DLC DATA...` (e.g. `65D 3 AA BB CC`) and transmits the frame.

Example UART session

```
==== CAN LOGGER MENU ==== 
1 -> Read All Frames
2 -> Read Filtered Frames
3 -> Speed Test
4 -> Idle
5 -> Write CAN Frame
Enter option:

> 1
T:12345 ID:0x65D DLC:3 DATA: AA BB CC

> 5
Enter: ID DLC DATA...
65D 2 11 22
Frame Sent
```

File map
--------
- [Core/App/Inc/app.h](Core/App/Inc/app.h) — public API, mode enums, and `CAN_LogFrame_t` type.
- [Core/App/Src/app.c](Core/App/Src/app.c) — application state machine, CAN RX callback, UART handler, TX.
- [Core/Inc/main.h](Core/Inc/main.h) — project-wide definitions.
- [Core/Src/main.c](Core/Src/main.c) — MCU and peripheral initialization, `app_main()` entry.

Configuration highlights
------------------------
- `LOG_BUFFER_SIZE` (in `app.c`): size of the in-memory ring buffer. Increase for greater retention.
- UART: `USART2` configured to 115200. Change in `MX_USART2_UART_Init()` if needed.
- CAN timing: configured in `MX_CAN1_Init()`; tune `Prescaler`, `TimeSeg1`, `TimeSeg2` for target baud rate.

Operational considerations
-------------------------
- Buffer overflow: if frame arrival rate exceeds processing, new frames are dropped and `logOverflowCount`
  increments. The code reports overflow warnings to UART — monitor this during high-rate tests.
- Filtering: current implementation uses software filtering for runtime flexibility. For production systems
  with sustained high throughput, configure hardware acceptance filters via `HAL_CAN_ConfigFilter()` to reduce
  CPU overhead and dropped frames.
- Timing: timestamps use `HAL_GetTick()` (ms resolution). For sub-ms accuracy integrate a high-resolution timer
  or RTC.

Troubleshooting
---------------
- No UART output: verify `USART2` TX/RX wiring and terminal settings (115200 8N1). Check `MX_USART2_UART_Init()`.
- No CAN frames: confirm external transceiver is present and bus is correctly terminated. Check `CAN1` pins and
  `MX_CAN1_Init()` return status.
- Initialization failure: `Error_Handler()` halts the MCU on peripheral init errors; attach a debugger to inspect
  peripheral state.

Extending the project
----------------------
- Persistent logging: add SD card or external flash support (look at existing `sd_logger.c` as a starting point).
- Streaming: implement DMA-driven UART transmit or packetized binary export to avoid blocking the MCU.
- Advanced telemetry: add bus error counters, bus load measurement, and automated test sequences.

Contributing & License
----------------------
See the repository root for license details. Contributions are welcome — open issues and PRs with clear scope
and test instructions.



