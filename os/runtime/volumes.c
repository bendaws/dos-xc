/*
 * DOS-XC /os/runtime/volumes.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "runtime.h"
#include "storage.h"

#define MAX_DISKS 16
#define READ_AHEAD 64                 /* sectors per disk read */
#define BOUNCE ((u8 *)0x78000)

struct disk {
    u64 sectors;
    u32 flags;                        /* DOSXC_DISK_* */
    u8 bios;                          /* INT 13h drive number, or 0 */
    unsigned firmware;                /* UEFI disk index */
    int usb;                          /* usb.c disk, or -1 */
    int driver;                       /* a driver's disk: its handle + 1, or 0 */
    int native;                       /* storage.c device + 1, or 0 */
    int absent;                       /* slot kept for a disk that went away */
    int ejected;                      /* UEFI: ejected, still in the drive */
    u32 crc;                          /* of sector 0, to recognise BIOS disks on USB */
    struct window { u32 lba, count; } ahead[2];
    int recent;
};
struct volume {
    int used, disk;                   /* disk: -1 for the system volume */
    u32 lba;
    struct fat32_volume fs;
    struct fat32_volume *vol;
};

static struct disk disks[MAX_DISKS];
static unsigned disk_total;
static struct volume volumes[26];
static u8 (*ahead[MAX_DISKS])[READ_AHEAD * 512];      /* 2 windows each, allocated when first used */

/* ---- raw disk access -------------------------------------------------- */

static void forget(struct disk *d) { d->ahead[0].count = d->ahead[1].count = 0; }

static int bios_transfer(u8 drive, u32 lba, unsigned count, void *buffer, int write) {
    volatile u8 *rpc = (volatile u8 *)0x7040;
    if (!count || count > READ_AHEAD) return -1;
    if (write) memcpy(BOUNCE, buffer, (uptr)count * 512);
    rpc[1] = write ? 0xc3 : 0xc2;
    rpc[2] = drive;
    *(volatile u32 *)(rpc + 4) = lba;
    *(volatile u16 *)(rpc + 12) = (u16)count;
    bios_call();
    if (*(volatile int *)(rpc + 8)) return -1;
    if (!write) memcpy(buffer, BOUNCE, (uptr)count * 512);
    return 0;
}

int disk_io(unsigned index, u64 lba, unsigned count, void *buffer, int write) {
    if (index >= disk_total || !count) return -DOSXC_EINVAL;
    struct disk *d = &disks[index];
    if (count > READ_AHEAD && !d->native && !d->driver && d->usb < 0) return -DOSXC_EINVAL;
    if (d->absent) return -DOSXC_ENODEV;
    if (lba >= d->sectors || count > d->sectors - lba) return -DOSXC_EINVAL;
    if (write && (d->flags & (DOSXC_DISK_READONLY | DOSXC_DISK_SYSTEM))) return -DOSXC_EROFS;
    if (write) forget(d);
    int r;
    if (d->native) r = storage_io((unsigned)d->native - 1, lba, count, buffer, write);
    else if (d->driver) r = driver_disk_io(d->driver - 1, lba, count, buffer, write);
    else if (d->usb >= 0) r = usb_disk_io(d->usb, lba, count, buffer, write);
    else if (ctx->disk_io) r = ctx->disk_io(d->firmware, lba, count, buffer, write);
    else if (lba + count > 0xffffffffull) r = -1;
    else r = bios_transfer(d->bios, (u32)lba, count, buffer, write);
    return r ? -DOSXC_EIO : 0;
}

/* Two read-ahead windows per disk, so the FAT sectors a long read keeps
   going back to do not push its data out (and the other way round). */
