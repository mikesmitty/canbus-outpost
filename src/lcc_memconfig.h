#ifndef LCC_MEMCONFIG_H
#define LCC_MEMCONFIG_H

#include "lcc.h"

/* Memory spaces */
#define LCC_SPACE_CDI     0xFF
#define LCC_SPACE_ALL     0xFE
#define LCC_SPACE_CONFIG  0xFD

/* Size of the CDI config space (0xFD): excludes magic prefix */
#define LCC_CONFIG_SPACE_SIZE  (sizeof(lcc_config_t) - sizeof(uint32_t))

/* Offset within lcc_config_t where CDI space begins (after magic) */
#define LCC_CONFIG_SPACE_OFFSET  sizeof(uint32_t)

/* Process a complete Memory Configuration datagram.
 * buf/len is the full datagram payload (first byte is 0x20).
 * Sends response datagrams back via node queues. */
void lcc_memconfig_handle(lcc_node_t *node, uint16_t src_alias,
                          const uint8_t *buf, uint8_t len);

#endif /* LCC_MEMCONFIG_H */
