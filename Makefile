# Makefile for canbus-outpost
# Delegates to Ninja for faster builds

BUILD_DIR ?= build

.PHONY: all clean flash test

all: $(BUILD_DIR)/CMakeCache.txt
	@ninja -C $(BUILD_DIR)

$(BUILD_DIR)/CMakeCache.txt:
	@mkdir -p $(BUILD_DIR)
	@cmake -G Ninja -B $(BUILD_DIR) .

clean:
	@rm -rf $(BUILD_DIR)

flash: all
	@picotool load -x $(BUILD_DIR)/canbus-outpost.elf

# Host-side tests (LCC library and application code); no hardware needed
test:
	@$(MAKE) --no-print-directory -C lib/lcc/test
	@$(MAKE) --no-print-directory -C test
