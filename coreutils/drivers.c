/*
 * DOS-XC /coreutils/drivers.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "common.h"

int main(int argc, char **argv) {
    if (argc > 1 && (same(argv[1], "/?") || same(argv[1], "/HELP"))) {
        puts("DRIVERS                  Lists loaded drivers\n"
             "DRIVERS /LOAD file.drv   Loads a driver now\n"
             "DRIVERS /UNLOAD name.drv Stops and unloads a driver\n"
             "DRIVERS /DEVICES         Lists PCI devices and the driver of each\n"
             "DRIVERS /LOG             Shows the driver log");
        return 0;
    }
    if (argc > 1 && same(argv[1], "/LOG")) {
        static char text[8192];
        int n = dosxc_sys_driver(DOSXC_DRIVER_LOG, 0, text, sizeof(text));
        if (n <= 0) puts("The driver log is empty.");
        else fputs(text, stdout);
        return 0;
    }
    if (argc > 1 && same(argv[1], "/DEVICES")) {
        int count = dosxc_sys_driver(DOSXC_DRIVER_DEVICES, 0, 0, 0);
        if (count <= 0) { puts("No PCI devices were found."); return 0; }
        printf("%-8s %-10s %-6s %-10s %s\n", "Address", "ID", "Class", "Subsystem", "Driver");
        for (int i = 0; i < count; ++i) {
            struct dosxc_deviceinfo d;
            if (dosxc_sys_driver(DOSXC_DRIVER_DEVICES, i, &d, sizeof(d)) < 0) continue;
            printf("%02x:%02x.%u  %04x:%04x  %02x%02x   %04x:%04x  %s\n", d.bus, d.device, d.function, d.vendor, d.id,
                   d.class_code, d.subclass, d.subsystem_vendor, d.subsystem, d.driver[0] ? d.driver : "-");
        }
        return 0;
    }
    if (argc > 2 && same(argv[1], "/LOAD")) {
        int r = dosxc_sys_driver(DOSXC_DRIVER_LOAD, 0, argv[2], 0);
        if (r < 0) { printf("Cannot load %s: %s.\n", argv[2], strerror(-r)); return 1; }
        puts("Driver loaded.");
        return 0;
    }
    if (argc > 2 && same(argv[1], "/UNLOAD")) {
        int r = dosxc_sys_driver(DOSXC_DRIVER_UNLOAD, 0, argv[2], 0);
        if (r < 0) { printf("Cannot unload %s: %s.\n", argv[2], strerror(-r)); return 1; }
        puts("Driver unloaded.");
        return 0;
    }
    int count = dosxc_sys_driver(DOSXC_DRIVER_LIST, 0, 0, 0);
    if (count <= 0) { puts("No drivers are loaded."); return 0; }
    printf("%-14s %-22s %-8s %-8s %s\n", "File", "Driver", "Version", "State", "Devices");
    for (int i = 0; i < count; ++i) {
        struct dosxc_driverinfo info;
        if (dosxc_sys_driver(DOSXC_DRIVER_LIST, i, &info, sizeof(info)) < 0) continue;
        const char *state = info.state == DOSXC_DRIVER_STATE_LOADED ? "running"
                          : info.state == DOSXC_DRIVER_STATE_IDLE ? "no device" : "FAILED";
        printf("%-14s %-22s %-8s %-8s %u\n", info.file, info.name, info.version, state, info.devices);
    }
    return 0;
}
