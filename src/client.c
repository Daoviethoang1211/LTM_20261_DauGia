#include "frame.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
static int transfer(int fd, unsigned char *buffer, size_t length, int writing) {
    while (length) {
        ssize_t n = writing ? send(fd, buffer, length, MSG_NOSIGNAL) : recv(fd, buffer, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        buffer += n; length -= (size_t)n;
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s HOST_IPV4 PORT 'JSON'\n", argv[0]); return 1; }
    char *end; errno = 0;
    long port = strtol(argv[2], &end, 10);
    size_t length = strlen(argv[3]);
    if (errno || end == argv[2] || *end || port < 1 || port > 65535 || !length || length > FRAME_MAX) return 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) return 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct timeval timeout = {.tv_sec = 5};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    unsigned char buffer[FRAME_MAX + 4];
    frame_header(buffer, (uint32_t)length); memcpy(buffer + 4, argv[3], length);
    int result = 1;
    if (connect(fd, (struct sockaddr *)&address, sizeof address) < 0 || transfer(fd, buffer, length + 4, 1) < 0 || transfer(fd, buffer, 4, 0) < 0) goto done;
    uint32_t response_length = frame_length(buffer);
    if (!response_length || response_length > FRAME_MAX || transfer(fd, buffer, response_length, 0) < 0) goto done;
    fwrite(buffer, 1, response_length, stdout); putchar('\n'); result = 0;
done:
    if (result) fprintf(stderr, "connection or frame transfer failed\n");
    close(fd); return result;
}
