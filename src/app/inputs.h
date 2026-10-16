#ifndef INPUTS_H
#define INPUTS_H

// Fascia push buttons on GPIO_IN1-4 (active low). An edge starts a
// debounce timer; the settled level is handled in the LCC task.

// Before the scheduler starts. on_press(input 1-4) runs in the LCC task.
void inputs_init(void (*on_press)(int input));

// Enable the edge interrupts (from the LCC task).
void inputs_start(void);

#endif // INPUTS_H
