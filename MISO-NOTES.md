# SW7456 SPI MISO Support

## Problem

Betaflight 4.5 reads the OSDM register (0x8C) at boot to detect a MAX7456. It expects `0x1B`.
Without a MISO response, BF gets `0x00` and aborts with `MAX7456_INIT_NOT_FOUND`.

BF also reads VM0 (0x80) every 1 second for stall detection. If the response doesn't match the
last written value (minus the RST bit), BF treats it as a stall and reinitializes constantly.

Original SW7456 was RX-only (SPI_DIRECTION_2LINES_RXONLY) — MISO was never driven.

## Fix: TX FIFO Pre-load + Underrun Retransmit

The STM32G0 SPI slave retransmits the last shifted-out byte when the TX FIFO is empty (underrun).
There is no UDR interrupt flag in SPI mode (I2S only), so underrun is silent and automatic.

### Boot sequence

1. `osd_spi_preload()` runs immediately after `HAL_SPI_Init()` inside `MX_SPI1_Init()`
2. Waits for NSS HIGH (PA4) — FC's CS pin floats low at reset, could cause spurious clocks
3. Enables SPE, fills all 4 TX FIFO slots with `0x1B` (OSDM default)
4. No matter which byte position BF captures during the OSDM read, it gets `0x1B`

### Runtime

- On VM0 write, the register handler sets `spi_tx_response = registers[VM0] & ~RST`
- Main loop polls for change → writes new value to TX DR → becomes the new underrun byte
- BF's 1-second VM0 stall check reads this value and sees a match

### Why this design (not per-register address decoding)

During rendering, the COMP1 ISR blocks all lower-priority processing for ~14-17us per scanline.
SPI inter-byte gap is ~1-5us. Per-register TX response is physically impossible during rendering
without introducing video jitter. Underrun retransmit is the only zero-jitter option.

Between scanlines, the main loop updates the underrun value — no interrupts, no CPU during rendering.

### Rendering impact

The MISO support introduces **zero additional rendering artifacts**:

- **SPI RX DMA** already existed before this change. DMA bus contention with the CPU during
  pixel rendering can extend some pixels (known issue, see README). This is unchanged.
- **SPI TX uses no DMA and no interrupts.** The underrun retransmit is handled entirely by
  the SPI peripheral hardware — no bus arbitration, no wait states, no CPU involvement.
- **Main loop DR writes** (`osd_spi_process()`) only execute between scanlines. The COMP1 ISR
  blocks all lower-priority code for the full duration of each scanline render. A DR write
  cannot collide with pixel output.
- **Hardware NSS (CS) with pull-up** (PA4, `SPI_NSS_HARD_INPUT`, `GPIO_PULLUP`): When the FC
  deasserts CS, MISO goes tri-state automatically. MISO is only driven during active SPI
  transactions. The pull-up prevents spurious SPI activity when CS floats during FC boot.

Net effect: identical rendering quality to the original RX-only configuration.

## Files Modified

### `Core/Src/main.c`
- SPI direction: `SPI_DIRECTION_2LINES` (was `_RXONLY`) — enables MISO output from TX FIFO
- Added `#include "osd_spi.h"` in USER CODE Includes
- Added `osd_spi_preload()` call in USER CODE SPI1_Init 2 (survives CubeMX regeneration)

### `osd/osd_spi.c`
- Split into two functions:
  - `osd_spi_preload()` — NSS wait + 4x0x1B FIFO fill (called from main.c, before anything else)
  - `osd_spi_start()` — DMA setup for RX processing (called from osd_init, after IRQ/gen init)
- `osd_spi_process()` — added TX DR update when `spi_tx_response` changes (TXE-gated)

### `osd/osd_spi.h`
- Added `osd_spi_preload()` and `spi_data[]` declarations
- Added `extern U8 spi_tx_response`

### `osd/osd_registers.h`
- Added `R_7456_OSDM_DEFAULT 0x1B`

### `osd/osd_registers.c`
- **State 0**: Skip `0xFF` bytes — BF's `spiWrite(0xFF)` flush sends a single 0xFF on MOSI.
  Without the skip, 0xFF was treated as register address 0x1F (RB15), desynchronizing the
  state machine.
- **State 2** (read-addressed bytes): Always return to state 0. Previously had a conditional
  check (`if (data==0x00)`) that could leave the state machine stuck if BF sent non-zero
  during the data phase of a read.
- **VM0 handler**: Sets `spi_tx_response = registers[REG_7456_VM0] & ~R_7456_VM0_RST` after
  every VM0 write. After a software reset, `write_register_defaults()` clears VM0 to 0x00,
  so `spi_tx_response = 0x00` (BF expects this — RST bit self-clears).

### Files NOT modified
- `stm32g0xx_hal_msp.c` — PA6 (MISO) already configured as AF_PP
- `osd_irq.c` / `osd_asm.s` — zero rendering changes

## Betaflight Configuration

### Required CLI settings

