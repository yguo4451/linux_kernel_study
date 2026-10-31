
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <unistd.h>   // close()
#include <stdlib.h>

#define HELLO_MAGIC 'H'
#define HELLO_RESET _IO(HELLO_MAGIC, 0)


int main(int argc, char* argv[]) {
    int index = argc > 1 ? atoi(argv[1]) : 0;
    char dev_name[128];
    snprintf(dev_name, 128, "/dev/hello_dev%d", index);
    int fd = open(dev_name, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (ioctl(fd, HELLO_RESET) < 0) {
        perror("ioctl");
        return 1;
    }

    close(fd);
    return 0;
}