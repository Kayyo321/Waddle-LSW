/** @file kvmfr.c @brief Real driver export, aliasing and lease preservation test. */
#include "av_dmabuf.h"
#include "av_environment.h"
#include "av_layout.h"
#include "av_kvmfr.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
    daemon_config_t config;
    daemon_config_init_defaults(&config);
    config.av_enabled = 1;
    strcpy(config.av_shm_path, "/dev/kvmfr0");
    av_environment_t environment = {.fd = -1};
    char error[1024];
    assert(av_environment_prepare(&environment, &config, error, sizeof(error)) == 0);
    av_environment_t contender = {.fd = -1};
    assert(av_environment_prepare(&contender, &config, error, sizeof(error)) == -1);
    assert(contender.fd == -1 && !contender.mapping);
    assert(av_kvmfr_size(environment.fd) == (long)AvMappingBytes);
    assert(lseek(environment.fd, 0, SEEK_END) == (off_t)AvMappingBytes);
    assert(lseek(environment.fd, 0, SEEK_SET) == 0);
    int exported = av_dmabuf_export(environment.fd, AvPixelOffset, AvSlotCapacity);
    assert(exported >= 0 && (fcntl(exported, F_GETFD) & FD_CLOEXEC));
    uint8_t *pixels = mmap(NULL, AvSlotCapacity, PROT_READ | PROT_WRITE, MAP_SHARED, exported, 0);
    assert(pixels != MAP_FAILED);
    uint8_t *shared = (uint8_t *)environment.mapping + AvPixelOffset;
    shared[0] = 0x53;
    assert(pixels[0] == 0x53);
    pixels[AvSlotCapacity - 1] = 0xa7;
    assert(shared[AvSlotCapacity - 1] == 0xa7);
    shared[0] = shared[AvSlotCapacity - 1] = 0;
    assert(av_dmabuf_export(environment.fd, AvMappingBytes, AvSlotCapacity) == -1);
    assert(munmap(pixels, AvSlotCapacity) == 0);
    close(exported);
    window_slot_header_t *slot = av_layout_slot(environment.mapping, environment.length, 0, 0);
    atomic_store_explicit(&slot->slot_state, SlotConsuming, memory_order_release);
    assert(av_environment_prepare(&contender, &config, error, sizeof(error)) == -1);
    assert(atomic_load_explicit(&slot->slot_state, memory_order_acquire) == SlotConsuming);
    atomic_store_explicit(&slot->slot_state, SlotFree, memory_order_release);
    av_environment_free(&environment);
    assert(av_environment_prepare(&contender, &config, error, sizeof(error)) == 0);
    av_environment_free(&contender);
    assert(access(config.av_shm_path, R_OK | W_OK) == 0);
    puts("Real KVMFR: two-GiB capacity, CLOEXEC DMA-BUF aliasing and live lease preservation passed");
    return 0;
}
