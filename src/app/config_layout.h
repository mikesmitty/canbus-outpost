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
#define CONFIG_HEADS   16

// Signal head aspects
#define CONFIG_ASPECT_DARK             0
#define CONFIG_ASPECT_RED              1
#define CONFIG_ASPECT_YELLOW           2
#define CONFIG_ASPECT_GREEN            3
#define CONFIG_ASPECT_LUNAR            4
#define CONFIG_ASPECT_S                5
#define CONFIG_ASPECT_D                6
#define CONFIG_ASPECT_NUMBER           7
#define CONFIG_ASPECT_FLASHING_RED     8
#define CONFIG_ASPECT_FLASHING_YELLOW  9
#define CONFIG_ASPECT_FLASHING_GREEN   10
#define CONFIG_ASPECTS                 11

// The lamp on a head's output. S, D and Number are the lit letters and speed
// number of NYC subway time signals.
#define CONFIG_LAMP_NONE    0
#define CONFIG_LAMP_RED     1
#define CONFIG_LAMP_YELLOW  2
#define CONFIG_LAMP_GREEN   3
#define CONFIG_LAMP_LUNAR   4
#define CONFIG_LAMP_S       5
#define CONFIG_LAMP_D       6
#define CONFIG_LAMP_NUMBER  7

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

// One signal head: a chip on the WS2811 chain with a lamp on each output
typedef struct {
    uint8_t description[32];
    uint8_t aspect_events[CONFIG_ASPECTS][8];  // by CONFIG_ASPECT_*
    uint8_t enabled;
    uint8_t power_on_aspect;       // CONFIG_ASPECT_*
    uint8_t output_lamp[3];        // CONFIG_LAMP_* on OUTR, OUTG and OUTB
    uint8_t output_brightness[3];
    uint8_t reserved[8];
} config_head_t;

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
    // Segment "Signal Heads", origin 996
    uint8_t chain_speed;     // CONFIG_CHAIN_*
    uint8_t chain_reserved[3];
    config_head_t heads[CONFIG_HEADS];
} config_layout_t;

#define CONFIG_DEFAULT_THROWN     0
#define CONFIG_DEFAULT_CLOSED     1
#define CONFIG_DEFAULT_NO_CHANGE  2

// Which rail the detector senses, for reporting locomotive orientation
#define CONFIG_ORIENT_OFF     0
#define CONFIG_ORIENT_RAIL_A  1
#define CONFIG_ORIENT_RAIL_B  2

#define CONFIG_CHAIN_800KHZ  0
#define CONFIG_CHAIN_400KHZ  1

_Static_assert(sizeof(config_servo_t) == 120, "servo group stride must match the CDI");
_Static_assert(offsetof(config_layout_t, identify_enabled) == 128, "Identify segment origin");
_Static_assert(offsetof(config_layout_t, servos) == 132, "Switch Machines segment origin");
_Static_assert(sizeof(config_track_t) == 96, "track group stride must match the CDI");
_Static_assert(offsetof(config_layout_t, tracks) == 612, "RailCom Detectors segment origin");
_Static_assert(sizeof(config_head_t) == 136, "head group stride must match the CDI");
_Static_assert(offsetof(config_layout_t, chain_speed) == 996, "Signal Heads segment origin");
_Static_assert(sizeof(config_layout_t) == 3176, "configuration space size");

#endif // CONFIG_LAYOUT_H
