// escalate.h — one-time privilege escalation at startup via a user-provided,
// firmware-matched jb.prx exporting jailbreak_me(). Unlocks the hardware
// decoder, system-module loading, and app launching (all blocked in the
// sandbox). Bundled at /app0/assets/jb.prx.
#ifndef PS4CAST_ESCALATE_H
#define PS4CAST_ESCALATE_H

// Loads jb.prx and calls jailbreak_me(). Returns its result, or a negative
// sentinel if the prx is missing / symbol not found. Safe to call once.
int  jailbreak(void);

// Result of the last jailbreak() call (for /status). 0x7fffffff = not run.
int  jb_result(void);

#endif
