# i_CAN_hack — STM32 CAN Bus Logger & Injector

A bare-metal CAN bus logging and injection tool built on the **STM32F446RE** (Nucleo-64). Designed for sniffing, filtering, benchmarking, and injecting CAN frames — all controlled through an interactive UART console.

## Features

- **Interrupt-driven CAN RX** — frames are captured in the `CAN RX FIFO0` ISR and stored in a 32-slot circular buffer with zero polling overhead.
- **Interactive UART console** — menu-driven interface over USART2 (115200 8N1) with a non-blocking, event-driven state machine.
- **5 operating modes:**

| Mode | Key | Description |
|------|-----|-------------|
| Read All | `1` | Log and display every CAN frame on the bus |
| Read Filtered | `2` | Prompt for a hex CAN ID, then display only matching frames |
| Speed Test | `3` | Count and report frames-per-second with CAN ESR diagnostics |
| Idle | `4` | Suspend all capture activity |
| Write Frame | `5` | Transmit an arbitrary CAN frame (`ID DLC DATA...`) |

- **Overflow detection** — dropped frames are counted and reported as warnings on UART.
- **LED heartbeat** — PA5 toggles on every CAN RX and UART RX interrupt for visual activity feedback.
- Press **`M`** at any time to return to the main menu.

## Hardware Requirements

- **MCU:** STM32F446RE (Nucleo-64 board)
- **CAN transceiver:** External module (e.g. TJA1050 or MCP2551) — the MCU does not have a built-in PHY
- **UART:** USB-UART via the Nucleo's built-in ST-Link VCP on USART2 (PA2/PA3)
- **Bus termination:** 120 Ω resistors on both ends of the CAN bus

### Pin Mapping

| Function | Pin | Peripheral |
|----------|-----|------------|
| CAN TX | PA12 | CAN1 |
| CAN RX | PA11 | CAN1 |
| UART TX | PA2 | USART2 |
| UART RX | PA3 | USART2 |
| LED | PA5 | GPIO Output |

## CAN Bus Configuration

The CAN peripheral is configured for **500 kbps** on a 42 MHz APB1 clock:

```
Prescaler = 6
TimeSeg1 = 11 TQ (CAN_BS1_11TQ)
TimeSeg2 = 2 TQ  (CAN_BS2_2TQ)
Bit Time = 1 + 11 + 2 = 14 TQ
Baud = 42 MHz / (6 × 14) = 500 kbps
```

- **Mode:** Normal (CAN_MODE_NORMAL)
- **Auto Bus-Off Recovery:** Enabled
- **Auto Retransmission:** Enabled
- **Hardware Filter:** Bank 0 in ID Mask mode — accepts all IDs (mask = 0x0000). Software filtering is applied at the application layer.

## Project Structure

```
i_CAN_hack/
├── Core/
│   ├── App/
│   │   ├── Inc/
│   │   │   └── app.h            # Public API, enums (LoggerMode_t, UART_State_t), CAN_LogFrame_t
│   │   └── Src/
│   │       └── app.c            # Application logic: state machines, CAN RX/TX, UART handler
│   ├── Inc/
│   │   └── main.h               # HAL config, project-wide includes
│   └── Src/
│       ├── main.c               # MCU init, peripheral config, entry point → app_main()
│       ├── stm32f4xx_it.c       # IRQ handlers (CAN1_RX0, USART2, TIM6)
│       ├── stm32f4xx_hal_msp.c  # HAL MSP init (GPIO AF, clocks for CAN/UART)
│       └── system_stm32f4xx.c   # CMSIS system init
├── Drivers/                     # STM32 HAL & CMSIS (vendor code)
├── STM32F446RETX_FLASH.ld       # Linker script (flash)
├── STM32F446RETX_RAM.ld         # Linker script (RAM debug)
└── README.md
```

## Architecture

The application runs a **dual state machine** in `app.c`:

```
┌──────────────────────────────────────────────────────┐
│                    ISR Context                        │
│                                                      │
│  CAN RX FIFO0 ISR ──► Ring buffer (logBuffer[32])    │
│  UART RX ISR ──────► Sets uartEvent flag             │
└──────────────────────────────────────────────────────┘
              │                       │
              ▼                       ▼
┌──────────────────────────────────────────────────────┐
│                  Main Loop (app_main)                 │
│                                                      │
│  HandleUartEvent() ──► UART State Machine            │
│    UART_STATE_MENU ──► parse menu choice             │
│    UART_STATE_WAIT_FILTER_ID ──► parse hex ID        │
│    UART_STATE_WAIT_TX_FRAME ──► parse TX command     │
│                                                      │
│  LoggerMode switch ──► Logger State Machine           │
│    MODE_READ_ALL / MODE_READ_FILTERED ──► drain buf  │
│    MODE_SPEEDTEST ──► print frames/sec + ESR         │
│    MODE_WRITE ──► transition to IDLE after TX        │
│    MODE_IDLE ──► no-op                               │
└──────────────────────────────────────────────────────┘
```

