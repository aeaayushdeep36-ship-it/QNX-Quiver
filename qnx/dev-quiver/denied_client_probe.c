#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define QUIVER_DEVICE_PATH "/dev/quiver"

int main(int argc, char **argv)
{
    const char *path = QUIVER_DEVICE_PATH;
    if (argc > 2) {
        fprintf(stderr, "usage: %s [device-path]\n", argv[0]);
        return 2;
    }
    if (argc == 2) path = argv[1];

    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        (void)close(fd);
        fprintf(stderr, "FAIL: policy allowed an unpermitted open of %s\n", path);
        return 1;
    }
    if (errno != EACCES && errno != EPERM) {
        fprintf(stderr, "FAIL: open of %s failed for an unexpected reason: %s (%d)\n",
                path, strerror(errno), errno);
        return 1;
    }

    printf("PASS: policy denied open of %s (%s)\n", path, strerror(errno));
    return 0;
}
