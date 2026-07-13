#include "clock_protocol_stream.h"

#include <string.h>

static bool is_keep_alive(const clock_protocol_stream_t *stream)
{
    return stream->length == 3 &&
           stream->buffer[0] == 0 &&
           stream->buffer[1] == 0 &&
           stream->buffer[2] == 0;
}

static size_t fixed_frame_length(const clock_protocol_stream_t *stream)
{
    if (stream->length < 6 ||
        stream->buffer[0] != '/' ||
        stream->buffer[1] != 'T' ||
        stream->buffer[2] != 'A') {
        return 0;
    }

    uint8_t command_1 = stream->buffer[4];
    uint8_t command_2 = stream->buffer[5];

    if (command_1 == 'C' && command_2 == 'T') {
        return 9;
    }

    if ((command_1 == 'E' && command_2 == 'S') ||
        (command_1 == 'R' && command_2 == 'C') ||
        (command_1 == 'D' && command_2 == 'L') ||
        (command_1 == 'N' && command_2 == 'M')) {
        return 7;
    }

    if (command_1 == 'U' && command_2 == 'C') {
        return 14;
    }

    if (command_1 == 'C' && command_2 == 'A') {
        return 12;
    }

    if ((command_1 == 'D' && command_2 == 'A') ||
        (command_1 == 'L' && command_2 == 'A') ||
        (command_1 == 'R' && command_2 == 'T')) {
        return 8;
    }

    if (command_1 == 'H' && command_2 == 'B') {
        return 9;
    }

    if (command_1 == 'S' && command_2 == 'M') {
        return 9;
    }

    if (command_1 == 'R' && command_2 == 'M') {
        return 7;
    }

    return 0;
}

void clock_protocol_stream_init(clock_protocol_stream_t *stream)
{
    if (stream == nullptr) {
        return;
    }

    memset(stream, 0, sizeof(*stream));
}

clock_protocol_stream_result_t clock_protocol_stream_consume(
    clock_protocol_stream_t *stream,
    const uint8_t *data,
    size_t length,
    clock_protocol_frame_callback_t callback,
    void *context)
{
    clock_protocol_stream_result_t result = {};

    if (stream == nullptr || data == nullptr || length == 0) {
        return result;
    }

    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = data[i];

        if (stream->discarding_overflow) {
            if (byte == '\\') {
                stream->discarding_overflow = false;
            }

            continue;
        }

        if (stream->length >= CLOCK_PROTOCOL_STREAM_CAPACITY) {
            stream->length = 0;
            stream->discarding_overflow = byte != '\\';
            result.overflowed = true;
            continue;
        }

        stream->buffer[stream->length++] = byte;

        size_t expected_fixed_length = fixed_frame_length(stream);
        bool fixed_frame_complete =
            expected_fixed_length > 0 &&
            stream->length >= expected_fixed_length;
        bool delimiter_frame_complete =
            byte == '\\' &&
            (expected_fixed_length == 0 || fixed_frame_complete);

        if (delimiter_frame_complete || is_keep_alive(stream)) {
            if (callback != nullptr) {
                callback(stream->buffer, stream->length, context);
            }

            result.frames_dispatched++;
            stream->length = 0;
        }
    }

    return result;
}
