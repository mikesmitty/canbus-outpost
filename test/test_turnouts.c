// Runs turnouts.c on the real LCC library with the servo driver recorded,
// for a turnout whose servo output carries the signal chain.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "app/config.h"
#include "app/turnouts.h"
#include "lcc/node.h"
#include "lcc_task.h"
#include "servo.h"
#include "timers.h"

static int verbose;
void dbg_printf(const char *fmt, ...) { if (!verbose) return; va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }

// The LCC task runs calls straight away here
bool lcc_task_call(lcc_task_fn_t fn, uint32_t arg) { fn(arg); return true; }

config_layout_t config;
TimerCallbackFunction_t mock_timer_cb;
bool mock_timer_active;

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define NODE_ID  0x050101011234ULL
#define OTHER    0x0AB

// The servo driver, recorded. Every move is instant.
static bool attached[SERVO_COUNT];
static int moves[SERVO_COUNT], disables[SERVO_COUNT];
void servo_set_attached(int i, bool a) { attached[i] = a; }
void servo_set_target(int i, uint16_t target, uint8_t throw_time, uint8_t hold_time, uint8_t frog_pin, uint16_t midpoint)
{
    (void)target; (void)throw_time; (void)hold_time; (void)frog_pin; (void)midpoint;
    moves[i]++;
}
void servo_disable(int i) { disables[i]++; }
bool servo_is_moving(int i) { (void)i; return false; }
bool servo_busy(void) { return false; }
uint8_t servo_tick(void) { return 0; }

static lcc_frame_t sent[256];
static int nsent;
static uint32_t now = 1000;

static bool hook_send(const lcc_frame_t *f) { if (nsent < 256) sent[nsent++] = *f; return true; }

static bool has(uint16_t mti, const uint8_t *id)
{
    for (int i = 0; i < nsent; i++)
        if (!lcc_is_control_frame(sent[i].id) && sent[i].dlc == 8 &&
            lcc_frame_mti(sent[i].id) == mti && memcmp(sent[i].data, id, 8) == 0)
            return true;
    return false;
}

static void rx(uint16_t mti, const uint8_t *id)
{
    lcc_frame_t f = { .id = lcc_message_id(mti, OTHER), .dlc = 8 };
    memcpy(f.data, id, 8);
    nsent = 0;
    lcc_frame_received(&f, now);
}

static void throw_turnout(int t) { rx(LCC_MTI_EVENT_REPORT, config.servos[t].throw_event); }
static bool fed_back(int t) { return has(LCC_MTI_EVENT_REPORT, config.servos[t].thrown_feedback); }

static void clear_counts(void)
{
    memset(moves, 0, sizeof(moves));
    memset(disables, 0, sizeof(disables));
}

// Bring up a node with the chain on the given connector in its saved
// configuration
static void setup(int connector)
{
    lcc_node_config_t cfg = { .node_id = NODE_ID, .hooks = { .send = hook_send } };
    lcc_init(&cfg);
    memset(&config, 0, sizeof(config));
    turnouts_defaults(&config, NODE_ID);
    config.chain_connector = (uint8_t)connector;
    for (int i = 0; i < SERVO_COUNT; i++) {
        attached[i] = true;  // as servo_init leaves them
        config.servos[i].fascia_input = (uint8_t)(i + 1);
    }
    clear_counts();
    turnouts_init();

    lcc_start(now);
    now += 200;
    nsent = 0;
    lcc_run_timers(now);
}

static void test_chain_on_led_header(void)
{
    setup(CONFIG_CONNECTOR_LED);
    for (int i = 0; i < SERVO_COUNT; i++) {
        CHECK(attached[i], "servo %d detached", i + 1);
        CHECK(moves[i] == 1, "servo %d: %d moves to its default state", i + 1, moves[i]);
    }
    clear_counts();
    throw_turnout(1);
    CHECK(moves[1] == 1 && fed_back(1), "turnout 2 didn't throw");
}

static void test_chain_on_a_servo(void)
{
    setup(CONFIG_CONNECTOR_SERVO1 + 1);  // servo 2
    CHECK(!attached[1], "servo 2 still attached");
    CHECK(moves[1] == 0, "servo 2 moved to its default state");
    CHECK(attached[0] && moves[0] == 1 && attached[2] && attached[3], "other servos affected");

    clear_counts();
    throw_turnout(1);
    CHECK(moves[1] == 0 && !fed_back(1), "turnout 2 acted on a throw");
    rx(LCC_MTI_EVENT_REPORT, config.servos[1].center_event);
    CHECK(moves[1] == 0, "turnout 2 centered");
    turnouts_input_pressed(2);
    CHECK(moves[1] == 0, "turnout 2 acted on its fascia input");

    throw_turnout(0);
    CHECK(moves[0] == 1 && fed_back(0), "turnout 1 didn't throw");
}

static void test_moving_the_chain(void)
{
    setup(CONFIG_CONNECTOR_SERVO1 + 1);

    // Onto servo 3: servo 2 comes back, servo 3 stops
    clear_counts();
    config.chain_connector = CONFIG_CONNECTOR_SERVO1 + 2;
    turnouts_config_written();
    CHECK(attached[1] && !attached[2], "attached %d %d", attached[1], attached[2]);
    CHECK(disables[2] > 0, "servo 3 left running");
    throw_turnout(1);
    CHECK(moves[1] == 1 && fed_back(1), "turnout 2 didn't come back");
    throw_turnout(2);
    CHECK(moves[2] == 0 && !fed_back(2), "turnout 3 still in service");

    // Back to the LED header
    config.chain_connector = CONFIG_CONNECTOR_LED;
    turnouts_config_written();
    for (int i = 0; i < SERVO_COUNT; i++)
        CHECK(attached[i], "servo %d still detached", i + 1);
}

static void test_disabled_turnout_on_the_chain(void)
{
    // A disabled turnout's servo still gives up and takes back its pin
    setup(CONFIG_CONNECTOR_LED);
    config.servos[3].enabled = 0;
    config.chain_connector = CONFIG_CONNECTOR_SERVO1 + 3;
    turnouts_config_written();
    CHECK(!attached[3], "servo 4 still attached");
    config.chain_connector = CONFIG_CONNECTOR_LED;
    turnouts_config_written();
    CHECK(attached[3], "servo 4 not attached again");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    test_chain_on_led_header();
    test_chain_on_a_servo();
    test_moving_the_chain();
    test_disabled_turnout_on_the_chain();
    printf("turnouts: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
