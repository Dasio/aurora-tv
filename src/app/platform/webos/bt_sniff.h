#pragma once

/**
 * Holds Bluetooth gamepads out of Bluetooth sniff mode while streaming.
 *
 * In sniff, the TV's stack delivers a pad's input reports in ~77 ms batches (about 10/s), so
 * presses wait for the next batch and short taps arrive as press+release together. The stack
 * drifts back into sniff within seconds of a `stopSniff`, so a background thread re-asserts it
 * every 250 ms for every Bluetooth joystick, and gives the links back to the TV's own sniff
 * policy when the stream ends (punktfunk `pad_link`).
 */
typedef struct webos_bt_sniff_state webos_bt_sniff_state_t;

webos_bt_sniff_state_t *webos_bt_sniff_enter(void);

/** Non-blocking: the thread restores sniff and frees itself. */
void webos_bt_sniff_leave(webos_bt_sniff_state_t *state);
