// notify.h — on-screen system toast (survives even if the app crashes right
// after), used both for user feedback and as a crash localizer.
#ifndef PS4CAST_NOTIFY_H
#define PS4CAST_NOTIFY_H

// Always shown. Reserve for things the user must see: ready, hard failures.
void notify(const char *fmt, ...);

// Diagnostic toast: only shown when debug mode is enabled (Settings -> Debug
// notifications). Default OFF for a clean, final-product TV display.
void notify_dbg(const char *fmt, ...);

void notify_set_debug(int on);
int  notify_get_debug(void);

#endif
