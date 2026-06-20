// goldhen.h - minimal GoldHEN SDK syscall probe.
#ifndef PS4CAST_GOLDHEN_H
#define PS4CAST_GOLDHEN_H

int goldhen_probe(char *out, int len);
int goldhen_enter(char *out, int len);
int goldhen_restore(char *out, int len);
const char *goldhen_status(void);

#endif