static int __attribute__((sysv_abi)) sector_read(void *context, uint32_t lba, void *buffer) {
    unsigned index = (unsigned)(uptr)context;
    struct disk *d = &disks[index];
    for (int w = 0; w < 2; ++w) {
        struct window *a = &d->ahead[w];
        if (a->count && lba >= a->lba && lba < a->lba + a->count) {
            d->recent = w;
            memcpy(buffer, ahead[index][w] + (uptr)(lba - a->lba) * 512, 512);
            return 0;
        }
    }
    if (!ahead[index] && !(ahead[index] = memory_alloc(2 * READ_AHEAD * 512, PAGE, OWNER_KERNEL, 0)))
        return disk_io(index, lba, 1, buffer, 0) ? -1 : 0;
    int w = !d->recent;
    struct window *a = &d->ahead[w];
    u32 first = lba & ~(u32)(READ_AHEAD - 1);
    u64 count = d->sectors - first < READ_AHEAD ? d->sectors - first : READ_AHEAD;
    a->count = 0;
    if (lba >= d->sectors || disk_io(index, first, (unsigned)count, ahead[index][w], 0))
        return disk_io(index, lba, 1, buffer, 0) ? -1 : 0;
    a->lba = first;
    a->count = (u32)count;
    d->recent = w;
    memcpy(buffer, ahead[index][w] + (uptr)(lba - first) * 512, 512);
    return 0;
}

/* Long file reads: straight to the disk, READ_AHEAD sectors at a time on
   BIOS drives and all at once on the others. */
static int __attribute__((sysv_abi)) sectors_read(void *context, uint32_t lba, uint32_t count, void *buffer) {
    unsigned index = (unsigned)(uptr)context;
    struct disk *d = &disks[index];
    u8 *out = buffer;
    int whole = d->native || d->usb >= 0 || d->driver;
    while (count) {
        u32 n = whole ? count : count < READ_AHEAD ? count : READ_AHEAD;
        if (disk_io(index, lba, n, out, 0)) return -1;
        lba += n;
        count -= n;
        out += (uptr)n * 512;
    }
    return 0;
}

static int __attribute__((sysv_abi)) sector_write(void *context, uint32_t lba, const void *buffer) {
    unsigned index = (unsigned)(uptr)context;
    return disk_io(index, lba, 1, (void *)buffer, 1) ? -1 : 0;
}

int disk_info(unsigned index, struct dosxc_disk *out) {
    if (index >= disk_total || disks[index].absent) return -DOSXC_ENODEV;
    memset(out, 0, sizeof(*out));
    out->sectors = disks[index].sectors;
    out->flags = disks[index].flags;
    out->bios = disks[index].bios;
    return 0;
}

unsigned disk_count(void) { return disk_total; }

static u32 crc32(const u8 *p, uptr n) {
    u32 c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k) c = c >> 1 ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}

static struct disk *new_disk(void) {
    for (unsigned i = 0; i < disk_total; ++i)
        if (disks[i].absent && !disks[i].bios) { memset(&disks[i], 0, sizeof(disks[i])); disks[i].usb = -1; return &disks[i]; }
    if (disk_total == MAX_DISKS) return 0;
    struct disk *d = &disks[disk_total++];
    memset(d, 0, sizeof(*d));
    d->usb = -1;
    return d;
}

