/** @file gpu.c @brief Synthetic sysfs capability tests; no real driver changes. */
#include "av_gpu.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char root[] = "/tmp/waddle-gpu-XXXXXX";
static void path_for(char *path, const char *suffix) {
    assert(snprintf(path, 1024, "%s/%s", root, suffix) < 1024);
}
static void make_directory(const char *suffix) {
    char path[1024]; path_for(path, suffix); assert(mkdir(path, 0700) == 0);
}
static void make_link(const char *suffix, const char *target) {
    char path[1024]; path_for(path, suffix); assert(symlink(target, path) == 0);
}
static void remove_link(const char *suffix) {
    char path[1024]; path_for(path, suffix); assert(unlink(path) == 0);
}
int main(void) {
    assert(mkdtemp(root));
    const char *directories[] = {"kernel", "kernel/iommu_groups", "kernel/iommu_groups/1",
        "kernel/iommu_groups/1/devices", "bus", "bus/pci", "bus/pci/devices",
        "bus/pci/devices/0000:01:00.0", "bus/pci/devices/0000:01:00.1"};
    for (unsigned i = 0; i < sizeof(directories)/sizeof(*directories); ++i) make_directory(directories[i]);
    char group[1024], device[1024], error[512];
    path_for(group, "kernel/iommu_groups/1");
    make_link("bus/pci/devices/0000:01:00.0/iommu_group", group);
    path_for(device, "bus/pci/devices/0000:01:00.0");
    make_link("kernel/iommu_groups/1/devices/0000:01:00.0", device);
    path_for(device, "bus/pci/devices/0000:01:00.1");
    make_link("kernel/iommu_groups/1/devices/0000:01:00.1", device);
    assert(av_gpu_probe(root, "../../driver", error, sizeof(error)) == -1 && errno == EINVAL);
    assert(av_gpu_probe(root, "0000:01:00.0", error, sizeof(error)) == 0);
    make_link("bus/pci/devices/0000:01:00.0/driver", "/sys/bus/pci/drivers/vfio-pci");
    assert(av_gpu_probe(root, "0000:01:00.0", error, sizeof(error)) == 0);
    const char *drivers[] = {"/sys/bus/pci/drivers/nvidia", "/sys/bus/pci/drivers/amdgpu",
        "/sys/bus/pci/drivers/nouveau", "/sys/bus/pci/drivers/snd_hda_intel"};
    for (unsigned i = 0; i < 4; ++i) {
        make_link("bus/pci/devices/0000:01:00.1/driver", drivers[i]);
        assert(av_gpu_probe(root, "0000:01:00.0", error, sizeof(error)) == -1 && errno == EBUSY);
        assert(strstr(error, "0000:01:00.1") && strstr(error, "preserved"));
        remove_link("bus/pci/devices/0000:01:00.1/driver");
    }
    remove_link("bus/pci/devices/0000:01:00.0/driver");
    make_directory("bus/pci/devices/0000:01:00.0/driver");
    assert(av_gpu_probe(root, "0000:01:00.0", error, sizeof(error)) == -1);
    char path[1024]; path_for(path, "bus/pci/devices/0000:01:00.0/driver"); assert(rmdir(path) == 0);
    remove_link("bus/pci/devices/0000:01:00.0/iommu_group");
    make_link("bus/pci/devices/0000:01:00.0/iommu_group", root);
    assert(av_gpu_probe(root, "0000:01:00.0", error, sizeof(error)) == -1 && errno == EINVAL);
    remove_link("bus/pci/devices/0000:01:00.0/iommu_group");
    assert(av_gpu_probe(root, "0000:01:00.0", error, sizeof(error)) == -1);
    make_link("bus/pci/devices/0000:01:00.0/iommu_group", group);
    remove_link("kernel/iommu_groups/1/devices/0000:01:00.0");
    assert(av_gpu_probe(root, "0000:01:00.0", error, sizeof(error)) == -1 && errno == ENODEV);
    remove_link("kernel/iommu_groups/1/devices/0000:01:00.1");
    remove_link("bus/pci/devices/0000:01:00.0/iommu_group");
    for (unsigned i = sizeof(directories)/sizeof(*directories); i > 0; --i) {
        path_for(path, directories[i - 1]); assert(rmdir(path) == 0);
    }
    assert(rmdir(root) == 0);
    puts("GPU probe: unbound/VFIO, busy companions, malformed links and missing groups passed");
    return 0;
}