## Quick Start

### Build & Flash
1. Open the project in **STM32CubeIDE**
2. Build (`Ctrl+B`)
3. Flash via ST-Link (`F11` to debug, or `Run → Run`)

### UART Console
Connect a terminal (PuTTY, minicom, etc.) to the Nucleo VCP at **115200 8N1**:

```
UART OK
Entered app_main

==== CAN LOGGER MENU ====
1 -> Read All Frames
2 -> Read Filtered Frames
3 -> Speed Test
4 -> Idle
5 -> Write CAN Frame
Enter option:
```

#### Example: Sniff all traffic
```
> 1
Mode: READ ALL
T:12345 ID:0x65D DLC:3 DATA: AA BB CC
T:12350 ID:0x123 DLC:2 DATA: 01 02
```

#### Example: Inject a frame
```
> 5
Enter: ID DLC DATA...
65D 3 AA BB CC
Frame Sent
```

#### Example: Speed test
```
> 3
Mode: SPEED TEST
F/s:427 ESR:0x00000000 (REC=0 TEC=0 LEC=0 BOFF=0) BTR:0x001C0005
```

## Configuration

| Parameter | Location | Default | Notes |
|-----------|----------|---------|-------|
| `LOG_BUFFER_SIZE` | `app.c:16` | 32 | Ring buffer depth — increase for high-rate capture |
| CAN Baud Rate | `main.c:193` | 500 kbps | Adjust `Prescaler` for different rates |
| UART Baud Rate | `main.c:218` | 115200 | Standard VCP speed |
| HW Filter | `app.c:512` | Accept all | Mask = 0x0000, passes all IDs to software filter |

## Troubleshooting

| Symptom | Check |
|---------|-------|
| No UART output | Verify VCP driver installed, terminal at 115200 8N1 |
| No CAN frames | External transceiver wired? Bus terminated (120 Ω)? CAN_H/CAN_L not swapped? |
| `WARNING: LOG OVERFLOW` | Bus rate exceeds drain speed — increase `LOG_BUFFER_SIZE` or reduce UART print overhead |
| `TX Error` | No other node on bus? Transceiver not powered? Check ESR via speed test mode |
| MCU hangs on boot | `Error_Handler()` traps — attach debugger, check peripheral init return codes |

## Current Status

**✅ Working:**
- CAN frame reception (interrupt-driven) and ring buffer logging
- All 5 operating modes functional (Read All, CAN TX — arbitrary frame injection from the UART console
Filtered, Speed Test, Idle, Write)
- UART console with event-driven, non-blocking input parsing
- CAN TX — arbitrary frame injection from the UART console
- Software ID filtering with runtime configuration
- Overflow detection and UART warning reporting
- LED activity indicator on PA5

**⚠️ Known Limitations:**
- Ring buffer is fixed at 32 frames — can overflow under sustained high bus loads
- UART TX uses blocking `HAL_UART_Transmit()` with `HAL_MAX_DELAY`, which stalls the main loop during prints
- Software-only ID filtering — hardware filter bank 0 is set to accept-all, adding CPU overhead on busy buses
- Only supports **standard (11-bit) CAN IDs** for TX; RX handles both standard and extended but filtering is standard-only
- Single filter ID at a time — no multi-ID or range-based filtering
- Timestamps are millisecond resolution (`HAL_GetTick`), insufficient for precise inter-frame timing analysis
- No persistent logging — all data is lost on power cycle

## Future Work

- [ ] **DMA UART TX** — replace blocking transmits with DMA to eliminate main loop stalls
- [ ] **Larger / dynamic ring buffer** — increase `LOG_BUFFER_SIZE` or allocate from heap for high-throughput capture
- [ ] **Hardware CAN filters** — configure multiple filter banks for multi-ID accept lists, reducing ISR load
- [ ] **Extended ID (29-bit) support** — allow filtering and TX with extended CAN IDs
- [ ] **Multi-ID filter list** — support filtering on multiple IDs simultaneously (whitelist/blacklist)
- [ ] **SD card logging** — persistent frame capture to FAT32 via SPI/SDIO for post-mortem analysis
- [ ] **Binary export protocol** — SLIP or COBS framed binary output for high-speed host-side capture tools
- [ ] **High-resolution timestamps** — use a dedicated hardware timer (TIM2/TIM5 at µs resolution) instead of SysTick
- [ ] **Bus load & error telemetry** — continuously report bus load percentage, error counters (TEC/REC), and bus-off events
- [ ] **CAN FD support** — port to an STM32 with FDCAN peripheral for 64-byte payloads and flexible data rates
- [ ] **Replay / scripted injection** — store and replay captured frame sequences for automated test scenarios
- [ ] **PC companion tool** — Python/CLI utility for real-time plotting, DBC decoding, and log export (CSV, ASC, BLF)

## License

See repository root for license details. Contributions welcome.