static void find_disks(void) {
    disk_total = 0;
    if (ctx->disk_count) {
        unsigned n = ctx->disk_count();
        for (unsigned i = 0; i < n && disk_total < MAX_DISKS; ++i) {
            uint64_t sectors;
            unsigned flags;
            if (ctx->disk_info(i, &sectors, &flags) || !sectors) continue;
            struct disk *d = &disks[disk_total++];
            memset(d, 0, sizeof(*d));
            d->usb = -1;
            d->sectors = sectors;
            d->firmware = i;
            d->flags = (flags & DISK_FLAG_REMOVABLE ? DOSXC_DISK_REMOVABLE : 0) |
                       (flags & DISK_FLAG_READONLY ? DOSXC_DISK_READONLY : 0) |
                       (flags & DISK_FLAG_SYSTEM ? DOSXC_DISK_SYSTEM : 0);
        }
        return;
    }
    if (!ctx->bios_available) return;
    unsigned fixed = *(volatile u8 *)0x475;          /* BIOS data area: hard disk count */
    if (fixed > MAX_DISKS) fixed = MAX_DISKS;
    int boot_disk = *(volatile u32 *)0x7010 == 0x42544146u;   /* "FATB": booted from a disk */
    u8 boot_drive = *(volatile u8 *)0x7014;
    volatile u8 *rpc = (volatile u8 *)0x7040;
    for (unsigned i = 0; i < fixed; ++i) {
        u8 drive = (u8)(0x80 + i);
        rpc[1] = 0x48;
        rpc[2] = drive;
        bios_call();
        if (*(volatile int *)(rpc + 8)) continue;
        u16 flags = *(u16 *)(BOUNCE + 2), bytes = *(u16 *)(BOUNCE + 24);
        u64 sectors = *(u64 *)(BOUNCE + 16);
        if (bytes != 512 || !sectors) continue;
        struct disk *d = &disks[disk_total++];
        memset(d, 0, sizeof(*d));
        d->usb = -1;
        d->sectors = sectors > 0xffffffffull ? 0xffffffffull : sectors;
        d->bios = drive;
        d->flags = (flags & 4 ? DOSXC_DISK_REMOVABLE : 0) | (boot_disk && drive == boot_drive ? DOSXC_DISK_SYSTEM : 0);
        /* Remember sector 0: if the native USB driver takes this disk over,
           it is recognised by its size and first sector. */
        static u8 first[512];
        if (!bios_transfer(drive, 0, 1, first, 0)) d->crc = crc32(first, 512);
    }
}

/* ---- native controllers (BIOS boot) ----------------------------------- */

static int native_crc(unsigned device, u32 *crc) {
    static u8 first[512];
    if (storage_io(device, 0, 1, first, 0)) return -1;
    *crc = crc32(first, 512);
    return 0;
}

/* Is the system volume on DEVICE at sector BASE?  Compare a few sectors
   read both ways. */
static int holds_system(unsigned device, u64 base) {
    struct fat32_volume *v = ctx->volume;
    static u8 mine[512], theirs[512];
    if (!v || !v->read_sector) return 0;
    u32 probe[3] = {v->partition_lba, v->fat_lba, v->data_lba};
    for (int i = 0; i < 3; ++i) {
        if (base + probe[i] >= storage_sectors(device)) return 0;
        if (storage_io(device, base + probe[i], 1, mine, 0) || v->read_sector(v->context, probe[i], theirs)) return 0;
        if (memcmp(mine, theirs, 512)) return 0;
    }
    return 1;
}

static void attach_native(void) {
    if (!ctx->bios_available || ctx->disk_count) return;
    /* \SYSTEM\BIOSDISK on the system drive keeps every disk on INT 13h
       (for a controller the native drivers get wrong). */
    char off[] = "?:\\SYSTEM\\BIOSDISK";
    struct fat32_info info;
    off[0] = (char)rt_upper(ctx->drive);
    if (ctx->volume && !ctx->stat(ctx->volume, off + 2, &info)) return;
    storage_probe();
    int cd_boot = *(volatile u32 *)0x7010 == 0x46564443u;          /* "CDVF" */
    u8 boot_drive = *(volatile u8 *)0x7014;
    int system_done = 0;
    for (unsigned i = 0; i < storage_count(); ++i) {
        struct blockdev *dev = storage_device(i);
        if (dev->atapi) {
            u64 base = (u64)*(volatile u32 *)0x701c * 4;
            if (!cd_boot || system_done || !holds_system(i, base)) continue;
            bridge_install();
            bridge_set_native(boot_drive, (int)i);
            cache_use_native((int)i, base);
            system_done = 1;
            continue;
        }
        u32 crc;
        if (native_crc(i, &crc)) continue;
        u64 sectors = storage_sectors(i);
        struct disk *match = 0;
        for (unsigned k = 0; k < disk_total; ++k) {
            struct disk *d = &disks[k];
            if (!d->bios || d->native || d->usb >= 0 || d->crc != crc) continue;
            if (d->sectors != sectors && !(d->sectors == 0xffffffffull && sectors > d->sectors)) continue;
            match = d;
            break;
        }
        if (match) {
            match->native = (int)i + 1;
            match->sectors = sectors;
            forget(match);
            bridge_install();
            bridge_set_native(match->bios, (int)i);
            if ((match->flags & DOSXC_DISK_SYSTEM) && !system_done && holds_system(i, 0)) {
                cache_use_native((int)i, 0);
                system_done = 1;
            }
            continue;
        }
        if (disk_total == MAX_DISKS) continue;
        struct disk *d = &disks[disk_total++];
        memset(d, 0, sizeof(*d));
        d->usb = -1;
        d->native = (int)i + 1;
        d->sectors = sectors;
        d->crc = crc;
        d->flags = (dev->removable ? DOSXC_DISK_REMOVABLE : 0) | (dev->readonly ? DOSXC_DISK_READONLY : 0);
    }
}

