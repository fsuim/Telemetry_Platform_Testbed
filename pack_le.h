#ifndef PACK_LE_H
#define PACK_LE_H

#include <stdint.h>

static inline int16_t clamp_i16(long v){
    if(v > 32767) return 32767;
    if(v < -32768) return -32768;
    return (int16_t)v;
}

static inline void pack_i16_le(uint8_t *buf, int16_t v){
    uint16_t u = (uint16_t)v;
    buf[0] = (uint8_t)(u & 0xFF);
    buf[1] = (uint8_t)((u >> 8) & 0xFF);
}

static inline void pack_u16_le(uint8_t *buf, uint16_t v){
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void pack_u32_le(uint8_t *buf, uint32_t v){
    buf[0] = (uint8_t)(v & 0xFFu);
    buf[1] = (uint8_t)((v >> 8) & 0xFFu);
    buf[2] = (uint8_t)((v >> 16) & 0xFFu);
    buf[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void pack_i32_le(uint8_t *buf, int32_t v){
    pack_u32_le(buf, (uint32_t)v);
}

#endif