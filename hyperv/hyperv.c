/*
 * DOS-XC /hyperv/hyperv.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "hyperv.h"

const struct dk_api *dk;

static int load(const struct dk_api *api) {
    if (api->size < sizeof(struct dk_api)) return -1;
    dk = api;
    return 0;
}

static int probe(const struct dk_pci_device *device, void **instance) {
    (void)device;
    *instance = 0;
    if (vmbus_start()) {
        vmbus_stop();
        return -1;
    }
    int started = 0;
    started += !hv_keyboard_start();
    started += !hv_mouse_start();
    started += !hv_storage_start();
    started += !hv_net_start();
    dk->log("HYPERV: %d of 4 device kinds started", started);
    return 0;
}

static void poll(void *instance) {
    (void)instance;
    vmbus_poll();
    hv_keyboard_poll();
    hv_mouse_poll();
    hv_net_poll();
    hv_storage_poll();
}

static void remove_device(void *instance) {
    (void)instance;
    hv_net_stop();
    hv_storage_stop();
    hv_input_stop();
    vmbus_stop();
}

DK_DRIVER = {DK_ABI_VERSION, sizeof(struct dk_driver), "Hyper-V", "1.0",
             "Hyper-V network, disks, keyboard and mouse", 0, load, probe, poll, remove_device, 0};
