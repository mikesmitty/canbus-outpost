#include "openlcb_user_config.h"
#include "openlcb/openlcb_types.h"
#include "openlcb/openlcb_defines.h"
#include "cdi_data.c"

const node_parameters_t OpenLcbUserConfig_node_parameters = {

    .snip.mfg_version = 4,
    .snip.name = "simple-dcc",
    .snip.model = "DCC Command Station",
    .snip.hardware_version = "1.0.0",
    // x-release-please-start-version
    .snip.software_version = "0.3.0",
    // x-release-please-end
    .snip.user_version = 2,

    .protocol_support = (PSI_DATAGRAM |
                         PSI_MEMORY_CONFIGURATION |
                         PSI_EVENT_EXCHANGE |
                         PSI_SIMPLE_NODE_INFORMATION |
                         PSI_TRAIN_CONTROL |
                         PSI_FUNCTION_DESCRIPTION |
                         PSI_CONFIGURATION_DESCRIPTION_INFO),

    .consumer_count_autocreate = 4,
    .producer_count_autocreate = 0,

    .configuration_options.high_address_space = CONFIG_MEM_SPACE_CONFIGURATION_DEFINITION_INFO,
    .configuration_options.low_address_space = CONFIG_MEM_SPACE_CONFIGURATION_MEMORY,
    .configuration_options.read_from_manufacturer_space_0xfc_supported = true,
    .configuration_options.read_from_user_space_0xfb_supported = true,
    .configuration_options.stream_read_write_supported = false,
    .configuration_options.unaligned_reads_supported = true,
    .configuration_options.unaligned_writes_supported = true,
    .configuration_options.write_to_user_space_0xfb_supported = true,
    .configuration_options.write_under_mask_supported = false,
    .configuration_options.description = "",

    .address_space_configuration_definition.read_only = true,
    .address_space_configuration_definition.present = true,
    .address_space_configuration_definition.low_address_valid = false,
    .address_space_configuration_definition.low_address = 0,
    .address_space_configuration_definition.highest_address = sizeof(_cdi_data) - 2,
    .address_space_configuration_definition.address_space = CONFIG_MEM_SPACE_CONFIGURATION_DEFINITION_INFO,
    .address_space_configuration_definition.description = "",

    .address_space_all.read_only = true,
    .address_space_all.present = true,
    .address_space_all.low_address_valid = false,
    .address_space_all.low_address = 0,
    .address_space_all.highest_address = 0xFFFF,
    .address_space_all.address_space = CONFIG_MEM_SPACE_ALL,
    .address_space_all.description = "",

    .address_space_config_memory.read_only = false,
    .address_space_config_memory.present = true,
    .address_space_config_memory.low_address_valid = false,
    .address_space_config_memory.low_address = 0,
    .address_space_config_memory.highest_address = 0x01FF,
    .address_space_config_memory.address_space = CONFIG_MEM_SPACE_CONFIGURATION_MEMORY,
    .address_space_config_memory.description = "",

    .address_space_acdi_manufacturer.read_only = true,
    .address_space_acdi_manufacturer.present = true,
    .address_space_acdi_manufacturer.low_address_valid = false,
    .address_space_acdi_manufacturer.low_address = 0,
    .address_space_acdi_manufacturer.highest_address = 124,
    .address_space_acdi_manufacturer.address_space = CONFIG_MEM_SPACE_ACDI_MANUFACTURER_ACCESS,
    .address_space_acdi_manufacturer.description = "",

    .address_space_acdi_user.read_only = false,
    .address_space_acdi_user.present = true,
    .address_space_acdi_user.low_address_valid = false,
    .address_space_acdi_user.low_address = 0,
    .address_space_acdi_user.highest_address = 127,
    .address_space_acdi_user.address_space = CONFIG_MEM_SPACE_ACDI_USER_ACCESS,
    .address_space_acdi_user.description = "",

    .address_space_train_function_definition_info.read_only = true,
    .address_space_train_function_definition_info.present = true,
    .address_space_train_function_definition_info.low_address_valid = false,
    .address_space_train_function_definition_info.low_address = 0,
    .address_space_train_function_definition_info.highest_address = 0,
    .address_space_train_function_definition_info.address_space = CONFIG_MEM_SPACE_TRAIN_FUNCTION_DEFINITION_INFO,
    .address_space_train_function_definition_info.description = "",

    .address_space_train_function_config_memory.read_only = false,
    .address_space_train_function_config_memory.present = true,
    .address_space_train_function_config_memory.low_address_valid = false,
    .address_space_train_function_config_memory.low_address = 0,
    .address_space_train_function_config_memory.highest_address = 0,
    .address_space_train_function_config_memory.address_space = CONFIG_MEM_SPACE_TRAIN_FUNCTION_CONFIGURATION_MEMORY,
    .address_space_train_function_config_memory.description = "",

    .address_space_firmware.read_only = false,
    .address_space_firmware.present = false,
    .address_space_firmware.low_address_valid = false,
    .address_space_firmware.low_address = 0,
    .address_space_firmware.highest_address = 0,
    .address_space_firmware.address_space = CONFIG_MEM_SPACE_FIRMWARE,
    .address_space_firmware.description = "",

    .cdi = _cdi_data,
    .fdi = NULL,
};

