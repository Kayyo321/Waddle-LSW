/** @file png_writer.c @brief Caller-buffer PNG writer unit and golden fixtures. */
#include "src/interop/png_writer.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t TestDimensions[] = {16u, 24u, 32u, 48u, 64u, 128u, 256u};

static void write_fixture(const char *directory, uint32_t dimension)
{
    const size_t stride = (size_t)dimension * 4u + 7u;
    const size_t input_size = stride * (size_t)dimension;
    uint8_t *pixels = malloc(input_size);
    assert(pixels != NULL);
    for (uint32_t row = 0u; row < dimension; ++row) {
        for (uint32_t column = 0u; column < dimension; ++column) {
            const size_t offset = (size_t)row * stride + (size_t)column * 4u;
            pixels[offset] = (uint8_t)(row * 17u + column * 3u);
            pixels[offset + 1u] = (uint8_t)(row * 7u + column * 29u);
            pixels[offset + 2u] = (uint8_t)(row * 13u + column * 11u);
            pixels[offset + 3u] = (uint8_t)((row + column) % 4u == 0u ? 0u :
                                              (row + column) % 4u == 1u ? 85u :
                                              (row + column) % 4u == 2u ? 170u : 255u);
        }
        memset(pixels + (size_t)row * stride + (size_t)dimension * 4u, 0xee, 7u);
    }

    size_t encoded_size = 0u;
    assert(png_writer_encoded_size(dimension, &encoded_size) == PngWriterStatusOk);
    uint8_t *encoded = malloc(encoded_size);
    assert(encoded != NULL);
    memset(encoded, 0xa5, encoded_size);
    size_t written = 0u;
    assert(png_writer_encode_rgba8(pixels, stride, dimension, encoded, encoded_size,
                                   &written) == PngWriterStatusOk);
    assert(written == encoded_size);

    char path[4096];
    const int path_length = snprintf(path, sizeof(path), "%s/icon-%u.png", directory,
                                     (unsigned int)dimension);
    assert(path_length > 0 && (size_t)path_length < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(encoded, 1u, written, file) == written);
    assert(fclose(file) == 0);

    free(encoded);
    free(pixels);
}

static void test_errors(void)
{
    size_t size_out = 0x11223344u;
    assert(png_writer_encoded_size(17u, &size_out) == PngWriterStatusUnsupportedDimension);
    assert(size_out == 0x11223344u);
    assert(png_writer_encoded_size(16u, NULL) == PngWriterStatusInvalidArgument);
    assert(png_writer_encoded_size(16u, (size_t *)(void *)((uint8_t *)&size_out + 1u)) ==
           PngWriterStatusInvalidArgument);

    assert(png_writer_encoded_size(16u, (size_t *)(uintptr_t)(UINTPTR_MAX - sizeof(size_t) + 1u)) == PngWriterStatusInvalidArgument);

    uint8_t pixels[2048] = {0};
    uint8_t output[2048];
    memset(output, 0x7b, sizeof(output));
    size_t written = 0x55667788u;
    uint8_t before[sizeof(output)];
    memcpy(before, output, sizeof(output));
    assert(png_writer_encode_rgba8(pixels, 16u * 4u - 1u, 16u, output,
                                   sizeof(output), &written) == PngWriterStatusInvalidArgument);
    assert(written == 0x55667788u);
    assert(memcmp(output, before, sizeof(output)) == 0);
    assert(png_writer_encode_rgba8(pixels, 16u * 4u, 16u, output,
                                   1u, &written) == PngWriterStatusInsufficientCapacity);
    assert(written == 0x55667788u);
    assert(memcmp(output, before, sizeof(output)) == 0);
    assert(png_writer_encode_rgba8(pixels, 16u * 4u, 17u, output,
                                   sizeof(output), &written) == PngWriterStatusUnsupportedDimension);
    assert(memcmp(output, before, sizeof(output)) == 0);
    assert(png_writer_encode_rgba8(NULL, 64u, 16u, output, sizeof(output), &written) ==
           PngWriterStatusInvalidArgument);
    assert(png_writer_encode_rgba8(pixels, 64u, 16u, NULL, sizeof(output), &written) ==
           PngWriterStatusInvalidArgument);
    assert(png_writer_encode_rgba8(pixels, 64u, 16u, output, sizeof(output), NULL) ==
           PngWriterStatusInvalidArgument);
    assert(png_writer_encode_rgba8(pixels, 64u, 16u, output, sizeof(output),
                                   (size_t *)(void *)((uint8_t *)&written + 1u)) ==
           PngWriterStatusInvalidArgument);
    assert(png_writer_encode_rgba8(pixels, 16u * 4u, 16u, pixels,
                                   sizeof(pixels), &written) == PngWriterStatusInvalidArgument);
    assert(memcmp(output, before, sizeof(output)) == 0);
    assert(png_writer_encode_rgba8(pixels, 16u * 4u, 16u, output,
                                   sizeof(output), (size_t *)(void *)output) == PngWriterStatusInvalidArgument);
    assert(memcmp(output, before, sizeof(output)) == 0);
    assert(png_writer_encode_rgba8(pixels, SIZE_MAX / 15u, 16u, output,
                                   sizeof(output), &written) == PngWriterStatusOverflow);
    assert(written == 0x55667788u);
    assert(memcmp(output, before, sizeof(output)) == 0);
    assert(png_writer_encode_rgba8(pixels, SIZE_MAX / 15u + 1u, 16u, output,
                                   sizeof(output), &written) == PngWriterStatusOverflow);

    const uint8_t *near_address_end = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 3u);
    assert(png_writer_encode_rgba8(near_address_end, 64u, 16u, output,
                                   sizeof(output), &written) == PngWriterStatusInvalidArgument);
    uint8_t *near_output_end = (uint8_t *)(uintptr_t)(UINTPTR_MAX - 3u);
    assert(png_writer_encode_rgba8(pixels, 64u, 16u, near_output_end,
                                   2048u, &written) == PngWriterStatusInvalidArgument);
    size_t *near_written_end = (size_t *)(uintptr_t)(UINTPTR_MAX - sizeof(size_t) + 1u);
    assert(png_writer_encode_rgba8(pixels, 64u, 16u, output,
                                   sizeof(output), near_written_end) == PngWriterStatusInvalidArgument);
    assert(memcmp(output, before, sizeof(output)) == 0);
    assert(png_writer_encode_rgba8(pixels, 64u, 16u, output, sizeof(output),
                                   (size_t *)(void *)(pixels + 8u)) == PngWriterStatusInvalidArgument);
    assert(png_writer_encode_rgba8(pixels, 64u, 16u, pixels + 512u, 2048u,
                                   &written) == PngWriterStatusInvalidArgument);
    assert(png_writer_encode_rgba8(pixels, 64u, 16u, output, SIZE_MAX,
                                   &written) == PngWriterStatusInvalidArgument);
    _Alignas(size_t) uint8_t shared_output[2048] = {0};
    assert(png_writer_encode_rgba8(pixels, 64u, 16u, shared_output,
        sizeof(shared_output), (size_t *)(void *)(shared_output + 1800u)) == PngWriterStatusInvalidArgument);
    assert(written == 0x55667788u);
    assert(memcmp(output, before, sizeof(output)) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    test_errors();
    for (size_t index = 0u; index < sizeof(TestDimensions) / sizeof(TestDimensions[0]); ++index) {
        write_fixture(argv[1], TestDimensions[index]);
    }
    puts("PNG writer tests passed");
    return 0;
}
