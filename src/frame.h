#ifndef FRAME_H
#define FRAME_H
#include <stddef.h>
#include <stdint.h>
#define FRAME_MAX 65536u
uint32_t frame_length(const unsigned char *header);
void frame_header(unsigned char *header, uint32_t length);
#endif
