
#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

int main(void) {
    int fd0 = open("/dev/hello_dev0", O_RDWR);
    int fd1 = open("/dev/hello_dev1", O_RDWR);

    struct pollfd fds[2] = {
        {fd0, POLLIN, 0},
        {fd1, POLLIN, 0}
    };
    printf("polling 2 devices, waiting...\n");

    int ret = poll(fds, 2, -1);
    if (ret < 0) {
        perror("poll");
        return 1;
    }

    for (int i = 0; i < 2; ++i) {
        if (fds[i].revents & POLLIN) {
            char buf[128] = {0};
            read(fds[i].fd, buf, sizeof(buf));
            printf("device %d readable: %s\n", i, buf);
        }
    }

    return 0;
}