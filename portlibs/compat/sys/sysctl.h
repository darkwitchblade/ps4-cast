#pragma once
#include <stddef.h>
#define CTL_HW 6
#define HW_NCPU 3
int sysctl(int *, unsigned int, void *, size_t *, const void *, size_t);
int sysctlbyname(const char *, void *, size_t *, const void *, size_t);
