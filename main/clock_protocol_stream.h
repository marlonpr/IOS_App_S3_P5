#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_PROTOCOL_STREAM_CAPACITY 256

typedef struct {
    uint8_t buffer[CLOCK_PROTOCOL_STREAM_CAPACITY];
    size_t length;
    bool discarding_overflow;
} clock_protocol_stream_t;

typedef void (*clock_protocol_frame_callback_t)(const uint8_t *frame,
                                                size_t length,
                                                void *context);

typedef struct {
    size_t frames_dispatched;
    bool overflowed;
} clock_protocol_stream_result_t;

void clock_protocol_stream_init(clock_protocol_stream_t *stream);

clock_protocol_stream_result_t clock_protocol_stream_consume(
    clock_protocol_stream_t *stream,
    const uint8_t *data,
    size_t length,
    clock_protocol_frame_callback_t callback,
    void *context);

#ifdef __cplusplus
}
#endif