// Train nodes: same as CS but no auto-created events (faster init, less bus traffic)
const node_parameters_t OpenLcbUserConfig_train_node_parameters = {

    .snip.mfg_version = 4,
    .snip.name = "simple-dcc",
    .snip.model = "DCC Train Node",
    .snip.hardware_version = "1.0.0",
    // x-release-please-start-version
    .snip.software_version = "0.3.0",
    // x-release-please-end
    .snip.user_version = 2,

    .protocol_support = (PSI_DATAGRAM |
                         PSI_MEMORY_CONFIGURATION |
                         PSI_EVENT_EXCHANGE |
                         PSI_SIMPLE_NODE_INFORMATION |
                         PSI_TRAIN_CONTROL |
                         PSI_FUNCTION_DESCRIPTION |
                         PSI_CONFIGURATION_DESCRIPTION_INFO),

    .consumer_count_autocreate = 0,
    .producer_count_autocreate = 0,

    .configuration_options.high_address_space = CONFIG_MEM_SPACE_CONFIGURATION_DEFINITION_INFO,
    .configuration_options.low_address_space = CONFIG_MEM_SPACE_CONFIGURATION_MEMORY,
    .configuration_options.read_from_manufacturer_space_0xfc_supported = true,
    .configuration_options.read_from_user_space_0xfb_supported = true,
    .configuration_options.stream_read_write_supported = false,
    .configuration_options.unaligned_reads_supported = true,
    .configuration_options.unaligned_writes_supported = true,
    .configuration_options.write_to_user_space_0xfb_supported = true,
    .configuration_options.write_under_mask_supported = false,
    .configuration_options.description = "",

    .address_space_configuration_definition.read_only = true,
    .address_space_configuration_definition.present = true,
    .address_space_configuration_definition.low_address_valid = false,
    .address_space_configuration_definition.low_address = 0,
    .address_space_configuration_definition.highest_address = 0,
    .address_space_configuration_definition.address_space = CONFIG_MEM_SPACE_CONFIGURATION_DEFINITION_INFO,
    .address_space_configuration_definition.description = "",

    .address_space_all.read_only = true,
    .address_space_all.present = true,
    .address_space_all.low_address_valid = false,
    .address_space_all.low_address = 0,
    .address_space_all.highest_address = 0xFFFF,
    .address_space_all.address_space = CONFIG_MEM_SPACE_ALL,
    .address_space_all.description = "",

    .address_space_config_memory.read_only = false,
    .address_space_config_memory.present = true,
    .address_space_config_memory.low_address_valid = false,
    .address_space_config_memory.low_address = 0,
    .address_space_config_memory.highest_address = 0x01FF,
    .address_space_config_memory.address_space = CONFIG_MEM_SPACE_CONFIGURATION_MEMORY,
    .address_space_config_memory.description = "",

    .address_space_acdi_manufacturer.read_only = true,
    .address_space_acdi_manufacturer.present = true,
    .address_space_acdi_manufacturer.low_address_valid = false,
    .address_space_acdi_manufacturer.low_address = 0,
    .address_space_acdi_manufacturer.highest_address = 124,
    .address_space_acdi_manufacturer.address_space = CONFIG_MEM_SPACE_ACDI_MANUFACTURER_ACCESS,
    .address_space_acdi_manufacturer.description = "",

    .address_space_acdi_user.read_only = false,
    .address_space_acdi_user.present = true,
    .address_space_acdi_user.low_address_valid = false,
    .address_space_acdi_user.low_address = 0,
    .address_space_acdi_user.highest_address = 127,
    .address_space_acdi_user.address_space = CONFIG_MEM_SPACE_ACDI_USER_ACCESS,
    .address_space_acdi_user.description = "",

    .address_space_train_function_definition_info.read_only = true,
    .address_space_train_function_definition_info.present = true,
    .address_space_train_function_definition_info.low_address_valid = false,
    .address_space_train_function_definition_info.low_address = 0,
    .address_space_train_function_definition_info.highest_address = 0,
    .address_space_train_function_definition_info.address_space = CONFIG_MEM_SPACE_TRAIN_FUNCTION_DEFINITION_INFO,
    .address_space_train_function_definition_info.description = "",

    .address_space_train_function_config_memory.read_only = false,
    .address_space_train_function_config_memory.present = true,
    .address_space_train_function_config_memory.low_address_valid = false,
    .address_space_train_function_config_memory.low_address = 0,
    .address_space_train_function_config_memory.highest_address = 0,
    .address_space_train_function_config_memory.address_space = CONFIG_MEM_SPACE_TRAIN_FUNCTION_CONFIGURATION_MEMORY,
    .address_space_train_function_config_memory.description = "",

    .address_space_firmware.read_only = false,
    .address_space_firmware.present = false,
    .address_space_firmware.low_address_valid = false,
    .address_space_firmware.low_address = 0,
    .address_space_firmware.highest_address = 0,
    .address_space_firmware.address_space = CONFIG_MEM_SPACE_FIRMWARE,
    .address_space_firmware.description = "",

    .cdi = NULL,
    .fdi = NULL,
};
