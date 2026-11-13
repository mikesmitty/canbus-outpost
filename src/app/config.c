#include "config.h"
#include <string.h>
#include "nv_storage.h"
#include "util/dbg.h"

_Static_assert(sizeof(config_layout_t) <= NV_MAX_DATA(NV_CONFIG_SECTORS),
               "the configuration must fit its flash record");

config_layout_t config;

// Where the configuration is saved: its region, or on a board whose
// partition table predates that region, where earlier firmware kept it
static const nv_region_t *region = &nv_old_config_region;

size_t config_load(void)
{
    size_t stored_len;
    uint16_t version;

    if (nv_region_outside_partitions(&nv_config_region)) {
        region = &nv_config_region;
    } else {
        region = &nv_old_config_region;
        DBG("config: partition table predates the 64 KB storage area, saving up to %u bytes "
            "until the new one is in use\n",
            (unsigned)NV_MAX_DATA(nv_old_config_region.slot_sectors));
    }

    memset(&config, 0, sizeof(config));
    bool found = nv_load(region, &config, sizeof(config), &stored_len, &version);
    if (!found && region == &nv_config_region) {
        // Saved by earlier firmware: the next save moves it across
        found = nv_load(&nv_old_config_region, &config, sizeof(config), &stored_len, &version);
        if (found)
            DBG("config: moving from the old storage area\n");
    }
    if (!found) {
        DBG("config: none saved\n");
        return 0;
    }
    if (version != CONFIG_VERSION) {
        DBG("config: saved version %u, expected %u\n", version, CONFIG_VERSION);
        memset(&config, 0, sizeof(config));
        return 0;
    }
    return stored_len < sizeof(config) ? stored_len : sizeof(config);
}

bool config_save(void)
{
    bool ok = nv_save(region, &config, sizeof(config), CONFIG_VERSION);
    DBG("config: %s\n", ok ? "saved" : "save failed");
    return ok;
}
