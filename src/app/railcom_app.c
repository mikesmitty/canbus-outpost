// RailCom detector reports. railcom.c calls these from railcom_task; they
// are handed to the LCC task, where producing LCC events would go.
#include "railcom.h"
#include "lcc_task.h"
#include "util/dbg.h"

// railcom_data_t packed into a task-call argument
#define PACK_CHANNEL(p)   ((p) & 0x3)
#define PACK_OCCUPIED     (1u << 2)
#define PACK_TYPE(p)      (((p) >> 3) & 0x3)
#define PACK_ORIENT(p)    (((p) >> 5) & 0x3)
#define PACK_ADDRESS(p)   ((p) >> 16)

static void data_in_lcc_task(uint32_t p)
{
    static const char *const type_name[] = { "short", "long", "consist" };
    static const char *const orient_name[] = { "?", "A", "B" };

    DBG("RailCom T%d: %s loco=%u (%s, %s)\n", (int)PACK_CHANNEL(p) + 1,
        (p & PACK_OCCUPIED) ? "occupied" : "vacated", (unsigned)PACK_ADDRESS(p),
        type_name[PACK_TYPE(p)], orient_name[PACK_ORIENT(p)]);
}

static void signal_in_lcc_task(uint32_t p)
{
    DBG("RailCom T%d: signal %s\n", (int)(p & 0xFF) + 1, (p >> 8) ? "active" : "lost");
}

void railcom_on_data(int channel, railcom_data_t *data)
{
    uint32_t p = (uint32_t)channel | (data->occupied ? PACK_OCCUPIED : 0) |
                 ((uint32_t)data->addr_type << 3) | ((uint32_t)data->orientation << 5) |
                 ((uint32_t)data->address << 16);
    lcc_task_call(data_in_lcc_task, p);
}

void railcom_on_signal(int channel, bool active)
{
    lcc_task_call(signal_in_lcc_task, (uint32_t)channel | (active ? 0x100u : 0));
}
