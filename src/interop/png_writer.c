/** @file png_writer.c @brief Minimal stored-deflate PNG encoder. */
#include "png_writer.h"

#include <stdbool.h>
#include <stdint.h>

static const uint32_t SupportedDimensions[PngWriterSupportedDimensionCount] = {
    16u, 24u, 32u, 48u, 64u, 128u, 256u
};

static bool size_add(size_t left, size_t right, size_t *result)
{
    if (left > SIZE_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool size_multiply(size_t left, size_t right, size_t *result)
{
    if (left > SIZE_MAX / right) {
        return false;
    }
    *result = left * right;
    return true;
}

static bool supported_dimension(uint32_t dimension)
{
    for (size_t index = 0u; index < PngWriterSupportedDimensionCount; ++index) {
        if (SupportedDimensions[index] == dimension) {
            return true;
        }
    }
    return false;
}

static bool range_valid(const void *pointer, size_t length, uintptr_t *start, uintptr_t *end)
{
    const uintptr_t value = (uintptr_t)pointer;
    if (value > UINTPTR_MAX - length) {
        return false;
    }
    *start = value;
    *end = value + length;
    return true;
}

static bool ranges_overlap(uintptr_t first_start,
                           uintptr_t first_end,
                           uintptr_t second_start,
                           uintptr_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

static void write_be32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24u);
    output[1] = (uint8_t)(value >> 16u);
    output[2] = (uint8_t)(value >> 8u);
    output[3] = (uint8_t)value;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *bytes, size_t length)
{
    for (size_t index = 0u; index < length; ++index) {
        crc ^= bytes[index];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1u) ^ (0xedb88320u & mask);
        }
    }
    return crc;
}

static uint32_t adler32_update(uint32_t adler, const uint8_t *bytes, size_t length)
{
    uint32_t first = adler & 0xffffu;
    uint32_t second = (adler >> 16u) & 0xffffu;
    for (size_t index = 0u; index < length; ++index) {
        first = (first + bytes[index]) % 65521u;
        second = (second + first) % 65521u;
    }
    return (second << 16u) | first;
}

static size_t write_chunk(uint8_t *output,
                          const uint8_t type[4],
                          const uint8_t *data,
                          uint32_t length)
{
    write_be32(output, length);
    output[4] = type[0];
    output[5] = type[1];
    output[6] = type[2];
    output[7] = type[3];
    for (uint32_t index = 0u; index < length; ++index) {
        output[8u + index] = data[index];
    }
    uint32_t crc = crc32_update(0xffffffffu, type, 4u);
    crc = crc32_update(crc, data, length) ^ 0xffffffffu;
    write_be32(output + 8u + length, crc);
    return (size_t)length + 12u;
}

png_writer_status_t png_writer_encoded_size(uint32_t dimension, size_t *size_out)
{
    if (size_out == NULL || ((uintptr_t)size_out % _Alignof(size_t)) != 0u ||
        (uintptr_t)size_out > UINTPTR_MAX - sizeof(*size_out)) {
        return PngWriterStatusInvalidArgument;
    }
    if (!supported_dimension(dimension)) {
        return PngWriterStatusUnsupportedDimension;
    }
    size_t row_bytes = 0u;
    size_t raw_size = 0u;
    size_t zlib_size = 0u;
    size_t png_size = 0u;
    /* The frozen maximum dimension proves these products fit size_t. */
    row_bytes = (size_t)dimension * 4u + 1u;
    raw_size = row_bytes * (size_t)dimension;
    const size_t block_count = (raw_size + 65534u) / 65535u;
    zlib_size = raw_size + block_count * 5u + 6u;
    png_size = zlib_size + 57u;
    *size_out = png_size;
    return PngWriterStatusOk;
}

