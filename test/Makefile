# Host tests for the firmware's application code: `make` builds and runs them.
CC     ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O1 -g
SRC    := ../src

TESTS := test_cdi_layout test_railcom

.PHONY: run clean
run: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_cdi_layout: test_cdi_layout.c $(SRC)/app/cdi_data.c $(SRC)/app/config_layout.h
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_cdi_layout.c $(SRC)/app/cdi_data.c

# railcom.c is #included so the test can reach its internals; mock/ stands
# in for the SDK and FreeRTOS headers.
test_railcom: test_railcom.c $(SRC)/railcom.c $(SRC)/railcom.h $(wildcard mock/*.h mock/*/*.h)
	$(CC) $(CFLAGS) -Wno-unused-parameter -Imock -I$(SRC) -o $@ test_railcom.c -lm

clean:
	rm -f $(TESTS)
