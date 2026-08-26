#include <ApplicationServices/ApplicationServices.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: macos-send-key PID KEYCODE\n");
        return 2;
    }

    char *pid_end = NULL;
    char *key_end = NULL;
    errno = 0;
    long pid_value = strtol(argv[1], &pid_end, 10);
    long key_value = strtol(argv[2], &key_end, 10);
    if (errno || !pid_end || *pid_end || !key_end || *key_end ||
        pid_value <= 0 || key_value < 0 || key_value > UINT16_MAX) {
        fprintf(stderr, "invalid PID or keycode\n");
        return 2;
    }

    CGEventRef down = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)key_value, true);
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)key_value, false);
    if (!down || !up) {
        if (down) CFRelease(down);
        if (up) CFRelease(up);
        return 1;
    }

    CGEventPostToPid((pid_t)pid_value, down);
    usleep(80000);
    CGEventPostToPid((pid_t)pid_value, up);
    CFRelease(down);
    CFRelease(up);
    return 0;
}
