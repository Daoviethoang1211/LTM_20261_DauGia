#include "frame.h"
#include <arpa/inet.h>
#include <string.h>
uint32_t frame_length(const unsigned char *header) {
    uint32_t n;
    memcpy(&n, header, sizeof n);
    return ntohl(n);
}
void frame_header(unsigned char *header, uint32_t length) {
    uint32_t n = htonl(length);
    memcpy(header, &n, sizeof n);
}
