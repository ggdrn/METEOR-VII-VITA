/* Native submarine functions; see ff7_submarine.c. */
#ifndef FF7_SUBMARINE_H
#define FF7_SUBMARINE_H

void ff7_submarine_patch(void);
void ff7_submarine_input_patch(void);
int ff7_submarine_input_key(int down);
void ff7_submarine_input_poll(void);
void ff7_submarine_input_report(void);
void ff7_submarine_report(double seconds);
// Game thread, between frames (port menu A/B).
void ff7_submarine_set(int on);

#endif
