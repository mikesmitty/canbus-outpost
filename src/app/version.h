#ifndef VERSION_H
#define VERSION_H

// Identification reported over LCC (Simple Node Information). Keep these in
// step with the <identification> block in cdi_data.c.
#define FIRMWARE_MANUFACTURER  "mikesmitty"
#define FIRMWARE_MODEL         "CAN Bus Outpost"
#define FIRMWARE_HW_VERSION    "Rev A"

#ifdef LCC_SW_VERSION
#define FIRMWARE_SW_VERSION    LCC_SW_VERSION  // git describe, from CMake
#else
// x-release-please-start-version
#define FIRMWARE_SW_VERSION    "0.3.1"
// x-release-please-end
#endif

#endif // VERSION_H
