// notify.h — on-screen system toast (survives even if the app crashes right
// after), used both for user feedback and as a crash localizer.
#ifndef PS4CAST_NOTIFY_H
#define PS4CAST_NOTIFY_H

void notify(const char *fmt, ...);

#endif
