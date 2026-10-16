# Makefile for canbus-outpost
# Delegates to Ninja for faster builds

BUILD_DIR ?= build

.PHONY: all clean flash partition-table test

all: $(BUILD_DIR)/CMakeCache.txt
	@ninja -C $(BUILD_DIR)

$(BUILD_DIR)/CMakeCache.txt:
	@mkdir -p $(BUILD_DIR)
	@cmake -G Ninja -B $(BUILD_DIR) .

clean:
	@rm -rf $(BUILD_DIR)

flash: all
	@picotool load -x $(BUILD_DIR)/canbus-outpost.elf

# Once per board, in BOOTSEL mode: replace everything but the configuration
# and node ID at the top of flash with the A/B partition table LCC firmware
# updates need (partition_table.json), then load the firmware. After this,
# `make flash` loads into whichever partition isn't running.
partition-table: all
	@picotool partition create partition_table.json $(BUILD_DIR)/partition_table.uf2
	@picotool erase -r 0x10000000 0x103FC000
	@picotool load $(BUILD_DIR)/partition_table.uf2
	@picotool reboot -u
	@for i in 1 2 3 4 5 6 7 8 9 10; do picotool info >/dev/null 2>&1 && break; sleep 1; done
	@picotool load -x $(BUILD_DIR)/canbus-outpost.elf

# Host-side tests (LCC library and application code); no hardware needed
test:
	@$(MAKE) --no-print-directory -C lib/lcc/test
	@$(MAKE) --no-print-directory -C test
