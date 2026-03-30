#include "lcc_interface.h"
#include "railcom.h"
#include "servo.h"
#include "nv_storage.h"
#include "can2040.h"
#include "board.h"
#include "openlcb/openlcb_config.h"
#include "openlcb/openlcb_gridconnect.h"
#include "openlcb/openlcb_defines.h"
#include "openlcb/openlcb_application.h"
#include "openlcb/openlcb_node.h"
#include "drivers/canbus/can_config.h"
#include "drivers/canbus/can_types.h"
#include "drivers/canbus/can_utilities.h"
#include "drivers/canbus/can_rx_statemachine.h"

#include "pico/unique_id.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "util/dbg.h"
#include <string.h>
#include <stdio.h>

static struct can2040 cbus;
static openlcb_node_t *g_node;
static bool config_dirty = false;
static absolute_time_t next_flash_flush;

// --- GridConnect helpers ---

static void send_to_usb_as_gridconnect(can_msg_t *msg) {
    gridconnect_buffer_t gc_buf;
    OpenLcbGridConnect_from_can_msg(&gc_buf, msg);
    
    uint16_t len = 0;
    while (gc_buf[len] != 0 && len < MAX_GRID_CONNECT_LEN) len++;
    
    fwrite(gc_buf, 1, len, stdout);
    fwrite("\n", 1, 1, stdout);
}

// --- CAN driver shim ---

static bool can_transmit(can_msg_t *can_msg) {
    struct can2040_msg msg;
    msg.id = can_msg->identifier;
    msg.dlc = can_msg->payload_count;
    memcpy(msg.data, can_msg->payload, 8);

    if (can2040_check_transmit(&cbus)) {
        can2040_transmit(&cbus, &msg);
        send_to_usb_as_gridconnect(can_msg);
        return true;
    }
    return false;
}

static bool can_is_tx_clear(void) {
    return can2040_check_transmit(&cbus);
}

static void can_lock(void) { }
static void can_unlock(void) { }

// --- CAN RX Callback ---

static void can2040_cb(struct can2040 *cd, uint32_t notify, struct can2040_msg *msg) {
    if (notify & CAN2040_NOTIFY_RX) {
        can_msg_t olcb_msg;
        olcb_msg.identifier = msg->id;
        olcb_msg.payload_count = msg->dlc;
        memcpy(olcb_msg.payload, msg->data, 8);
        CanRxStatemachine_incoming_can_driver_callback(&olcb_msg);
        send_to_usb_as_gridconnect(&olcb_msg);
    }
}

// --- GridConnect RX from USB ---

static void process_usb_gridconnect(void) {
    int c;
    gridconnect_buffer_t gc_buf;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (OpenLcbGridConnect_copy_out_gridconnect_when_done((uint8_t)c, &gc_buf)) {
            can_msg_t msg;
            OpenLcbGridConnect_to_can_msg(&gc_buf, &msg);
            
            struct can2040_msg cmsg;
            cmsg.id = msg.identifier;
            cmsg.dlc = msg.payload_count;
            memcpy(cmsg.data, msg.payload, 8);
            
            if (can2040_check_transmit(&cbus)) {
                can2040_transmit(&cbus, &cmsg);
            }
        }
    }
}

// --- OpenLCB config memory stubs (RAM with Flash persistence) ---

#define CONFIG_MEM_SIZE 512
static uint8_t config_mem[CONFIG_MEM_SIZE];

static uint16_t config_memory_read(openlcb_node_t *node, uint32_t address,
                                uint16_t count, configuration_memory_buffer_t *buffer) {
    if (address >= CONFIG_MEM_SIZE) return 0;
    if (address + count > CONFIG_MEM_SIZE) count = CONFIG_MEM_SIZE - address;
    memcpy(buffer, &config_mem[address], count);
    return count;
}

static uint16_t config_memory_write(openlcb_node_t *node, uint32_t address,
                                 uint16_t count, configuration_memory_buffer_t *buffer) {
    if (address >= CONFIG_MEM_SIZE) return 0;
    if (address + count > CONFIG_MEM_SIZE) count = CONFIG_MEM_SIZE - address;
    
    if (memcmp(&config_mem[address], buffer, count) != 0) {
        memcpy(&config_mem[address], buffer, count);
        config_dirty = true;
        next_flash_flush = delayed_by_ms(get_absolute_time(), 2000);
    }
    return count;
}

// --- Node ID from hardware ---

static node_id_t get_unique_node_id(void) {
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    node_id_t id = 0x060100000000ULL;
    id |= ((uint64_t)board_id.id[4] << 24);
    id |= ((uint64_t)board_id.id[5] << 16);
    id |= ((uint64_t)board_id.id[6] << 8);
    id |= ((uint64_t)board_id.id[7]);
    return id;
}

