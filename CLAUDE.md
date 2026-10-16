# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

canbus-outpost is a Raspberry Pi Pico 2 (RP2350) firmware and hardware project for model railroad control. It is an LCC (OpenLCB) node that drives servo switch machines and detects RailCom decoders, with a USB GridConnect bridge to a host.

## Build Commands

```bash
# Build firmware (CMake + Ninja)
make

# Clean build artifacts
make clean

# Build and flash to connected Pico 2 via picotool
make flash

# Once per board, in BOOTSEL mode: install the A/B partition table (needed
# for LCC firmware upgrades), keeping the saved configuration and node ID
make partition-table

# Host-side tests: LCC library, CDI/config layout, RailCom decoder and
# events, firmware image checks
make test
```

The build system uses CMake 3.13+ generating Ninja build files. The Pico SDK (v2.2.0) is expected at `~/.pico-sdk/sdk/2.2.0/`, and FreeRTOS-Kernel at the path in `FREERTOS_KERNEL_PATH`. The toolchain is `arm-none-eabi-gcc` from the Pico Toolchain.

Build artifacts go to `build/`: `.uf2`/`.elf` for USB, `.bin` for LCC firmware upgrades.

`make test` builds and runs the host tests with the system C compiler; hardware behavior is verified manually (RTT log, bench testing).

## Architecture

The firmware runs FreeRTOS on core 0 of the RP2350. The entry point is `canbus-outpost.c`.

### LCC library (`lib/lcc/`)

Custom OpenLCB/LCC implementation in plain C11 with no SDK or RTOS calls: frames go out through a send hook and time comes in as a millisecond counter, so it builds and is tested on the host (`lib/lcc/test/`). It covers alias allocation and conflict handling, Verify Node ID, Protocol Support, SNIP, an event producer/consumer registry with producer ranges, datagrams (reassembly, rejection, resend), memory configuration, firmware upgrade and GridConnect framing. Public headers are in `lib/lcc/include/lcc/`.

### Tasks

- **lcc_task** (`src/lcc_task.c`): owns the LCC library, the configuration and the application state. Other tasks and interrupts hand it work through its queue (`lcc_task_post_frame`, `lcc_task_call`). It also feeds the hardware watchdog.
- **can_bus_task** (`src/can_bus.c`): can2040 on PIO1. Received frames go to the LCC task and the USB bridge.
- **usb_gc_rx_task / usb_gc_tx_task** (`src/usb_gridconnect.c`): GridConnect over USB CDC, plus console commands (`nodeid`).
- **railcom_task** (`src/railcom.c`): woken by the CUTOUT_SIGNAL falling edge (GPIO 29); decodes that cutout from the 2 MHz PIO/DMA sample ring (`src/railcom_sample.pio`) for all four blocks (UART line = !(A || B)).

Work is event-driven (queues, task notifications, FreeRTOS timers, GPIO interrupts); avoid polling loops.

### Application (`src/app/`)

- **config** (`config_layout.h`, `config.c`): the LCC configuration space as a byte image in OpenLCB byte order (big-endian). `config_layout_t` must match the CDI in `cdi_data.c`; `make test` checks it.
- **turnouts**: servo switch machines (throw/close/center events, feedback, frog output, fascia toggle).
- **railcom_app**: per track, an Occupied/Clear event pair and locomotive reports in the OpenLCB DCC detector format (a 64K event range per track for JMRI reporters; default reporter IDs are the four unique IDs after the node ID). Track states hold while cutouts are missing (track power off).
- **identify**, **inputs** (debounced fascia buttons).

### Drivers and storage

- **can2040** (`src/can2040.c/h`): PIO-based software CAN controller (third party).
- **servo** (`src/servo.c/h`): 4-channel PWM servo output at 50 Hz with timed motion.
- **nv_storage** (`src/nv_storage.c/h`): power-safe records (two sectors each, CRC) in the top 16 KB of flash, outside the partitions, for the configuration and the node ID. Read through the untranslated XIP window, since a partition boot maps only that partition.
- **firmware_update** (`src/firmware_update.c/h`, `src/fw_image.c/h`): LCC firmware upgrades into the A/B partitions of `partition_table.json`. Images are built try-before-you-buy with a SHA-256 hash: a new image runs on trial and confirms itself 5 s after the node comes up; if it resets before then, or hasn't confirmed within 60 s, the next boot returns to the previous one. Without a partition table the node doesn't offer upgrades. Loading firmware over SWD writes at the start of flash and replaces the partition table, so flash with picotool.
- **node_id** (`src/node_id.c/h`): the LCC node ID, set per board with the `nodeid` USB command from an OpenLCB-assigned range. Until then a locally-allocated stand-in (`0A` prefix, from the chip's device ID) is used, which can't clash with other vendors' IDs. The LCC library checks for another node with the same ID after joining the bus and reports one (well-known event, USB console, fast-blinking identify LED).

**Pin assignments** are defined in `src/board.h`.

### PIO Programs

PIO programs must be `.pio` assembly files (not inline C). They are compiled by `pioasm` as part of the CMake build. Current PIO programs:
- `src/railcom_sample.pio` — RailCom sampler (16 GPIOs at 2 MHz, DMA to a ring buffer)

### Hardware (`hardware/canbus-outpost/`)

KiCad design files. Key components: RP2350, TLV3202 window comparators and ISO7720 isolators (RailCom detection), RJ45 CAN bus connector. Manufacturing outputs (gerbers, BOM, pick-and-place) are in `production/`. `canbus-outpost.kicad_dru` holds the track-to-logic isolation clearance rule.

### Planning Docs (`conductor/`)

Phase documents tracking project evolution. `phase-1.md` has the most current MVP firmware spec.

## Key Conventions

- **Language**: C11, compiled with `arm-none-eabi-gcc`
- **Commit messages**: Conventional commit format (`feat:`, `fix:`, `chore:`, etc.)
- **USB stdio**: Enabled (UART stdio disabled), used only for GridConnect and console output. USB CDC enumeration timeout is 2 seconds.
- **Debug output**: `DBG()` writes to RTT; RTT is removed from stdio at startup so GridConnect traffic stays off it.
- **CMake target board**: `pico2` (RP2350)
