# Host tests for the firmware's application code: `make` builds and runs them.
CC     ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O1 -g
SRC    := ../src

TESTS := test_cdi_layout test_railcom test_fw_image test_railcom_app test_signals
LCC    := ../lib/lcc

.PHONY: run clean
run: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_cdi_layout: test_cdi_layout.c $(SRC)/app/cdi_data.c $(SRC)/app/config_layout.h
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_cdi_layout.c $(SRC)/app/cdi_data.c

# railcom.c is #included so the test can reach its internals; mock/ stands
# in for the SDK and FreeRTOS headers.
test_railcom: test_railcom.c $(SRC)/railcom.c $(SRC)/railcom.h $(wildcard mock/*.h mock/*/*.h)
	$(CC) $(CFLAGS) -Wno-unused-parameter -Imock -I$(SRC) -o $@ test_railcom.c -lm

test_fw_image: test_fw_image.c $(SRC)/fw_image.c $(SRC)/fw_image.h
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_fw_image.c $(SRC)/fw_image.c

# railcom_app.c on the real LCC library; LCC task calls run immediately
test_railcom_app: test_railcom_app.c $(SRC)/app/railcom_app.c $(SRC)/app/railcom_app.h \
                  $(wildcard $(LCC)/src/*.c $(LCC)/src/*.h $(LCC)/include/lcc/*.h)
	$(CC) $(CFLAGS) -Imock -I$(SRC) -I$(LCC)/include -o $@ test_railcom_app.c \
	    $(SRC)/app/railcom_app.c $(wildcard $(LCC)/src/*.c)

# signals.c on the real LCC library, with the WS2811 driver recorded
test_signals: test_signals.c $(SRC)/app/signals.c $(SRC)/app/signals.h $(SRC)/app/config_layout.h \
              $(wildcard mock/*.h) $(wildcard $(LCC)/src/*.c $(LCC)/src/*.h $(LCC)/include/lcc/*.h)
	$(CC) $(CFLAGS) -Imock -I$(SRC) -I$(LCC)/include -o $@ test_signals.c \
	    $(SRC)/app/signals.c $(wildcard $(LCC)/src/*.c)

clean:
	rm -f $(TESTS)
