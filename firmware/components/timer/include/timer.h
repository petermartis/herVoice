#pragma once
/* Kitchen timer: device-local countdown driven by FRAME_TIMER_SET/CANCEL.
 * Owns the display (via ui_timer_display) and the end-of-timer triple beep,
 * so it keeps working even if the backend/network disappears mid-timer. */
void timer_start(int seconds);
void timer_cancel(void);
int  timer_remaining(void);   /* >0 counting, 0 idle/expired-shown */
