#ifndef FIRMWARE_UPDATE_H
#define FIRMWARE_UPDATE_H

// Firmware updates over LCC into the RP2350's A/B flash partitions
// (partition_table.json). A new image is written to the partition that
// isn't running and started with a flash-update boot. It then runs on
// trial: it confirms itself once the node has been up for a few seconds,
// and if it doesn't, the next boot goes back to the previous image. A
// confirmed image then installs its partition table if the board's differs
// (partition_update.h).
//
// Everything here runs in the LCC task.

// After lcc_init: offers firmware upgrades to LCC tools when the board has
// a partition table, and starts the trial clock for a new image.
void firmware_update_init(void);

// The LCC node is up (alias claimed): shortly, a trial image confirms
// itself and the partition table is brought up to date.
void firmware_update_node_up(void);

// The LCC library's reboot hook: starts an accepted new image, otherwise
// reboots normally.
void firmware_update_reboot(void);

#endif // FIRMWARE_UPDATE_H
