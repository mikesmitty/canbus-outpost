#ifndef CONFIG_LAYOUT_H
#define CONFIG_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

// Layout of the configuration memory space (0xFD) described by the CDI in
// cdi_data.c. Every field is a byte array holding the value exactly as LCC
// tools read and write it (multi-byte values big-endian), so the struct has
// no padding and offsetof() gives each field's CDI address. Read values
// with the lcc/bytes.h helpers.
//
// Bump CONFIG_VERSION when existing fields move or change meaning. Fields
// added at the end keep old saved configurations loadable: missing bytes
// take their defaults.

#define CONFIG_VERSION 1

#define CONFIG_SERVOS  4
#define CONFIG_TRACKS  4

typedef struct {
    uint8_t throw_event[8];
    uint8_t close_event[8];
    uint8_t thrown_feedback[8];
    uint8_t closed_feedback[8];
    uint8_t center_event[8];
    uint8_t thrown_position[2];
    uint8_t closed_position[2];
    uint8_t center_position[2];
    uint8_t throw_time;      // tenths of a second, 0 = instant
    uint8_t hold_time;       // seconds of PWM after arriving, 0 = forever
    uint8_t enabled;
    uint8_t default_state;   // CONFIG_DEFAULT_*
    uint8_t reversed;
    uint8_t frog_output;     // 0 = none, 1-4 = GPIO_OUT1-4
    uint8_t fascia_input;    // 0 = none, 1-4 = GPIO_IN1-4
    uint8_t description[64];
    uint8_t reserved[3];
} config_servo_t;

// One RailCom detector block
typedef struct {
    uint8_t description[64];
    uint8_t occupied_event[8];
    uint8_t clear_event[8];
    uint8_t reporter_id[8];  // DCC detector event range base, low two bytes 0
    uint8_t orientation;     // CONFIG_ORIENT_*
    uint8_t reserved[7];
} config_track_t;

typedef struct {
    // Segment "Node Identity", origin 0
    uint8_t user_name[64];
    uint8_t user_description[64];
    // Segment "Identify", origin 128
    uint8_t identify_enabled;
    uint8_t identify_timeout_minutes;  // 0 = indefinite
    uint8_t identify_reserved[2];
    // Segment "Switch Machines", origin 132
    config_servo_t servos[CONFIG_SERVOS];
    // Segment "RailCom Detectors", origin 612
    config_track_t tracks[CONFIG_TRACKS];
} config_layout_t;

#define CONFIG_DEFAULT_THROWN     0
#define CONFIG_DEFAULT_CLOSED     1
#define CONFIG_DEFAULT_NO_CHANGE  2

// Which rail the detector senses, for reporting locomotive orientation
#define CONFIG_ORIENT_OFF     0
#define CONFIG_ORIENT_RAIL_A  1
#define CONFIG_ORIENT_RAIL_B  2

_Static_assert(sizeof(config_servo_t) == 120, "servo group stride must match the CDI");
_Static_assert(offsetof(config_layout_t, identify_enabled) == 128, "Identify segment origin");
_Static_assert(offsetof(config_layout_t, servos) == 132, "Switch Machines segment origin");
_Static_assert(sizeof(config_track_t) == 96, "track group stride must match the CDI");
_Static_assert(offsetof(config_layout_t, tracks) == 612, "RailCom Detectors segment origin");
_Static_assert(sizeof(config_layout_t) == 996, "configuration space size");

#endif // CONFIG_LAYOUT_H
