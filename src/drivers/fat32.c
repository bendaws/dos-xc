/*
 * DOS-XC /drivers/fat32.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "fat32.h"

#define FAT_MAX_SECTOR 4096
#define FAT_MAX_CLUSTER 65536
#define FAT_MAX_NAME 260
#define FAT_FREE 0
#define FAT_BAD 0x0ffffff7u

static unsigned char sector_buffer[FAT_MAX_SECTOR];
static unsigned char fat_buffer[FAT_MAX_SECTOR];
static unsigned char zero_cluster[FAT_MAX_CLUSTER];
static void forget_walk(void);
/* The directory sector read last (directory_entry_at), kept until written. */
static unsigned char dir_buffer[FAT_MAX_SECTOR];
static const struct fat32_volume *dir_volume;
static uint32_t dir_lba;
static int put_sector(const struct fat32_volume *volume, uint32_t lba, const void *data) {
    if (volume == dir_volume && lba == dir_lba) dir_volume = 0;
    return volume->write_sector(volume->context, lba, data);
}

static unsigned int string_length(const char *text) {
    unsigned int length = 0;
    while (text[length]) ++length;
    return length;
}

static uint16_t get16(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get32(const unsigned char *p) {
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static void put16(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static int is_eoc(const struct fat32_volume *volume, uint32_t value) {
    if (volume->type == FAT_TYPE_12) return value >= 0xff8;
    if (volume->type == FAT_TYPE_16) return value >= 0xfff8;
    return value >= 0x0ffffff8;
}

static int valid_cluster(const struct fat32_volume *volume, uint32_t cluster) {
    return cluster >= 2 && cluster < volume->cluster_count + 2;
}

static uint32_t cluster_lba(const struct fat32_volume *volume, uint32_t cluster) {
    return volume->data_lba + (cluster - 2) * volume->sectors_per_cluster;
}

static int read_fat_entry(const struct fat32_volume *volume, uint32_t cluster, uint32_t *value) {
    uint32_t offset;
    uint32_t lba;
    if (volume->type == FAT_TYPE_12) offset = cluster + cluster / 2;
    else if (volume->type == FAT_TYPE_16) offset = cluster * 2;
    else offset = cluster * 4;
    lba = volume->fat_lba + offset / volume->bytes_per_sector;
    offset %= volume->bytes_per_sector;
    if (volume->read_sector(volume->context, lba, fat_buffer) != 0) return -1;
    if (volume->type == FAT_TYPE_12) {
        uint16_t pair;
        if (offset == volume->bytes_per_sector - 1) return -1;
        pair = get16(fat_buffer + offset);
        *value = (cluster & 1) ? pair >> 4 : pair & 0x0fff;
    } else if (volume->type == FAT_TYPE_16) {
        if (offset > volume->bytes_per_sector - 2) return -1;
        *value = get16(fat_buffer + offset);
    } else {
        if (offset > volume->bytes_per_sector - 4) return -1;
        *value = get32(fat_buffer + offset) & 0x0fffffff;
    }
    return 0;
}

static int write_fat_entry(const struct fat32_volume *volume, uint32_t cluster, uint32_t value) {
    uint32_t offset;
    uint32_t lba;
    if (!volume->write_sector) return -1;
    if (volume->type == FAT_TYPE_12) offset = cluster + cluster / 2;
    else if (volume->type == FAT_TYPE_16) offset = cluster * 2;
    else offset = cluster * 4;
    lba = volume->fat_lba + offset / volume->bytes_per_sector;
    offset %= volume->bytes_per_sector;
    if (volume->read_sector(volume->context, lba, fat_buffer) != 0) return -1;
    if (volume->type == FAT_TYPE_12) {
        uint16_t pair;
        if (offset == volume->bytes_per_sector - 1) return -1;
        pair = get16(fat_buffer + offset);
        if (cluster & 1) pair = (uint16_t)((pair & 0x000f) | ((value & 0x0fff) << 4));
        else pair = (uint16_t)((pair & 0xf000) | (value & 0x0fff));
        put16(fat_buffer + offset, pair);
    } else if (volume->type == FAT_TYPE_16) {
        if (offset > volume->bytes_per_sector - 2) return -1;
        put16(fat_buffer + offset, (uint16_t)value);
    } else {
        if (offset > volume->bytes_per_sector - 4) return -1;
        put32(fat_buffer + offset, value);
    }
    for (uint32_t copy = 0; copy < volume->fat_count; ++copy) {
        uint32_t copy_lba = lba + copy * volume->sectors_per_fat;
        if (put_sector(volume, copy_lba, fat_buffer) != 0) return -1;
    }
    return 0;
}

static void update_fsinfo(const struct fat32_volume *volume,int change,uint32_t next) {
    struct fat32_volume *mutable=(struct fat32_volume *)volume;
    mutable->next_free=next;
    if(mutable->free_clusters!=0xffffffffu)mutable->free_clusters+=change;
    if(volume->type!=FAT_TYPE_32||!volume->fsinfo_sector||!volume->write_sector)return;
    uint32_t lba=volume->partition_lba+volume->fsinfo_sector;
    if(volume->read_sector(volume->context,lba,sector_buffer))return;
    if(get32(sector_buffer)!=0x41615252||get32(sector_buffer+484)!=0x61417272)return;
    put32(sector_buffer+488,volume->free_clusters);put32(sector_buffer+492,next);
    if(put_sector(volume,lba,sector_buffer))mutable->free_clusters=0xffffffffu;
}
static int allocate_cluster(const struct fat32_volume *volume, uint32_t *cluster) {
    uint32_t start=valid_cluster(volume,volume->next_free)?volume->next_free:2;
    for(uint32_t n=0;n<volume->cluster_count;++n){
        uint32_t candidate=start+n;if(candidate>=volume->cluster_count+2)candidate-=volume->cluster_count;
        uint32_t value;if(read_fat_entry(volume,candidate,&value))return -1;
        if(value==FAT_FREE){if(write_fat_entry(volume,candidate,volume->type==FAT_TYPE_12?0xfff:volume->type==FAT_TYPE_16?0xffff:0x0fffffff))return -1;
            *cluster=candidate;update_fsinfo(volume,-1,candidate+1);return 0;}
    }
    return -1;
}

static int free_chain(const struct fat32_volume *volume, uint32_t cluster) {
    forget_walk();
    while (valid_cluster(volume, cluster)) {
        uint32_t next;
        if (read_fat_entry(volume, cluster, &next) != 0 ||
            write_fat_entry(volume, cluster, FAT_FREE) != 0) return -1;
        update_fsinfo(volume,1,cluster);
        if (is_eoc(volume, next)) break;
        cluster = next;
    }
    return 0;
}

static int next_cluster(const struct fat32_volume *volume, uint32_t cluster, uint32_t *next) {
    if (read_fat_entry(volume, cluster, next) != 0) return -1;
    return is_eoc(volume, *next) ? 1 : valid_cluster(volume, *next) ? 0 : -1;
}

static int path_component(const char **path, char *component) {
    unsigned int length = 0;
    while (**path == '/' || **path == '\\') ++*path;
    if (!**path) return 0;
    while (**path && **path != '/' && **path != '\\') {
        if (length + 1 >= FAT_MAX_NAME) return -1;
        component[length++] = *(*path)++;
    }
    component[length] = 0;
    return 1;
}

static int equal_name(const char *left, const char *right) {
    while (*left && *right) {
        char a = *left++, b = *right++;
        if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
        if (a != b) return 0;
    }
    return *left == 0 && *right == 0;
}

static unsigned char short_checksum(const unsigned char *name) {
    unsigned char sum = 0;
    for (unsigned int i = 0; i < 11; ++i) sum = (unsigned char)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

static void short_name(const unsigned char *entry, char *name) {
    unsigned int at = 0;
    for (unsigned int i = 0; i < 8 && entry[i] != ' '; ++i) name[at++] = entry[i];
    if (entry[8] != ' ') {
        name[at++] = '.';
        for (unsigned int i = 8; i < 11 && entry[i] != ' '; ++i) name[at++] = entry[i];
    }
    name[at] = 0;
}

static void lfn_chars(const unsigned char *entry, char *name, unsigned int start) {
    static const unsigned char positions[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (unsigned int i = 0; i < sizeof(positions); ++i) {
        unsigned char position = positions[i];
        uint16_t character = get16(entry + position);
        if (start + i >= FAT_MAX_NAME) continue;
        name[start + i] = character == 0 || character == 0xffff ? 0 :
                          character < 128 ? (char)character : '?';
    }
}

struct directory_entry {
    unsigned char data[32];
    uint32_t lba;
    uint32_t offset;
    uint32_t directory_cluster;
    uint32_t ordinal;
    uint32_t lfn_count;
};

/* Where the last directory lookup ended, so scans that go entry by entry
   continue from there instead of walking the cluster chain again. */
static const struct fat32_volume *walk_volume;
static uint32_t walk_directory, walk_cluster, walk_first;

static void forget_walk(void) { walk_volume = 0; dir_volume = 0; }

static int directory_entry_at(const struct fat32_volume *volume, uint32_t directory,
                              uint32_t ordinal, struct directory_entry *result) {
    uint32_t per_sector = volume->bytes_per_sector / 32, lba, offset;
    if (volume->type != FAT_TYPE_32 && directory == 0) {
        uint32_t sector = ordinal / per_sector;
        if (sector >= volume->root_sectors) return -1;
        lba = volume->root_lba + sector;
    } else {
        uint32_t per_cluster = per_sector * volume->sectors_per_cluster, cluster = directory, first = 0;
        if (walk_volume == volume && walk_directory == directory && ordinal >= walk_first) {
            cluster = walk_cluster;
            first = walk_first;
        }
        while (ordinal >= first + per_cluster) {
            uint32_t next;
            if (next_cluster(volume, cluster, &next) != 0) return -1;
            cluster = next;
            first += per_cluster;
        }
        if (!valid_cluster(volume, cluster)) return -1;
        walk_volume = volume;
        walk_directory = directory;
        walk_cluster = cluster;
        walk_first = first;
        lba = cluster_lba(volume, cluster) + (ordinal - first) / per_sector;
    }
    offset = (ordinal % per_sector) * 32;
    if (dir_volume != volume || dir_lba != lba) {
        dir_volume = 0;
        if (volume->read_sector(volume->context, lba, dir_buffer) != 0) return -1;
        dir_volume = volume;
        dir_lba = lba;
    }
    for (unsigned int i = 0; i < 32; ++i) result->data[i] = dir_buffer[offset + i];
    result->lba = lba;
    result->offset = offset;
    result->directory_cluster = directory;
    result->ordinal = ordinal;
    return 0;
}

static int find_in_directory(const struct fat32_volume *volume, uint32_t directory,
                             const char *wanted, struct directory_entry *result) {
    char long_name[FAT_MAX_NAME];
    uint32_t lfn_count = 0;
    for (uint32_t ordinal = 0;; ++ordinal) {
        struct directory_entry current;
        if (directory_entry_at(volume, directory, ordinal, &current) != 0) return -1;
        unsigned char first = current.data[0];
        if (first == 0) return -1;
        if (first == 0xe5) { lfn_count = 0; continue; }
        if (current.data[11] == 0x0f) {
            unsigned int order = current.data[0] & 0x1f;
            if (current.data[0] & 0x40) {
                lfn_count = 0;
                for (unsigned int i = 0; i < FAT_MAX_NAME; ++i) long_name[i] = 0;
            }
            if (order > 0 && order <= FAT_MAX_NAME / 13) {
                lfn_chars(current.data, long_name, (order - 1) * 13);
                if (order > lfn_count) lfn_count = order;
            }
            continue;
        }
        if (current.data[11] & 0x08) { lfn_count = 0; continue; }
        char candidate[FAT_MAX_NAME];
        short_name(current.data, candidate);
        if (lfn_count * 13 < FAT_MAX_NAME) long_name[lfn_count * 13] = 0;
        if ((lfn_count && equal_name(long_name, wanted)) || equal_name(candidate, wanted)) {
            for (unsigned int i = 0; i < 32; ++i) result->data[i] = current.data[i];
            result->lba = current.lba;
            result->offset = current.offset;
            result->directory_cluster = current.directory_cluster;
            result->ordinal = current.ordinal;
            result->lfn_count = lfn_count;
            return 0;
        }
        lfn_count = 0;
    }
}

static int find_path(const struct fat32_volume *volume, const char *path, struct directory_entry *result) {
    const char *cursor = path;
    char component[FAT_MAX_NAME];
    uint32_t directory = volume->type == FAT_TYPE_32 ? volume->root_cluster : 0;
    int status;
    while ((status = path_component(&cursor, component)) > 0) {
        if (find_in_directory(volume, directory, component, result) != 0) return -1;
        directory = ((uint32_t)get16(result->data + 20) << 16) | get16(result->data + 26);
    }
    return status < 0 ? -1 : 0;
}

/* PATH with every component replaced by its 8.3 alias ("\PROGRA~1\OREGON"),
   for 16-bit programs.  SIZE is the size of OUT. */
int fat32_short_path(const struct fat32_volume *volume, const char *path, char *out, uint32_t size) {
    const char *cursor = path;
    char component[FAT_MAX_NAME], name[16];
    uint32_t directory = volume->type == FAT_TYPE_32 ? volume->root_cluster : 0, used = 0;
    struct directory_entry entry;
    int status;
    while ((status = path_component(&cursor, component)) > 0) {
        if (find_in_directory(volume, directory, component, &entry) != 0) return -1;
        directory = ((uint32_t)get16(entry.data + 20) << 16) | get16(entry.data + 26);
        short_name(entry.data, name);
        uint32_t n = string_length(name);
        if (used + n + 2 > size) return -1;
        out[used++] = '\\';
        for (uint32_t i = 0; i < n; ++i) out[used++] = name[i];
    }
    if (status < 0) return -1;
    if (!used) { if (size < 2) return -1; out[used++] = '\\'; }
    out[used] = 0;
    return 0;
}

static int parent_path(const struct fat32_volume *volume, const char *path,
                       uint32_t *directory, char *leaf) {
    char components[FAT_MAX_NAME];
    const char *cursor = path;
    const char *last = path;
    int status;
    *directory = volume->type == FAT_TYPE_32 ? volume->root_cluster : 0;
    while ((status = path_component(&cursor, components)) > 0) {
        last = cursor;
        if (*cursor == 0) { for (unsigned int i = 0; components[i]; ++i) leaf[i] = components[i]; leaf[string_length(components)] = 0; return 0; }
        struct directory_entry entry;
        if (find_in_directory(volume, *directory, components, &entry) != 0 ||
            !(entry.data[11] & 0x10)) return -1;
        *directory = ((uint32_t)get16(entry.data + 20) << 16) | get16(entry.data + 26);
    }
    (void)last;
    return -1;
}

static int write_entry(const struct fat32_volume *volume, const struct directory_entry *entry) {
    if (volume->read_sector(volume->context, entry->lba, sector_buffer) != 0) return -1;
    for (unsigned int i = 0; i < 32; ++i) sector_buffer[entry->offset + i] = entry->data[i];
    return put_sector(volume, entry->lba, sector_buffer);
}

static int write_cluster(const struct fat32_volume *volume, uint32_t cluster, const unsigned char *data);

static int write_ordinal(const struct fat32_volume *volume, uint32_t directory,
                         uint32_t ordinal, const unsigned char *data) {
    struct directory_entry entry;
    if (directory_entry_at(volume, directory, ordinal, &entry) != 0) return -1;
    for (unsigned int i = 0; i < 32; ++i) entry.data[i] = data[i];
    return write_entry(volume, &entry);
}

static int extend_directory(const struct fat32_volume *volume, uint32_t directory) {
    uint32_t last = directory;
    uint32_t next;
    if (!valid_cluster(volume, directory)) return -1;
    while (next_cluster(volume, last, &next) == 0) last = next;
    if (allocate_cluster(volume, &next) != 0 || write_fat_entry(volume, last, next) != 0) return -1;
    for (uint32_t i = 0; i < volume->bytes_per_sector * volume->sectors_per_cluster; ++i) zero_cluster[i] = 0;
    return write_cluster(volume, next, zero_cluster);
}

static int find_free_ordinal(const struct fat32_volume *volume, uint32_t directory,
                             uint32_t needed, uint32_t *ordinal) {
    for (uint32_t candidate = 0; candidate < 0xffff; ++candidate) {
        int free_slots = 1;
        for (uint32_t i = 0; i < needed; ++i) {
            struct directory_entry entry;
            if (directory_entry_at(volume, directory, candidate + i, &entry) != 0) {
                /* Past the end: the root of FAT12/16 cannot grow; others get a new cluster. */
                if (directory == 0 || extend_directory(volume, directory) != 0 ||
                    directory_entry_at(volume, directory, candidate + i, &entry) != 0) return -1;
            }
            if (entry.data[0] != 0 && entry.data[0] != 0xe5) { free_slots = 0; break; }
        }
        if (free_slots) { *ordinal = candidate; return 0; }
    }
    return -1;
}

/* The 8.3 alias of NAME, unique in DIRECTORY: names that do not fit
   8.3 exactly (or are not upper case) get a numeric tail, BASE~N.EXT. */
static int alias_taken(const struct fat32_volume *volume, uint32_t directory, const unsigned char alias[11]) {
    for (uint32_t ordinal = 0; ordinal < 0xffff; ++ordinal) {
        struct directory_entry entry;
        if (directory_entry_at(volume, directory, ordinal, &entry) != 0 || !entry.data[0]) return 0;
        if (entry.data[0] == 0xe5 || entry.data[11] == 0x0f) continue;
        unsigned int i = 0;
        while (i < 11 && entry.data[i] == alias[i]) ++i;
        if (i == 11) return 1;
    }
    return 0;
}

static void make_alias(const struct fat32_volume *volume, uint32_t directory, const char *name, unsigned char alias[11]) {
    unsigned int base = 0, ext = 0, exact = 1, length = string_length(name);
    const char *dot = 0;
    for (const char *p = name; *p; ++p) if (*p == '.') dot = p;
    if (dot == name) dot = 0;
    for (unsigned int i = 0; i < 11; ++i) alias[i] = ' ';
    for (const char *p = name; *p && p != dot; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') { c -= 'a' - 'A'; exact = 0; }
        if (c == ' ' || c == '.' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') { exact = 0; continue; }
        if (base < 8) alias[base++] = (unsigned char)c; else exact = 0;
    }
    if (dot) {
        for (const char *p = dot + 1; *p; ++p) {
            char c = *p;
            if (c >= 'a' && c <= 'z') { c -= 'a' - 'A'; exact = 0; }
            if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') { exact = 0; continue; }
            if (ext < 3) alias[8 + ext++] = (unsigned char)c; else exact = 0;
        }
    }
    if (!base) { alias[0] = '_'; base = 1; exact = 0; }
    if (length > 12) exact = 0;
    if (exact && !alias_taken(volume, directory, alias)) return;
    for (unsigned int n = 1; n < 1000000; ++n) {
        char digits[8];
        unsigned int count = 0;
        for (unsigned int v = n; v; v /= 10) digits[count++] = (char)('0' + v % 10);
        unsigned int keep = base < 7 - count ? base : 7 - count;
        alias[keep] = '~';
        for (unsigned int i = 0; i < count; ++i) alias[keep + 1 + i] = (unsigned char)digits[count - 1 - i];
        for (unsigned int i = keep + 1 + count; i < 8; ++i) alias[i] = ' ';
        if (!alias_taken(volume, directory, alias)) return;
    }
}

static void set_lfn_char(unsigned char *entry, unsigned int index, uint16_t value) {
    static const unsigned char positions[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    if (index < sizeof(positions)) put16(entry + positions[index], value);
}

static unsigned char rtc_read(unsigned char reg) {
    unsigned char value;__asm__ volatile("outb %0,$0x70"::"a"(reg));__asm__ volatile("inb $0x71,%0":"=a"(value));return value;
}
static unsigned rtc_number(unsigned value,int binary){return binary?value:(value>>4)*10+(value&15);}
static void timestamp(unsigned char *entry) {
    for(unsigned n=0;n<100000;++n)if(!(rtc_read(10)&128))break;
    unsigned mode=rtc_read(11);int binary=mode&4;
    unsigned year=2000+rtc_number(rtc_read(9),binary),month=rtc_number(rtc_read(8),binary),day=rtc_number(rtc_read(7),binary);
    unsigned raw_hour=rtc_read(4),hour=rtc_number(raw_hour&127,binary),minute=rtc_number(rtc_read(2),binary),second=rtc_number(rtc_read(0),binary);
    if(!(mode&2)){if(hour==12)hour=0;if(raw_hour&128)hour+=12;}
    if(month<1||month>12||day<1||day>31)return;
    put16(entry+24,((year-1980)<<9)|(month<<5)|day);put16(entry+22,(hour<<11)|(minute<<5)|(second/2));
}

static int create_entry(const struct fat32_volume *volume, uint32_t directory,
                        const char *name, unsigned char attributes, uint32_t first_cluster,
                        uint32_t size, struct directory_entry *result) {
    unsigned char alias[11], short_entry[32], lfn_entry[32];
    unsigned int length = string_length(name);
    uint32_t lfn_count = (length + 12) / 13;
    uint32_t first_ordinal;
    make_alias(volume, directory, name, alias);
    if (find_free_ordinal(volume, directory, lfn_count + 1, &first_ordinal) != 0) return -1;
    for (unsigned int i = 0; i < 32; ++i) short_entry[i] = 0;
    for (unsigned int i = 0; i < 11; ++i) short_entry[i] = alias[i];
    short_entry[11] = attributes;
    timestamp(short_entry);
    put16(short_entry + 20, (uint16_t)(first_cluster >> 16));
    put16(short_entry + 26, (uint16_t)first_cluster);
    put32(short_entry + 28, size);
    for (uint32_t number = lfn_count; number > 0; --number) {
        for (unsigned int i = 0; i < 32; ++i) lfn_entry[i] = 0xff;
        lfn_entry[0] = (unsigned char)number;
        if (number == lfn_count) lfn_entry[0] |= 0x40;
        lfn_entry[11] = 0x0f;
        lfn_entry[12] = 0;
        put16(lfn_entry + 26, 0);
        lfn_entry[13] = short_checksum(alias);
        for (unsigned int i = 0; i < 13; ++i) {
            uint32_t index = (number - 1) * 13 + i;
            set_lfn_char(lfn_entry, i, index < length ? (unsigned char)name[index] : 0);
        }
        if (write_ordinal(volume, directory, first_ordinal + lfn_count - number, lfn_entry) != 0) return -1;
    }
    if (write_ordinal(volume, directory, first_ordinal + lfn_count, short_entry) != 0) return -1;
    if (directory_entry_at(volume, directory, first_ordinal + lfn_count, result) != 0) return -1;
    result->lfn_count = lfn_count;
    return 0;
}

static int write_cluster(const struct fat32_volume *volume, uint32_t cluster, const unsigned char *data) {
    for (uint32_t sector = 0; sector < volume->sectors_per_cluster; ++sector)
        if (put_sector(volume, cluster_lba(volume, cluster) + sector,
                                 data + sector * volume->bytes_per_sector) != 0) return -1;
    return 0;
}

int fat32_mount(struct fat32_volume *volume, uint32_t partition_lba) {
    forget_walk();
    uint32_t total, sectors_per_fat, data_sectors;
    if (!volume || !volume->read_sector || volume->read_sector(volume->context, partition_lba, sector_buffer) != 0) return -1;
    volume->bytes_per_sector = get16(sector_buffer + 11);
    volume->sectors_per_cluster = sector_buffer[13];
    volume->reserved_sectors = get16(sector_buffer + 14);
    volume->fat_count = sector_buffer[16];
    total = get16(sector_buffer + 19); if (!total) total = get32(sector_buffer + 32);
    sectors_per_fat = get16(sector_buffer + 22); if (!sectors_per_fat) sectors_per_fat = get32(sector_buffer + 36);
    if (get16(sector_buffer + 510) != 0xaa55 || volume->bytes_per_sector > FAT_MAX_SECTOR ||
        volume->bytes_per_sector < 512 || volume->sectors_per_cluster == 0 || volume->reserved_sectors == 0 ||
        volume->fat_count == 0 || !sectors_per_fat || !total) return -1;
    volume->partition_lba = partition_lba;
    volume->total_sectors = total;
    volume->fat_lba = partition_lba + volume->reserved_sectors;
    volume->sectors_per_fat = sectors_per_fat;
    volume->root_lba = volume->fat_lba + volume->fat_count * sectors_per_fat;
    volume->root_sectors = ((uint32_t)get16(sector_buffer + 17) * 32 + volume->bytes_per_sector - 1) / volume->bytes_per_sector;
    volume->data_lba = volume->root_lba + volume->root_sectors;
    data_sectors = total - volume->reserved_sectors - volume->fat_count * sectors_per_fat - volume->root_sectors;
    volume->cluster_count = data_sectors / volume->sectors_per_cluster;
    if (volume->cluster_count < 4085) volume->type = FAT_TYPE_12;
    else if (volume->cluster_count < 65525) volume->type = FAT_TYPE_16;
    else volume->type = FAT_TYPE_32;
    volume->root_cluster = volume->type == FAT_TYPE_32 ? get32(sector_buffer + 44) : 0;
    volume->free_clusters=0xffffffffu;volume->next_free=2;volume->fsinfo_sector=volume->type==FAT_TYPE_32?get16(sector_buffer+48):0;
    if(volume->type==FAT_TYPE_32 && volume->fsinfo_sector && volume->fsinfo_sector<volume->reserved_sectors &&
       !volume->read_sector(volume->context,partition_lba+volume->fsinfo_sector,sector_buffer) &&
       get32(sector_buffer)==0x41615252 && get32(sector_buffer+484)==0x61417272){
        uint32_t free=get32(sector_buffer+488);if(free<=volume->cluster_count)volume->free_clusters=free;
        uint32_t next=get32(sector_buffer+492);if(valid_cluster(volume,next))volume->next_free=next;
    }
    return volume->type == FAT_TYPE_32 && volume->root_cluster < 2 ? -1 : 0;
}

int fat32_read(const struct fat32_volume *volume, const char *path, void *buffer, uint32_t size) {
    struct directory_entry entry;
    uint32_t cluster, remaining;
    unsigned char *out = (unsigned char *)buffer;
    if (!volume || !buffer || find_path(volume, path, &entry) != 0 || (entry.data[11] & 0x10)) return -1;
    cluster = ((uint32_t)get16(entry.data + 20) << 16) | get16(entry.data + 26);
    remaining = size < get32(entry.data + 28) ? size : get32(entry.data + 28);
    while (remaining && valid_cluster(volume, cluster)) {
        for (uint32_t sector = 0; sector < volume->sectors_per_cluster && remaining; ++sector) {
            if (volume->read_sector(volume->context, cluster_lba(volume, cluster) + sector, sector_buffer) != 0) return -1;
            uint32_t count = remaining < volume->bytes_per_sector ? remaining : volume->bytes_per_sector;
            for (uint32_t i = 0; i < count; ++i) out[i] = sector_buffer[i];
            out += count; remaining -= count;
        }
        if (remaining && next_cluster(volume, cluster, &cluster) != 0) return -1;
    }
    return remaining ? -1 : 0;
}

int fat32_write(const struct fat32_volume *volume, const char *path, const void *buffer, uint32_t size) {
    struct directory_entry entry;
    if (!volume || !buffer || !volume->write_sector) return -1;
    uint32_t original_size = size;
    uint32_t cluster, previous = 0, needed = (size + volume->bytes_per_sector * volume->sectors_per_cluster - 1) /
                                               (volume->bytes_per_sector * volume->sectors_per_cluster);
    const unsigned char *input = (const unsigned char *)buffer;
    if (!volume || !buffer || !volume->write_sector) return -1;
    if (find_path(volume, path, &entry) != 0) {
        char leaf[FAT_MAX_NAME];
        uint32_t parent;
        if (parent_path(volume, path, &parent, leaf) != 0 ||
            create_entry(volume, parent, leaf, 0x20, 0, 0, &entry) != 0) return -1;
    }
    if (entry.data[11] & 0x10) return -1;
    cluster = ((uint32_t)get16(entry.data + 20) << 16) | get16(entry.data + 26);
    if (cluster >= 2 && free_chain(volume, cluster) != 0) return -1;
    cluster = 0;
    for (uint32_t allocated = 0; allocated < needed; ++allocated) {
        uint32_t next;
        if (allocate_cluster(volume, &next) != 0) return -1;
        if (!cluster) cluster = next;
        if (previous && write_fat_entry(volume, previous, next) != 0) return -1;
        previous = next;
        for (uint32_t i = 0; i < volume->bytes_per_sector * volume->sectors_per_cluster; ++i)
            zero_cluster[i] = i < size ? input[i] : 0;
        if (size > volume->bytes_per_sector * volume->sectors_per_cluster) size -= volume->bytes_per_sector * volume->sectors_per_cluster;
        else size = 0;
        if (write_cluster(volume, next, zero_cluster) != 0) return -1;
        input += volume->bytes_per_sector * volume->sectors_per_cluster;
    }
    put16(entry.data + 20, (uint16_t)(cluster >> 16)); entry.data[21] = (unsigned char)(cluster >> 24);
    put16(entry.data + 26, (uint16_t)cluster); put32(entry.data + 28, original_size);
    timestamp(entry.data);
    return write_entry(volume, &entry);
}

int fat32_mkdir(const struct fat32_volume *volume, const char *path) {
    char leaf[FAT_MAX_NAME];
    uint32_t parent, cluster;
    struct directory_entry entry;
    if (!volume || !volume->write_sector || parent_path(volume, path, &parent, leaf) != 0 ||
        find_in_directory(volume, parent, leaf, &entry) == 0 ||
        allocate_cluster(volume, &cluster) != 0) return -1;
    for (uint32_t i = 0; i < volume->bytes_per_sector * volume->sectors_per_cluster; ++i) zero_cluster[i] = 0;
    if (write_cluster(volume, cluster, zero_cluster) != 0) return -1;
    if (create_entry(volume, parent, leaf, 0x10, cluster, 0, &entry) != 0) return -1;
    unsigned char dot[32];
    for (unsigned int i = 0; i < 32; ++i) dot[i] = 0;
    for(unsigned i=0;i<11;++i)dot[i]=' ';
    dot[0] = '.'; dot[11] = 0x10; put16(dot + 20, cluster >> 16); put16(dot + 26, cluster);
    if (write_ordinal(volume, cluster, 0, dot) != 0) return -1;
    for (unsigned int i = 0; i < 32; ++i) dot[i] = 0;
    for(unsigned i=0;i<11;++i)dot[i]=' ';
    dot[0] = '.'; dot[1] = '.'; dot[11] = 0x10;
    if(volume->type==FAT_TYPE_32&&parent==volume->root_cluster)parent=0;
    put16(dot + 20, parent >> 16); put16(dot + 26, parent);
    return write_ordinal(volume, cluster, 1, dot);
}

int fat32_delete(const struct fat32_volume *volume, const char *path) {
    struct directory_entry entry;
    uint32_t cluster;
    if (!volume || !volume->write_sector || find_path(volume, path, &entry) != 0 ||
        (entry.data[11] & 0x08) || (entry.data[11] & 0x10)) return -1;
    cluster = ((uint32_t)get16(entry.data + 20) << 16) | get16(entry.data + 26);
    if (cluster >= 2 && free_chain(volume, cluster) != 0) return -1;
    for (uint32_t i = 0; i <= entry.lfn_count; ++i) {
        struct directory_entry old;
        if (entry.ordinal < i || directory_entry_at(volume, entry.directory_cluster,
                                                     entry.ordinal - i, &old) != 0) return -1;
        if (volume->read_sector(volume->context, old.lba, sector_buffer) != 0) return -1;
        sector_buffer[old.offset] = 0xe5;
        if (put_sector(volume, old.lba, sector_buffer) != 0) return -1;
    }
    return 0;
}

static void info_from_entry(const struct directory_entry *entry, struct fat32_info *info) {
    short_name(entry->data, info->name);
    info->size=get32(entry->data+28);info->attributes=entry->data[11];
    info->date=get16(entry->data+24);info->time=get16(entry->data+22);
}
int fat32_stat(const struct fat32_volume *volume,const char *path,struct fat32_info *info) {
    if(!volume||!path||!info)return -1;
    while(*path=='/'||*path=='\\')++path;
    if(!*path){info->name[0]=0;info->size=0;info->attributes=16;info->date=info->time=0;return 0;}
    struct directory_entry entry;if(find_path(volume,path,&entry))return -1;
    info_from_entry(&entry,info);return 0;
}
int fat32_list(const struct fat32_volume *volume,const char *path,fat32_list_fn callback,void *context) {
    if(!volume||!callback)return -1;
    uint32_t directory=volume->type==FAT_TYPE_32?volume->root_cluster:0;
    while(*path=='/'||*path=='\\')++path;
    if(*path){struct directory_entry entry;if(find_path(volume,path,&entry)||!(entry.data[11]&16))return -1;
        directory=((uint32_t)get16(entry.data+20)<<16)|get16(entry.data+26);}
    char long_name[260];uint32_t count=0;
    for(uint32_t n=0;n<65535;++n){struct directory_entry entry;if(directory_entry_at(volume,directory,n,&entry))return 0;
        if(!entry.data[0])return 0;if(entry.data[0]==0xe5){count=0;continue;}
        if(entry.data[11]==15){unsigned order=entry.data[0]&31;if(entry.data[0]&64){count=0;for(unsigned i=0;i<260;++i)long_name[i]=0;}
            if(order&&order<=20){lfn_chars(entry.data,long_name,(order-1)*13);count=order>count?order:count;}continue;}
        if(entry.data[11]&8){count=0;continue;}
        struct fat32_info info;info_from_entry(&entry,&info);
        if(count){for(unsigned i=0;i<259;++i){info.name[i]=long_name[i];if(!long_name[i])break;}info.name[259]=0;}
        count=0;if(callback(context,&info))return 0;
    }
    return -1;
}
static int erase_directory_entry(const struct fat32_volume *volume,const struct directory_entry *entry) {
    for(uint32_t i=0;i<=entry->lfn_count;++i){struct directory_entry old;
        if(entry->ordinal<i||directory_entry_at(volume,entry->directory_cluster,entry->ordinal-i,&old))return -1;
        old.data[0]=0xe5;if(write_entry(volume,&old))return -1;
    }return 0;
}
int fat32_rename(const struct fat32_volume *volume,const char *source,const char *destination) {
    struct directory_entry from,to;if(!volume||!volume->write_sector||find_path(volume,source,&from)||!find_path(volume,destination,&to))return -1;
    char leaf[260];uint32_t parent;if(parent_path(volume,destination,&parent,leaf))return -1;
    uint32_t cluster=((uint32_t)get16(from.data+20)<<16)|get16(from.data+26);
    if(from.data[11]&16){unsigned i=0;while(source[i]&&destination[i]&&source[i]==destination[i])++i;if(!source[i]&&(destination[i]=='/'||destination[i]=='\\'))return -1;}
    if(create_entry(volume,parent,leaf,from.data[11],cluster,get32(from.data+28),&to))return -1;
    if(erase_directory_entry(volume,&from))return -1;
    if(from.data[11]&16){struct directory_entry dotdot;if(directory_entry_at(volume,cluster,1,&dotdot))return -1;
        put16(dotdot.data+20,parent>>16);put16(dotdot.data+26,parent);if(write_entry(volume,&dotdot))return -1;}
    return 0;
}
int fat32_rmdir(const struct fat32_volume *volume,const char *path) {
    struct directory_entry entry;if(!volume||!volume->write_sector||find_path(volume,path,&entry)||!(entry.data[11]&16))return -1;
    uint32_t cluster=((uint32_t)get16(entry.data+20)<<16)|get16(entry.data+26);
    for(uint32_t n=0;n<65535;++n){struct directory_entry child;if(directory_entry_at(volume,cluster,n,&child))return -1;
        if(!child.data[0])break;if(child.data[0]==0xe5||child.data[0]=='.'||child.data[11]==15)continue;return -1;}
    if(free_chain(volume,cluster))return -1;return erase_directory_entry(volume,&entry);
}
uint64_t fat32_free_bytes(const struct fat32_volume *volume) {
    if(!volume)return 0;
    if(volume->free_clusters!=0xffffffffu)return (uint64_t)volume->free_clusters*volume->bytes_per_sector*volume->sectors_per_cluster;
    uint32_t free=0,entry_bytes=volume->type==FAT_TYPE_32?4:2;
    if(volume->type==FAT_TYPE_12)return 0;
    for(uint32_t s=0;s<volume->sectors_per_fat;++s){if(volume->read_sector(volume->context,volume->fat_lba+s,fat_buffer))return 0;
        for(uint32_t o=0;o<volume->bytes_per_sector;o+=entry_bytes){uint32_t cluster=(s*volume->bytes_per_sector+o)/entry_bytes;
            uint32_t value=entry_bytes==4?get32(fat_buffer+o)&0x0fffffff:get16(fat_buffer+o);
            if(cluster>=2&&cluster<volume->cluster_count+2&&!value)++free;}}
    ((struct fat32_volume *)volume)->free_clusters=free;
    return (uint64_t)free*volume->bytes_per_sector*volume->sectors_per_cluster;
}

int fat32_copy(const struct fat32_volume *volume,const char *source,const char *destination) {
    struct directory_entry from,to;
    if(!volume||!volume->write_sector||find_path(volume,source,&from)||(from.data[11]&16))return -1;
    int exists=!find_path(volume,destination,&to);
    if(exists&&((to.data[11]&16)||(to.lba==from.lba&&to.offset==from.offset)))return -1;
    uint32_t remaining=get32(from.data+28),current=((uint32_t)get16(from.data+20)<<16)|get16(from.data+26);
    uint32_t first=0,previous=0,cluster_bytes=volume->bytes_per_sector*volume->sectors_per_cluster;
    while(remaining){
        if(!valid_cluster(volume,current))goto failed;
        for(uint32_t s=0;s<volume->sectors_per_cluster;++s)
            if(volume->read_sector(volume->context,cluster_lba(volume,current)+s,zero_cluster+s*volume->bytes_per_sector))goto failed;
        uint32_t next_source=0;if(remaining>cluster_bytes&&next_cluster(volume,current,&next_source))goto failed;
        uint32_t allocated;if(allocate_cluster(volume,&allocated))goto failed;
        if(!first)first=allocated;
        if(previous&&write_fat_entry(volume,previous,allocated))goto failed;
        previous=allocated;if(write_cluster(volume,allocated,zero_cluster))goto failed;
        remaining=remaining>cluster_bytes?remaining-cluster_bytes:0;current=next_source;
    }
    uint32_t old=exists?((uint32_t)get16(to.data+20)<<16)|get16(to.data+26):0;
    if(!exists){char leaf[260];uint32_t parent;if(parent_path(volume,destination,&parent,leaf)||create_entry(volume,parent,leaf,32,first,get32(from.data+28),&to))goto failed;}
    else{put16(to.data+20,first>>16);put16(to.data+26,first);put32(to.data+28,get32(from.data+28));timestamp(to.data);if(write_entry(volume,&to))goto failed;}
    if(old>=2&&free_chain(volume,old))return -1;return 0;
failed:
    if(first)free_chain(volume,first);return -1;
}

int fat32_read_at(const struct fat32_volume *v,const char *path,uint32_t offset,void *buffer,uint32_t size){
    struct directory_entry e;if(find_path(v,path,&e)||(e.data[11]&16)||offset>get32(e.data+28)||size>get32(e.data+28)-offset)return -1;
    uint32_t cluster=((uint32_t)get16(e.data+20)<<16)|get16(e.data+26),bytes=v->bytes_per_sector*v->sectors_per_cluster;
    while(offset>=bytes){if(next_cluster(v,cluster,&cluster))return -1;offset-=bytes;}
    unsigned char *out=buffer;
    while(size){if(!valid_cluster(v,cluster))return -1;uint32_t sector=offset/v->bytes_per_sector,within=offset%v->bytes_per_sector;
        for(;sector<v->sectors_per_cluster&&size;++sector){if(v->read_sector(v->context,cluster_lba(v,cluster)+sector,sector_buffer))return -1;
            uint32_t n=v->bytes_per_sector-within;if(n>size)n=size;for(uint32_t i=0;i<n;++i)*out++=sector_buffer[within+i];size-=n;within=0;}
        offset=0;if(size&&next_cluster(v,cluster,&cluster))return -1;
    }return 0;
}
int fat32_file_lba(const struct fat32_volume *v,const char *path,uint32_t *lba,uint32_t *size){
    struct directory_entry e;if(find_path(v,path,&e))return -1;
    uint32_t cluster=((uint32_t)get16(e.data+20)<<16)|get16(e.data+26);*size=get32(e.data+28);if(!*size){*lba=0;return 0;}if(!valid_cluster(v,cluster))return -1;
    *lba=cluster_lba(v,cluster);return 0;
}
int fat32_copy_to(const struct fat32_volume *source,const char *from,const struct fat32_volume *target,const char *to){
    struct directory_entry e,existing;if(find_path(source,from,&e)||(e.data[11]&16)||!target->write_sector||!find_path(target,to,&existing))return -1;
    uint32_t size=get32(e.data+28),offset=0,first=0,previous=0,bytes=target->bytes_per_sector*target->sectors_per_cluster;
    while(offset<size){uint32_t n=size-offset;if(n>bytes)n=bytes;
        for(uint32_t i=0;i<bytes;++i)zero_cluster[i]=0;
        if(fat32_read_at(source,from,offset,zero_cluster,n))goto failed;
        uint32_t next;if(allocate_cluster(target,&next))goto failed;
        if(!first)first=next;if(previous&&write_fat_entry(target,previous,next))goto failed;previous=next;
        if(write_cluster(target,next,zero_cluster))goto failed;offset+=n;
    }
    char leaf[260];uint32_t parent;if(parent_path(target,to,&parent,leaf)||create_entry(target,parent,leaf,32,first,size,&existing))goto failed;
    return 0;
failed:
    if(first)free_chain(target,first);return -1;
}

/* Sequential reads: the cursor remembers the cluster reached last time, so
   reading a file front to back walks its cluster chain once. */
int fat32_read_cursor(const struct fat32_volume *v, const char *path, struct fat32_cursor *c,
                      uint32_t offset, void *buffer, uint32_t size) {
    if (!c->size_known) {
        struct directory_entry e;
        if (find_path(v, path, &e) || (e.data[11] & 16)) return -1;
        c->first = ((uint32_t)get16(e.data + 20) << 16) | get16(e.data + 26);
        c->size = get32(e.data + 28);
        c->cluster = c->first;
        c->cluster_offset = 0;
        c->size_known = 1;
    }
    if (offset > c->size || size > c->size - offset) return -1;
    if (!size) return 0;
    uint32_t bytes = v->bytes_per_sector * v->sectors_per_cluster;
    if (offset < c->cluster_offset || !valid_cluster(v, c->cluster)) { c->cluster = c->first; c->cluster_offset = 0; }
    while (offset - c->cluster_offset >= bytes) {
        if (next_cluster(v, c->cluster, &c->cluster)) { c->cluster = c->first; c->cluster_offset = 0; return -1; }
        c->cluster_offset += bytes;
    }
    unsigned char *out = buffer;
    uint32_t within = offset - c->cluster_offset;
    while (size) {
        if (!valid_cluster(v, c->cluster)) return -1;
        uint32_t sector = within / v->bytes_per_sector, skip = within % v->bytes_per_sector;
        for (; sector < v->sectors_per_cluster && size; ++sector) {
            uint32_t lba = cluster_lba(v, c->cluster) + sector;
            if (!skip && size >= v->bytes_per_sector) {
                if (v->read_sector(v->context, lba, out)) return -1;
                out += v->bytes_per_sector;
                size -= v->bytes_per_sector;
                continue;
            }
            if (v->read_sector(v->context, lba, sector_buffer)) return -1;
            uint32_t n = v->bytes_per_sector - skip;
            if (n > size) n = size;
            for (uint32_t i = 0; i < n; ++i) *out++ = sector_buffer[skip + i];
            size -= n;
            skip = 0;
        }
        within = 0;
        if (size) {
            if (next_cluster(v, c->cluster, &c->cluster)) return -1;
            c->cluster_offset += bytes;
        }
    }
    return 0;
}
