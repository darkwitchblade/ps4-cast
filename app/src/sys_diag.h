#ifndef PS4CAST_SYS_DIAG_H
#define PS4CAST_SYS_DIAG_H

// Instrumentation for the SceShellUI-crash investigation (CE-36329-3).
//
// The crash is SceShellUI dying on an Invalid User Id (ANONYMOUS 0xffffffff)
// inside its VideoPlayingChecker while OUR process stays healthy. None of our
// own metrics (frames/cur/flips) reveal it. This module samples the two
// system-side signals that might: the foreground/initial user id, and the
// SystemService status (isSystemUiOverlaid / isInBackgroundExecution / eventNum).
// Exposed via /status as "sys" so we can correlate against the klog.

void sys_diag_update(void);        // sample once (call ~1 Hz from the main loop)
const char *sys_diag_get(void);    // latest one-line snapshot for /status

// Parsed accessors for a future fail-closed watchdog (filled by sys_diag_update).
int sys_fg_user(void);             // foreground user id (-999 if query failed)
int sys_ui_overlaid(void);         // 1 = a system UI is overlaid on our app
int sys_in_background(void);       // 1 = our app is in background execution

#endif