```
set vcd_video_system = PAL    # or NTSC — must match your camera
set osd_displayport_device = AUTO
save
```

### Why explicit video standard (not AUTO)

SW7456 doesn't currently implement the STAT register (0xA0) readback. When BF reads STAT for
PAL/NTSC auto-detection, it gets the VM0 shadow value instead of actual format bits. With
`vcd_video_system = AUTO`, BF may misidentify the format or fail detection.

Setting PAL or NTSC explicitly tells BF to skip STAT reads during reinit.

Note: SW7456 already detects the video standard internally (see Future Work below), so STAT
readback could be implemented in a future version, which would allow `AUTO` to work.

### Why `osd_displayport_device = AUTO`

Ensures BF uses MAX7456 mode (the real SPI OSD path) instead of DisplayPort serial protocol.
`AUTO` is the default — only needs explicit setting if previously changed.

## Known Limitations

### 1. Betaflight only (SPI Mode 3)

SW7456 is hardcoded to SPI Mode 3 (CPOL=1, CPHA=1), matching Betaflight. INAV uses Mode 0
(CPOL=0, CPHA=0) for most targets. The MISO mechanism works identically in both modes — only
the CPOL/CPHA configuration needs changing. See Future Work below.

### 2. All register reads return the same value

Every read returns `spi_tx_response` (VM0 shadow at runtime, or 0x1B at boot). Verified safe
against a BF 4.5 source audit — BF only reads VM0 at runtime (stall check). Would break if
future BF versions add reads where the VM0 shadow bit pattern is harmful.

### 3. HAL_SPI_Receive_DMA does not flush TX FIFO

Verified by reading the STM32G0 HAL source: for a slave in `SPI_DIRECTION_2LINES` mode,
`HAL_SPI_Receive_DMA()` does not toggle SPE, does not flush FIFOs, does not set RXONLY.
It only configures the RX DMA channel. The pre-loaded FIFO bytes survive the call.

This is a critical assumption of the design — if ST changes the HAL behavior in a future
version, the FIFO pre-load could be lost.

## Future Work

### STAT register readback (PAL/NTSC auto-detection)

SW7456 already detects the video standard internally. In `osd_irq.c`, the ISR counts scanlines
per field and distinguishes PAL (305 lines) from NTSC (253/254 lines) via
`stats.field1_line_count`. This drives `end_line` selection (PAL_LINES vs NTSC_LINES) and is
already used for rendering — the detection is proven and reliable.

The challenge is exposing this through the STAT register read. The current underrun design
returns the same value for ALL register reads. Per-register address decoding would require
CPU response within the ~1-5us SPI inter-byte gap, which collides with the COMP1 scanline
ISR during rendering.

Possible approaches:
- **Main loop STAT update**: The state machine already sees the read address (0xA0) on MOSI.
  It could set `spi_tx_response` to the STAT value when it sees a STAT read address, then
  restore VM0 shadow afterward. This only works if the main loop processes the address byte
  before the SPI peripheral shifts out the data byte — timing-dependent, unreliable during
  rendering, but may work during BF's reinit (when rendering is idle).
- **Dual-value approach**: Encode a combined VM0+STAT value in `spi_tx_response` where the
  bit patterns don't conflict. Requires careful analysis of which bits BF checks in each
  register. VM0 stall check compares the full byte against `videoSignalReg`, so there may
  be no safe bits to borrow.
- **TX interrupt**: Use the SPI TX interrupt to respond per-register. Would cause ~1 pixel
  jitter on one scanline per read (once per second for VM0 stall check). Arguably acceptable
  for the benefit of full register readback.

### INAV support (SPI Mode 0)

INAV uses SPI Mode 0 (CPOL=0, CPHA=0) on most targets (STM32F4/F7/H7). The MISO response
mechanism is SPI-mode-agnostic — only the clock polarity and phase configuration differs.

Possible approaches:
- **Compile-time `#define`**: Simple, zero-overhead. User selects BF or INAV at build time.
- **SCK idle detection**: Read PA5 (SCK) IDR state before enabling SPI. Mode 3 idles HIGH,
  Mode 0 idles LOW. Requires a delay after boot for the FC to configure its SPI clock output.
  Auto-detection without user configuration.

INAV's MAX7456 driver also reads OSDM and VM0, so the existing MISO support should work once
the SPI mode is correct. An INAV source audit would confirm no additional register reads are
required.

## Hardware Notes

The reference schematic NC/NO terminal labels may not match all analog switch IC conventions.
Verify against your specific part's datasheet. The firmware logic (PB0 HIGH = OSD active,
PB0 LOW = video passthrough) is correct for both wiring variants.

## Tested With

- **Board**: STM32G071 (SW7456-R2), March 2026
- **Flight controller firmware**: Betaflight 4.5-maintenance
- **BF CLI verification**: `status` shows `OSD: MAX7456`
- **OSD rendering**: Correct character display, no flicker, no reinit loops, stable >5 minutes
- **BF configuration**: `vcd_video_system = PAL`, `osd_displayport_device = AUTO`
