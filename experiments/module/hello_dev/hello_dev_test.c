
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <unistd.h>   // close()

#define HELLO_MAGIC 'H'
#define HELLO_RESET _IO(HELLO_MAGIC, 0)


int main(void) {
    int fd = open("/dev/hello_dev", O_RDWR);
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