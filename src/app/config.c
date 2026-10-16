#include "config.h"
#include <string.h>
#include "nv_storage.h"
#include "util/dbg.h"

config_layout_t config;

size_t config_load(void)
{
    size_t stored_len;
    uint16_t version;

    memset(&config, 0, sizeof(config));
    if (!nv_load(&nv_config_region, &config, sizeof(config), &stored_len, &version)) {
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
    bool ok = nv_save(&nv_config_region, &config, sizeof(config), CONFIG_VERSION);
    DBG("config: %s\n", ok ? "saved" : "save failed");
    return ok;
}