png_writer_status_t png_writer_encode_rgba8(const uint8_t *rgba,
                                             size_t stride,
                                             uint32_t dimension,
                                             uint8_t *output,
                                             size_t output_capacity,
                                             size_t *written_out)
{
    if (rgba == NULL || output == NULL || written_out == NULL ||
        ((uintptr_t)written_out % _Alignof(size_t)) != 0u) {
        return PngWriterStatusInvalidArgument;
    }
    if (!supported_dimension(dimension)) {
        return PngWriterStatusUnsupportedDimension;
    }

    size_t row_bytes = 0u;
    size_t raw_size = 0u;
    size_t source_extent = 0u;
    row_bytes = (size_t)dimension * 4u;
    raw_size = (row_bytes + 1u) * (size_t)dimension;
    if (stride < row_bytes) {
        return PngWriterStatusInvalidArgument;
    }
    if (!size_multiply(stride, (size_t)(dimension - 1u), &source_extent) ||
        !size_add(source_extent, row_bytes, &source_extent)) {
        return PngWriterStatusOverflow;
    }
    const size_t block_count = (raw_size + 65534u) / 65535u;
    const size_t required_size = 57u + raw_size + block_count * 5u + 6u;
    if (output_capacity < required_size) {
        return PngWriterStatusInsufficientCapacity;
    }

    uintptr_t input_start = 0u;
    uintptr_t input_end = 0u;
    uintptr_t output_start = 0u;
    uintptr_t output_end = 0u;
    uintptr_t written_start = 0u;
    uintptr_t written_end = 0u;
    if (!range_valid(rgba, source_extent, &input_start, &input_end) ||
        !range_valid(output, output_capacity, &output_start, &output_end) ||
        !range_valid(written_out, sizeof(*written_out), &written_start, &written_end) ||
        ranges_overlap(input_start, input_end, output_start, output_end) ||
        ranges_overlap(input_start, input_end, written_start, written_end) ||
        ranges_overlap(output_start, output_end, written_start, written_end)) {
        return PngWriterStatusInvalidArgument;
    }

    static const uint8_t Signature[8] = {137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u};
    static const uint8_t IhdrType[4] = {'I', 'H', 'D', 'R'};
    static const uint8_t IdatType[4] = {'I', 'D', 'A', 'T'};
    static const uint8_t IendType[4] = {'I', 'E', 'N', 'D'};
    size_t position = 0u;
    for (size_t index = 0u; index < sizeof(Signature); ++index) {
        output[position++] = Signature[index];
    }

    uint8_t ihdr[13] = {0};
    write_be32(ihdr, dimension);
    write_be32(ihdr + 4u, dimension);
    ihdr[8] = 8u;
    ihdr[9] = 6u;
    position += write_chunk(output + position, IhdrType, ihdr, sizeof(ihdr));

    const size_t idat_length_size = 2u + raw_size + block_count * 5u + 4u;
    const uint32_t idat_length = (uint32_t)idat_length_size;
    output[position++] = (uint8_t)(idat_length >> 24u);
    output[position++] = (uint8_t)(idat_length >> 16u);
    output[position++] = (uint8_t)(idat_length >> 8u);
    output[position++] = (uint8_t)idat_length;
    const size_t idat_start = position;
    for (size_t index = 0u; index < sizeof(IdatType); ++index) {
        output[position++] = IdatType[index];
    }
    const size_t zlib_start = position;
    output[position++] = 0x78u;
    output[position++] = 0x01u;

    uint32_t adler = 1u;
    size_t raw_position = 0u;
    size_t block_index = 0u;
    while (raw_position < raw_size) {
        const size_t remaining = raw_size - raw_position;
        const uint16_t block_length = (uint16_t)(remaining > 65535u ? 65535u : remaining);
        output[position++] = (uint8_t)(block_index + 1u == block_count ? 1u : 0u);
        output[position++] = (uint8_t)block_length;
        output[position++] = (uint8_t)(block_length >> 8u);
        const uint16_t inverse_length = (uint16_t)~block_length;
        output[position++] = (uint8_t)inverse_length;
        output[position++] = (uint8_t)(inverse_length >> 8u);

        size_t block_written = 0u;
        while (block_written < block_length) {
            const size_t scanline_offset = raw_position;
            const size_t row = scanline_offset / (row_bytes + 1u);
            const size_t column = scanline_offset % (row_bytes + 1u);
            uint8_t byte = column == 0u ? 0u : rgba[row * stride + column - 1u];
            output[position] = byte;
            adler = adler32_update(adler, &output[position], 1u);
            ++position;
            ++raw_position;
            ++block_written;
        }
        ++block_index;
    }
    write_be32(output + position, adler);
    position += 4u;
    const uint32_t idat_crc = crc32_update(0xffffffffu, output + idat_start,
                                          sizeof(IdatType) + idat_length_size) ^ 0xffffffffu;
    write_be32(output + position, idat_crc);
    position += 4u;
    (void)zlib_start;

    position += write_chunk(output + position, IendType, NULL, 0u);
    *written_out = position;
    return PngWriterStatusOk;
}
