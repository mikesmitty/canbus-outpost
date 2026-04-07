# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

canbus-outpost is a Raspberry Pi Pico 2 (RP2350) firmware and hardware project for model railroad control. It provides CAN bus connectivity, RailCom detection, servo control, and GPIO.

## Build Commands

```bash
# Build firmware (CMake + Ninja)
make

# Clean build artifacts
make clean

# Build and flash to connected Pico 2 via picotool
make flash
```

The build system uses CMake 3.13+ generating Ninja build files. The Pico SDK (v2.2.0) is expected at `~/.pico-sdk/sdk/2.2.0/`. The toolchain is `arm-none-eabi-gcc` from the Pico Toolchain.

Build artifacts go to `firmware/canbus-outpost/build/` (`.elf` and `.uf2` files).

There is no test suite — verification is manual (UART debug, physical hardware testing).

## Architecture

### Firmware (`firmware/canbus-outpost/`)

The main entry point is `canbus-outpost.cpp`. Application code is in `src/`. The firmware runs bare-metal on the RP2350 (no RTOS). There is currently no protocol stack — only hardware drivers.

**Core drivers:**
- **can2040** (`src/can2040.c/h`): PIO-based software CAN bus driver. Handles bit-level CAN protocol via RP2350 Programmable I/O state machines.
- **railcom** (`src/railcom.c/h`): 4-channel RailCom detector using PIO UART receivers at 250kbps. Uses cutout-based gating for DCC synchronization. The PIO program is in `src/railcom_sample.pio`.
- **servo** (`src/servo.c/h`): 4-channel PWM servo output at 50Hz.
- **nv_storage** (`src/nv_storage.c/h`): Persistent configuration in the last flash sector.

**Pin assignments** are defined in `src/board.h`.

### PIO Programs

PIO programs must be `.pio` assembly files (not inline C). They are compiled by `pioasm` as part of the CMake build. Current PIO programs:
- `blink.pio` — heartbeat LED
- `src/railcom_uart.pio` — RailCom UART receiver (10 cycles/bit, middle-bit sampling)

### Hardware (`hardware/canbus-outpost/`)

KiCad 6+ design files. Key components: RP2350, TLV3202 comparator (RailCom signal detection), RJ45 CAN bus connector. Manufacturing outputs (gerbers, BOM, pick-and-place) are in `production/`.

### Planning Docs (`conductor/`)

Phase documents tracking project evolution. `phase-1.md` has the most current MVP firmware spec.

## Key Conventions

- **Language**: C11, compiled with `arm-none-eabi-gcc`
- **Commit messages**: Conventional commit format (`feat:`, `fix:`, `chore:`, etc.)
- **USB stdio**: Enabled (UART stdio disabled). USB CDC enumeration timeout is 2 seconds.
- **Debug output**: RTT (Real-Time Transfer) is enabled. Debug UART on pins 0/1 at 115200 baud.
- **CMake target board**: `pico2` (RP2350)