static char next_letter(void) {
    for (char c = 'C'; c <= 'Z'; ++c)
        if (!volumes[c - 'A'].used) return c;
    return 0;
}

static int is_fat_boot(const u8 *s) {
    return s[510] == 0x55 && s[511] == 0xaa && s[11] == 0 && s[12] == 2 && s[13] && s[16] &&
           (!memcmp(s + 54, "FAT", 3) || !memcmp(s + 82, "FAT32", 5));
}

static void mount(unsigned disk, u32 lba) {
    char letter = next_letter();
    if (!letter) return;
    for (int i = 0; i < 26; ++i)
        if (volumes[i].used && volumes[i].disk == (int)disk && volumes[i].lba == lba) return;
    struct volume *v = &volumes[letter - 'A'];
    memset(v, 0, sizeof(*v));
    v->fs.context = (void *)(uptr)disk;
    v->fs.read_sector = sector_read;
    v->fs.read_sectors = sectors_read;
    v->fs.write_sector = disks[disk].flags & DOSXC_DISK_READONLY ? 0 : sector_write;
    if (ctx->mount(&v->fs, lba)) return;
    v->used = 1;
    v->disk = (int)disk;
    v->lba = lba;
    v->vol = &v->fs;
}

static void scan_disk(unsigned disk) {
    static u8 sector[512], entries[512];
    if (disk_io(disk, 0, 1, sector, 0)) return;
    if (is_fat_boot(sector)) {
        mount(disk, 0);
        return;
    }
    if (sector[510] != 0x55 || sector[511] != 0xaa) return;
    u8 table[64];
    memcpy(table, sector + 446, 64);
    for (int i = 0; i < 4; ++i) {
        const u8 *p = table + i * 16;
        u8 type = p[4];
        u32 start = p[8] | (u32)p[9] << 8 | (u32)p[10] << 16 | (u32)p[11] << 24;
        if (type == 0xee) {                /* protective MBR: read the GPT */
            if (disk_io(disk, 1, 1, sector, 0) || memcmp(sector, "EFI PART", 8)) return;
            u64 table_lba = *(u64 *)(sector + 72);
            u32 count = *(u32 *)(sector + 80), size = *(u32 *)(sector + 84);
            if (size < 128 || size > 512 || count > 256) return;
            static const u8 basic[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                         0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
            static const u8 esp[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                       0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
            for (u32 e = 0; e < count; ++e) {
                u64 byte = (u64)e * size;
                if (disk_io(disk, table_lba + byte / 512, 1, entries, 0)) return;
                const u8 *entry = entries + byte % 512;
                u64 first = *(u64 *)(entry + 32);
                if ((!memcmp(entry, basic, 16) || !memcmp(entry, esp, 16)) && first < 0xffffffffull)
                    mount(disk, (u32)first);
            }
            return;
        }
        if (start && (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0b || type == 0x0c ||
                      type == 0x0e || type == 0xef))
            mount(disk, start);
    }
}

void volumes_scan(void) {
    for (int i = 0; i < 26; ++i)
        if (volumes[i].used && volumes[i].disk >= 0) volumes[i].used = 0;
    for (unsigned i = 0; i < disk_total; ++i) {
        forget(&disks[i]);
        if (!(disks[i].flags & DOSXC_DISK_SYSTEM) && !disks[i].absent) scan_disk(i);
    }
}

#define NOTICES 8
static char notices[NOTICES][80];
static unsigned notice_head, notice_tail;

static void notice(const char *a, const char *b, const char *c) {
    unsigned next = (notice_tail + 1) % NOTICES;
    if (next == notice_head) notice_head = (notice_head + 1) % NOTICES;
    char *out = notices[notice_tail];
    uptr n = 0;
    const char *parts[3] = {a, b, c};
    for (int i = 0; i < 3; ++i)
        for (const char *p = parts[i]; p && *p && n < 79; ++p) out[n++] = *p;
    out[n] = 0;
    notice_tail = next;
    event_log(DOSXC_EVENT_INFO, "Devices", "%s", out);
}

void volume_post_notice(const char *text) { notice(text, 0, 0); }

int volume_notice(char *buffer, uptr size) {
    if (notice_head == notice_tail) return 0;
    const char *text = notices[notice_head];
    uptr n = rt_strlen(text);
    if (n + 1 > size) return -DOSXC_ERANGE;
    memcpy(buffer, text, n + 1);
    notice_head = (notice_head + 1) % NOTICES;
    return (int)n;
}

/* "E:" or "E:, F:" for the volumes of DISK. */
static void letters_of(unsigned disk, char *out) {
    uptr n = 0;
    for (int i = 0; i < 26; ++i)
        if (volumes[i].used && volumes[i].disk == (int)disk && n + 5 < 40) {
            if (n) { out[n++] = ','; out[n++] = ' '; }
            out[n++] = (char)('A' + i);
            out[n++] = ':';
        }
    out[n] = 0;
}

static void size_text(u64 sectors, char *out) {
    u64 mb = sectors / 2048;
    const char *unit = " MB";
    u64 value = mb;
    if (mb >= 10240) { value = mb / 1024; unit = " GB"; }
    char digits[24];
    int d = 0;
    do digits[d++] = (char)('0' + value % 10); while (value /= 10);
    uptr n = 0;
    while (d) out[n++] = digits[--d];
    for (const char *u = unit; *u; ++u) out[n++] = *u;
    out[n] = 0;
}

/* Unmount every volume of DISK; returns how many there were. */
static int unmount_disk(unsigned disk) {
    int count = 0;
    for (int i = 0; i < 26; ++i)
        if (volumes[i].used && volumes[i].disk == (int)disk) {
            files_volume_gone((char)('A' + i));
            volumes[i].used = 0;
            ++count;
        }
    forget(&disks[disk]);
    return count;
}

static void disk_arrived(unsigned index, const char *kind) {
    struct disk *d = &disks[index];
    scan_disk(index);
    char letters[40], size[24];
    letters_of(index, letters);
    size_text(d->sectors, size);
    if (letters[0]) notice(kind, " drive ready as ", letters);
    else notice(kind, " disk without a FAT volume found, ", size);
}

static void disk_left(unsigned index, const char *kind) {
    char letters[40];
    letters_of(index, letters);
    int had = unmount_disk(index);
    disks[index].absent = 1;
    if (had) notice(kind, " drive removed: ", letters);
}

/* usb.c: USB disk DISK appeared (PRESENT) or went away. */
void volumes_usb_changed(int usb, int present) {
    if (present) {
        struct usb_disk_info info;
        if (usb_disk_info(usb, &info)) return;
        static u8 first[512];
        u32 crc = usb_disk_io(usb, 0, 1, first, 0) ? 0 : crc32(first, 512);
        for (unsigned i = 0; i < disk_total; ++i) {
            struct disk *d = &disks[i];
            if (!d->bios || d->usb >= 0 || d->absent || d->crc != crc ||
                (d->sectors != info.sectors && !(d->sectors == 0xffffffffull && info.sectors > d->sectors))) continue;
            d->usb = usb;
            forget(d);
            bridge_set_drive(d->bios, usb);
            return;
        }
        struct disk *d = new_disk();
        if (!d) return;
        d->usb = usb;
        d->sectors = info.sectors;
        d->crc = crc;
        d->flags = DOSXC_DISK_USB | (info.removable ? DOSXC_DISK_REMOVABLE : 0) | (info.readonly ? DOSXC_DISK_READONLY : 0);
        disk_arrived((unsigned)(d - disks), "USB");
        return;
    }
    for (unsigned i = 0; i < disk_total; ++i) {
        struct disk *d = &disks[i];
        if (d->usb != usb || d->absent) continue;
        if (d->bios) bridge_set_drive(d->bios, -1);
        d->usb = -1;
        disk_left(i, "USB");
    }
}

/* drivers.c: a driver's disk appeared or went away. */
int volumes_driver_disk(int handle, u64 sectors, int readonly, const char *name) {
    struct disk *d = new_disk();
    if (!d) return -1;
    d->driver = handle + 1;
    d->sectors = sectors;
    d->flags = readonly ? DOSXC_DISK_READONLY : 0;
    disk_arrived((unsigned)(d - disks), name);
    return 0;
}

void volumes_driver_disk_gone(int handle) {
    for (unsigned i = 0; i < disk_total; ++i) {
        if (disks[i].driver != handle + 1 || disks[i].absent) continue;
        disks[i].driver = 0;
        disk_left(i, "Disk");
    }
}

/* EJECT: write back, unmount every volume of the drive's disk, and stop
   using the disk until it is plugged in again. */
int volume_eject(char letter) {
    letter = (char)rt_upper(letter);
    if (letter < 'A' || letter > 'Z' || !volumes[letter - 'A'].used) return -DOSXC_ENODEV;
    struct volume *v = &volumes[letter - 'A'];
    if (v->disk < 0) return -DOSXC_EBUSY;
    unsigned index = (unsigned)v->disk;
    struct disk *d = &disks[index];
    for (int i = 0; i < 26; ++i)
        if (volumes[i].used && volumes[i].disk == (int)index) files_flush_volume((char)('A' + i));
    char letters[40];
    letters_of(index, letters);
    unmount_disk(index);
    struct blockdev *native = d->native ? storage_device((unsigned)d->native - 1) : 0;
    if (native && native->flush && !native->readonly) native->flush(native);
    if (d->usb >= 0) {
        usb_disk_eject(d->usb);
        if (d->bios) bridge_set_drive(d->bios, -1);
        d->usb = -1;
        d->absent = 1;
    } else if (ctx->disk_io && (d->flags & DOSXC_DISK_REMOVABLE)) {
        d->absent = 1;
        d->ejected = 1;
    }
    notice("", letters, " can now be removed");
    return 0;
}

void volumes_poll(void) {
    static u64 last;
    if (!ctx->disk_rescan) return;
    u64 now = cpu_nanoseconds();
    if (now - last < 1000000000ull) return;
    last = now;
    unsigned n = ctx->disk_rescan();
    for (unsigned f = 0; f < n; ++f) {
        uint64_t sectors = 0;
        unsigned flags = 0;
        int present = !ctx->disk_info(f, &sectors, &flags) && sectors;
        int slot = -1;
        for (unsigned i = 0; i < disk_total; ++i)
            if (disks[i].firmware == f && disks[i].usb < 0 && !(disks[i].bios)) { slot = (int)i; break; }
        if (slot >= 0 && disks[slot].flags & DOSXC_DISK_SYSTEM) continue;
        if (slot >= 0 && !disks[slot].absent && (!present || sectors != disks[slot].sectors)) {
            disk_left((unsigned)slot, "Removable");
        }
        if (slot >= 0 && disks[slot].ejected) {
            if (!present) disks[slot].ejected = 0;
            continue;
        }
        if (!present) continue;
        if (slot >= 0 && !disks[slot].absent) continue;
        struct disk *d = slot >= 0 ? &disks[slot] : new_disk();
        if (!d) continue;
        memset(d, 0, sizeof(*d));
        d->usb = -1;
        d->firmware = f;
        d->sectors = sectors;
        d->flags = (flags & DISK_FLAG_REMOVABLE ? DOSXC_DISK_REMOVABLE : 0) |
                   (flags & DISK_FLAG_READONLY ? DOSXC_DISK_READONLY : 0);
        if ((unsigned)(d - disks) >= disk_total) disk_total = (unsigned)(d - disks) + 1;
        disk_arrived((unsigned)(d - disks), "Removable");
    }
}

void volumes_init(void) {
    memset(volumes, 0, sizeof(volumes));
    char system = (char)rt_upper(ctx->drive);
    if (system >= 'A' && system <= 'Z' && ctx->volume) {
        struct volume *v = &volumes[system - 'A'];
        v->used = 1;
        v->disk = -1;
        v->vol = ctx->volume;
    }
    find_disks();
    attach_native();
    volumes_scan();
}

void volumes_resume(void) {
    char system = (char)rt_upper(ctx->drive);
    if (system >= 'A' && system <= 'Z') volumes[system - 'A'].vol = ctx->volume;
    for (unsigned i = 0; i < disk_total; ++i) forget(&disks[i]);
}

struct fat32_volume *volume_of(const char *canonical) {
    char c = (char)rt_upper(canonical[0]);
    if (c < 'A' || c > 'Z' || canonical[1] != ':' || !volumes[c - 'A'].used) return 0;
    return volumes[c - 'A'].vol;
}

int volume_exists(char letter) {
    letter = (char)rt_upper(letter);
    return letter >= 'A' && letter <= 'Z' && volumes[letter - 'A'].used;
}

int volumes_list(struct dosxc_volume *out, unsigned capacity) {
    unsigned count = 0;
    for (int i = 0; i < 26; ++i) {
        struct volume *v = &volumes[i];
        if (!v->used) continue;
        if (count < capacity) {
            struct dosxc_volume *e = &out[count];
            memset(e, 0, sizeof(*e));
            e->letter = (char)('A' + i);
            e->type = (u8)v->vol->type;
            if (v->disk < 0) e->flags |= DOSXC_VOLUME_SYSTEM | (ctx->live ? DOSXC_VOLUME_LIVE : 0);
            else {
                e->disk = (u32)v->disk;
                if (disks[v->disk].flags & DOSXC_DISK_REMOVABLE) e->flags |= DOSXC_VOLUME_REMOVABLE;
            }
            if (!v->vol->write_sector) e->flags |= DOSXC_VOLUME_READONLY;
            e->total_bytes = (u64)v->vol->cluster_count * v->vol->sectors_per_cluster * v->vol->bytes_per_sector;
            e->free_bytes = ctx->free_bytes(v->vol);
        }
        ++count;
    }
    return (int)count;
}

#define VOLUME(path) struct fat32_volume *volume = volume_of(path); if (!volume) return -1
int fs_stat(const char *path, struct fat32_info *info) { VOLUME(path); return ctx->stat(volume, path + 2, info); }
int fs_list(const char *path, fat32_list_fn fn, void *context) { VOLUME(path); return ctx->list(volume, path + 2, fn, context); }
int fs_read(const char *path, void *buffer, u32 size) { VOLUME(path); return ctx->read(volume, path + 2, buffer, size); }
int fs_read_at(const char *path, u32 offset, void *buffer, u32 size) {
    VOLUME(path);
    return ctx->read_at(volume, path + 2, offset, buffer, size);
}
int fs_read_cursor(const char *path, struct fat32_cursor *cursor, u32 offset, void *buffer, u32 size) {
    VOLUME(path);
    return ctx->read_cursor(volume, path + 2, cursor, offset, buffer, size);
}
int fs_save(const char *path, const void *data, u32 size) { VOLUME(path); return ctx->save(volume, path + 2, data, size); }
int fs_mkdir(const char *path) { VOLUME(path); return ctx->mkdir(volume, path + 2); }
int fs_remove(const char *path) { VOLUME(path); return ctx->remove(volume, path + 2); }
int fs_rmdir(const char *path) { VOLUME(path); return ctx->rmdir(volume, path + 2); }
int fs_rename(const char *from, const char *to) {
    VOLUME(from);
    if (rt_upper(from[0]) != rt_upper(to[0])) return -1;
    return ctx->rename(volume, from + 2, to + 2);
}
int fs_writable(const char *path) {
    struct fat32_volume *volume = volume_of(path);
    return volume && volume->write_sector;
}