// --- RailCom Callbacks ---

void railcom_on_data(int channel, railcom_data_t *data) {
    DBG("RailCom Ch %d: Address %d, Occupied %d\n", channel, data->address, data->occupied);
    
    if (g_node) {
        event_id_t event = g_node->id | (uint64_t)channel;
        OpenLcbApplication_send_event_pc_report(g_node, event);
    }
}

void railcom_on_signal(int channel, bool active) {
    DBG("RailCom Ch %d: Cutout signal %s\n", channel, active ? "OK" : "LOST");
    
    if (g_node) {
        // Event ID = NodeID | 0x10 + (channel * 2) + !active
        event_id_t event = g_node->id | (uint64_t)(0x10 + (channel * 2) + !active);
        OpenLcbApplication_send_event_pc_report(g_node, event);
    }
}

// --- LCC well-known event handling (servos, GPIOs) ---

static void on_pc_event_report(openlcb_node_t *node, event_id_t *event_id) {
    if (node != g_node) return;

    if ((*event_id & 0xFFFFFFFFFFFF0000ULL) == g_node->id) {
        uint16_t sub_id = (uint16_t)(*event_id & 0xFFFF);
        
        if (sub_id >= 0x100 && sub_id < 0x100 + (SERVO_COUNT * 2)) {
            int index = (sub_id - 0x100) / 2;
            int state = (sub_id - 0x100) % 2;
            servo_set_position(index, state ? 65535 : 0);
        }
        
        static const uint8_t gpio_pins[] = {GPIO_OUT1, GPIO_OUT2, GPIO_OUT3, GPIO_OUT4};
        if (sub_id >= 0x200 && sub_id < 0x200 + (4 * 2)) {
            int index = (sub_id - 0x200) / 2;
            int state = (sub_id - 0x200) % 2;
            gpio_put(gpio_pins[index], state);
        }
    }
}

void lcc_interface_init(void) {
    can2040_setup(&cbus, 0);
    can2040_callback_config(&cbus, can2040_cb);
    can2040_start(&cbus, 125000000, 125000, CAN_RX_PIN, CAN_TX_PIN);

    railcom_init();
    servo_init();

    static const uint8_t gpio_pins[] = {GPIO_OUT1, GPIO_OUT2, GPIO_OUT3, GPIO_OUT4};
    for (int i = 0; i < 4; i++) {
        gpio_init(gpio_pins[i]);
        gpio_set_dir(gpio_pins[i], GPIO_OUT);
    }

    if (!nv_storage_init(config_mem, CONFIG_MEM_SIZE)) {
        memset(config_mem, 0, CONFIG_MEM_SIZE);
        config_dirty = true;
        next_flash_flush = delayed_by_ms(get_absolute_time(), 2000);
    }

    static const can_config_t can_cfg = {
        .transmit_raw_can_frame  = can_transmit,
        .is_tx_buffer_clear      = can_is_tx_clear,
        .lock_shared_resources   = can_lock,
        .unlock_shared_resources = can_unlock,
    };
    CanConfig_initialize(&can_cfg);

    static const openlcb_config_t olcb_cfg = {
        .lock_shared_resources   = can_lock,
        .unlock_shared_resources = can_unlock,
        .config_mem_read         = config_memory_read,
        .config_mem_write        = config_memory_write,
        .on_pc_event_report      = on_pc_event_report,
    };
    OpenLcb_initialize(&olcb_cfg);

    extern const node_parameters_t OpenLcbUserConfig_node_parameters;
    node_id_t id = get_unique_node_id();
    g_node = OpenLcb_create_node(id, &OpenLcbUserConfig_node_parameters);

    // Register consumers
    for (int i = 0; i < SERVO_COUNT * 2; i++) {
        OpenLcbApplication_register_consumer_eventid(g_node, g_node->id | (0x100 + i), EVENT_STATUS_UNKNOWN);
    }
    for (int i = 0; i < 4 * 2; i++) {
        OpenLcbApplication_register_consumer_eventid(g_node, g_node->id | (0x200 + i), EVENT_STATUS_UNKNOWN);
    }
}

void lcc_interface_run(void) {
    process_usb_gridconnect();
    railcom_run();
    OpenLcb_run();

    if (config_dirty && absolute_time_diff_us(get_absolute_time(), next_flash_flush) <= 0) {
        if (nv_storage_write(config_mem, CONFIG_MEM_SIZE)) {
            config_dirty = false;
        }
    }
}

void lcc_interface_100ms_tick(void) {
    OpenLcb_100ms_timer_tick();
}
