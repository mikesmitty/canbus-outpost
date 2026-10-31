# canbus-outpost

An [LCC](https://openlcb.org/) (OpenLCB) node for model railroads, built around the RP2350. One board:

- drives four servo switch machines, with frog outputs and fascia toggles
- detects RailCom decoders on four track blocks, reporting occupancy and locomotives
- drives up to 16 signal heads on an SSSB chain
- bridges the CAN bus to a host over USB GridConnect
- takes firmware upgrades over LCC

Everything is set through the node's LCC configuration, so it works with any LCC configuration tool. Signals beyond simple links need a host, such as [Interlock](https://github.com/mikesmitty/interlock).

The hardware is in `hardware/canbus-outpost/` (KiCad), with gerbers, BOM and pick-and-place files in `production/`.

## Signal heads

> [!NOTE]
> The signal chain hasn't been run on hardware yet. The firmware passes its host tests, but the chain speed and supply voltage of RR-CirKits' heads still need measuring. Boards made before commit 7316e79 have no signal header.

The board speaks [RR-CirKits](https://www.rr-cirkits.com/)' Simple Serial Signal Bus (SSSB) on J15: one WS2811 chip per head, daisy-chained with Futaba J servo cables. J15 supplies 5 V through a 750 mA fuse. Head 1 is the first on the cable, and every chip has to be connected for the ones after it to work, enabled or not. WS2811 outputs only sink current, so heads need common-anode LEDs.

### Setting up a head

Heads start disabled, since the board can't tell which are fitted. Under **Signal Heads** in the configuration:

1. Set **Chain Speed** to match the chips' SET pin: 800 kHz when open, 400 kHz when tied to power.
2. Set the head's **Enabled**.
3. Set the lamp on each output. The defaults are red, green and yellow on OUTR, OUTG and OUTB, which is the SSSB wiring. Each output also has its own brightness.

Each aspect has its own event, and only the aspects a head has lamps for are on the bus:

| Aspect | Shows |
| --- | --- |
| Dark | nothing |
| Red, Yellow, Green | that lamp |
| Flashing Red, Yellow, Green | that lamp, at 60 flashes a minute, with all flashing heads in step |
| Lunar | a lunar lamp |
| S, D, Number | the lit letters and speed number of NYC subway time signals |

A head lights one kind of lamp at a time, so a light that shows alongside another, like lunar under a red, takes a chip of its own: for example head 1 as red/yellow/green and head 2 as lunar/S/D.

To check a chip without LCC, the USB console's `lights <chip> <r> <g> <b>` sets its outputs directly until the application next updates that chip. Chips count from 0 here.

### What to expect from a host

The node has no signal logic. A head shows the last aspect event it received.

- **Simple cases need no host.** An aspect can use the same event ID as a single input, like a turnout's feedback event. Anything that depends on occupancy, routes or the next signal needs a host to decide and send the aspects.
- **A signal with two heads takes one event per head.** Each aspect holds one event ID, so a single event for a whole indication, like red over lunar, only works on a one-chip signal. Two indications that both show red would need two IDs on the same aspect.
- **Heads that always match can share event IDs.** Share all of them or none, or a host that reads a head's state back can mistake one head's aspect for the other's.
- **A reset puts a head on its Power-On Aspect** until the host sends it another.
- **A head doesn't announce changes.** It answers an event identify with the aspect it's showing.

## Building

You'll need CMake, Ninja, `arm-none-eabi-gcc` and `picotool`, the Pico SDK 2.2.0 at `~/.pico-sdk/sdk/2.2.0/`, and FreeRTOS-Kernel at `FREERTOS_KERNEL_PATH`.

```bash
make                  # build the firmware
make flash            # build and load it with picotool
make test             # run the host tests; no hardware needed
```

Once per board, in BOOTSEL mode, install the A/B partition table that LCC firmware upgrades need. It keeps the saved configuration and node ID:

```bash
make partition-table
```

Flash with picotool rather than SWD, since loading over SWD replaces the partition table.

## Node ID

Set each board's ID from your OpenLCB-assigned range on the USB console:

```
nodeid 05.01.01.01.12.34
```

Until then the board uses a locally allocated stand-in with an `0A` prefix, made from the chip's device ID.
