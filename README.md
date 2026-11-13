# canbus-outpost

An [LCC](https://openlcb.org/) (OpenLCB) node for model railroads, built around the RP2350. One board:

- drives four servo switch machines, with frog outputs and fascia toggles
- detects RailCom decoders on four track blocks, reporting occupancy and locomotives
- drives up to 16 signal heads on an SSSB chain
- bridges the CAN bus to a host over USB GridConnect
- takes firmware upgrades over LCC

Everything is set through the node's LCC configuration, so it works with any LCC configuration tool. Signals beyond simple links need a host, such as [Interlock](https://github.com/mikesmitty/interlock).

The hardware is in `hardware/canbus-outpost/`, and a one-head signal board is in `hardware/sssb-single/` (both KiCad), each with gerbers, BOM and pick-and-place files in `production/`.

## Signal heads

> [!NOTE]
> The signal chain hasn't been run on hardware yet; so far the firmware has only passed host tests. Boards made before commit 7316e79 have no LED header, but can use a servo output instead: see [Boards without an LED header](#boards-without-an-led-header).

The board speaks [RR-CirKits](https://www.rr-cirkits.com/)' Simple Serial Signal Bus (SSSB) on the LED header: one WS2811 chip per head, daisy-chained with Futaba J servo cables. The LED header supplies 5 V through a 750 mA fuse. Head 1 is the first on the cable unless its **Chain Position** says otherwise, and every chip has to be connected for the ones after it to work, in use or not. WS2811 outputs only sink current, so heads need common-anode LEDs.

### Setting up a head

Heads start disabled, since the board can't tell which are fitted. Under **Signal Heads** in the configuration:

1. Set **Chain Speed** to match the chips' SET pin: 800 kHz when open, 400 kHz when tied to power. RR-CirKits' heads leave it open, so they run at the default of 800 kHz.
2. Set the head's **Enabled**.
3. Set the lamp on each output: **R Lamp**, **Y Lamp** and **G Lamp**, named as on an SSSB head's RGY header. The defaults are red on R, yellow on Y and green on G, which is the SSSB wiring, but any lamp can go on any output. Each output also has its own brightness.
4. Optionally, set **Lamp Fade** to Incandescent. The lamps then warm up over 150 ms and cool down over 300 ms, like ballasted bulbs, instead of switching at once. Flashing aspects fade too.
5. Optionally, set the **Flash Rate** for its flashing aspects: slow (40 a minute), medium (60, the default) or fast (90). Each output also has a **Phase**: phase B lamps light while phase A lamps are dark, so two red lamps can alternate like a crossing flasher, on one head or on two at the same rate.

To add a chip partway along the chain, set the **Chain Position** of each head after it to one more than its number, instead of setting those heads up again on their new numbers. 0, the default, means the head's own number. Two heads on one chip both show on it.

Each aspect has its own event, and only the aspects a head has lamps for are on the bus:

| Aspect | Shows |
| --- | --- |
| Dark | nothing |
| Red, Yellow, Green | that lamp |
| Flashing Red, Yellow, Green | that lamp, at the head's Flash Rate and in each output's Phase, with heads at one rate in step |
| Lunar | a lunar lamp |
| S, D, Number | the lit letters and speed number of NYC subway time signals |

A head lights one kind of lamp at a time, so a light that shows alongside another, like lunar under a red, takes a chip of its own: for example head 1 as red/yellow/green and head 2 as lunar/S/D.

### Lamps of their own

An output set to **Own Events** isn't part of the aspects. It has an On and an Off event of its own, under **Own Events** at the end of **Signal Heads**, and lights at its brightness, fading if the head fades. Use it for panel lights, building lights, or lamps that must light together on one chip. Its events are on the bus while its head is enabled, and it starts out dark. A head whose outputs are all Own Events or None has no aspects on the bus, not even Dark.

### Boards without an LED header

On boards made before the LED header, a servo output can carry the chain:

1. Buffer the data line to 5 V. The servo outputs come straight from the RP2350 at 3.3 V, short of the 3.5 V a WS2811 on 5 V needs for a high. A 74AHCT125-family buffer powered from the servo header's 5 V does it, followed by a 33 Ω resistor as on the LED header. MOSFET level shifters of the kind sold for I2C are too slow at 800 kHz. The [head board](#head-board) can carry the buffer itself.
2. Set **Connected To** under **Signal Heads** to that servo. Its switch machine is then out of use, as if disabled, and the other three work as usual.

The servo header's 5 V comes straight from the board's regulator, without the LED header's 750 mA fuse, so put a fuse in the adapter.

### Head board

`hardware/sssb-single/` is a board for one head: a WS2811 with IN and OUT headers for the chain and an RGY header for a common-anode head. IN and OUT are S, +, − in servo cable order, the same as the LED header, and RGY is 5V, R, G, Y. SET is open, so it runs at 800 kHz. It hasn't been built yet.

It's assembled one of two ways:

- **R1 fitted** (the default): for a head fed by the LED header or another head.
- **U1 and C2 fitted, R1 left off**: the head buffers its own input to 5 V, so it can be the first head on a servo output. U1 is a 74AHCT1G125.

Never fit U1 and R1 together: U1's 5 V output would drive back into the pin feeding the chain. The board has no fuse, so on a servo output put one in the cable.

### Checking the chain

**Signal Test**, at the end of **Signal Heads**, has a Start and a Stop event. From Start until Stop, every chip on the chain, set up or not, lights red, yellow and green in turn and then none, a second each (outputs R, Y and G, as SSSB heads are wired). The heads still follow their events meanwhile, and go back to their aspects at Stop. Each node has its own pair by default; give every node the same pair to test all the chains on the layout at once.

On the USB console, `lights test` does the same until `lights test` again, and `lights <chip> <r> <y> <g>` sets one chip's red, yellow and green directly until the application next updates that chip, ending any test. Chips count from 0 here.

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

Once per board, in BOOTSEL mode, install the A/B partition table that LCC firmware upgrades need. It keeps the saved configuration and node ID in the top 64 KB of flash:

```bash
make partition-table
```

A running board goes into BOOTSEL mode without its button through `picotool reboot -f -u` over USB.

Flash with picotool rather than SWD, since loading over SWD replaces the partition table.

After that, firmware upgrades over CAN bring the partition table along. The firmware carries the table from `partition_table.json`, and once a newly installed image has confirmed itself, it checks the board's table. If that one is older, the firmware writes the new one beside it, since the boot ROM keeps two and uses the newer. The next reboot uses it: a power cycle, or Reboot in Interlock's node configuration. The old table stays in place until then, so a power cut while the new one is written changes nothing.

Boards set up before the top 64 KB was kept clear have partition B running up to the last 16 KB, and get the new table that way. Until they reboot with it, they keep working and keep their configuration, but can save only 4 KB of it. The configuration moves across on its first save after the reboot.

## Node ID

Set each board's ID from your OpenLCB-assigned range on the USB console:

```
nodeid 05.01.01.01.12.34
```

Until then the board uses a locally allocated stand-in with an `0A` prefix, made from the chip's device ID.
