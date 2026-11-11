/*
 * DOS-XC /sound/hda.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include <dosxc/driver.h>

#define GCAP 0x00
#define GCTL 0x08
#define STATESTS 0x0e
#define INTCTL 0x20
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP 0x48
#define CORBRP 0x4a
#define CORBCTL 0x4c
#define CORBSIZE 0x4e
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP 0x58
#define RINTCNT 0x5a
#define RIRBCTL 0x5c
#define RIRBSTS 0x5d
#define RIRBSIZE 0x5e

#define SD_CTL 0x00
#define SD_STS 0x03
#define SD_LPIB 0x04
#define SD_CBL 0x08
#define SD_LVI 0x0c
#define SD_FMT 0x12
#define SD_BDLPL 0x18
#define SD_BDLPU 0x1c

#define VERB_PARAMETER 0xf0000u
#define VERB_CONNECTIONS 0xf0200u
#define VERB_SELECT 0x70100u
#define VERB_POWER 0x70500u
#define VERB_STREAM 0x70600u
#define VERB_PIN_CONTROL 0x70700u
#define VERB_EAPD 0x70c00u
#define VERB_PIN_CONFIG 0xf1c00u
#define VERB_FORMAT 0x20000u
#define VERB_AMP 0x30000u

#define PARAM_NODES 0x04
#define PARAM_FUNCTION 0x05
#define PARAM_WIDGET 0x09
#define PARAM_PIN 0x0c
#define PARAM_AMP_IN 0x0d
#define PARAM_CONNECTIONS 0x0e
#define PARAM_AMP_OUT 0x12

#define WIDGET_OUTPUT 0
#define WIDGET_MIXER 2
#define WIDGET_SELECTOR 3
#define WIDGET_PIN 4

#define FORMAT_48K_STEREO_16 0x0011u
#define STREAM_TAG 1u
#define RING_BYTES 65536u
#define RING_FRAMES (RING_BYTES / 4u)
#define CHUNK_BYTES (RING_BYTES / 2u)
#define MAX_DEPTH 6

struct bdl_entry { dosxc_u64 address; dosxc_u32 length, flags; };

struct hda {
    volatile void *regs;
    volatile void *stream;
    struct dk_pci_device pci;
    volatile dosxc_u32 *corb;
    volatile dosxc_u32 *rirb;
    unsigned corb_entries, rirb_entries, rirb_read;
    unsigned char *ring;
    struct bdl_entry *bdl;
    int handle, is_open, running;
    dosxc_u64 written, played, cleared, opened_at;
    unsigned last_position;
    unsigned dac_codec, dacs[16], dac_count;   /* one codec's converters in use */
};

static const struct dk_api *dk;

static int wait_bit8(volatile void *r, unsigned offset, unsigned char mask, int set) {
    dosxc_u64 deadline = dk->nanoseconds() + 100000000ull;
    while (!!(dk_read8(r, offset) & mask) != set)
        if (dk->nanoseconds() > deadline) return -1;
    return 0;
}

static int command(struct hda *a, unsigned codec, unsigned node, dosxc_u32 verb, dosxc_u32 *response) {
    unsigned at = (dk_read16(a->regs, CORBWP) + 1u) % a->corb_entries;
    a->corb[at] = (dosxc_u32)codec << 28 | (dosxc_u32)node << 20 | verb;
    dk_barrier();
    dk_write16(a->regs, CORBWP, (unsigned short)at);
    dosxc_u64 deadline = dk->nanoseconds() + 200000000ull;
    for (;;) {
        unsigned wp = dk_read16(a->regs, RIRBWP) & 0xffu;
        while (a->rirb_read != wp) {
            a->rirb_read = (a->rirb_read + 1u) % a->rirb_entries;
            dosxc_u32 value = a->rirb[a->rirb_read * 2u], extra = a->rirb[a->rirb_read * 2u + 1u];
            dk_write8(a->regs, RIRBSTS, 5);
            if (extra & 0x10u) continue;
            if (response) *response = value;
            return 0;
        }
        if (dk->nanoseconds() > deadline) return -1;
    }
}

static dosxc_u32 parameter(struct hda *a, unsigned codec, unsigned node, unsigned which) {
    dosxc_u32 value = 0;
    command(a, codec, node, VERB_PARAMETER | which, &value);
    return value;
}

static unsigned widget_type(struct hda *a, unsigned codec, unsigned node) {
    return parameter(a, codec, node, PARAM_WIDGET) >> 20 & 15u;
}

static unsigned connections(struct hda *a, unsigned codec, unsigned node, unsigned *out, unsigned capacity) {
    dosxc_u32 info = parameter(a, codec, node, PARAM_CONNECTIONS);
    unsigned count = info & 0x7fu, wide = info >> 7 & 1u, per = wide ? 2u : 4u, bits = wide ? 16u : 8u, found = 0;
    for (unsigned i = 0; i < count && found < capacity; i += per) {
        dosxc_u32 block = 0;
        if (command(a, codec, node, VERB_CONNECTIONS | i, &block)) break;
        for (unsigned j = 0; j < per && i + j < count && found < capacity; ++j)
            out[found++] = block >> (j * bits) & ((1u << bits) - 1u);
    }
    return found;
}

/* Full output level (0 dB) and no mute, on the output and input amplifiers. */
static void open_amplifier(struct hda *a, unsigned codec, unsigned afg, unsigned node, int input, unsigned index) {
    dosxc_u32 caps = parameter(a, codec, node, input ? PARAM_AMP_IN : PARAM_AMP_OUT);
    if (!caps) caps = parameter(a, codec, afg, input ? PARAM_AMP_IN : PARAM_AMP_OUT);
    unsigned steps = caps >> 8 & 0x7fu, gain = caps & 0x7fu;
    if (gain > steps) gain = steps;
    command(a, codec, node, VERB_AMP | (input ? 0x7000u | index << 8 : 0xb000u) | gain, 0);
}

/* Connect NODE back to a converter.  Returns the converter's node, or 0. */
static unsigned find_dac(struct hda *a, unsigned codec, unsigned afg, unsigned node, unsigned preferred, int depth) {
    unsigned type = widget_type(a, codec, node);
    if (type == WIDGET_OUTPUT) return !preferred || preferred == node ? node : 0;
    if (depth >= MAX_DEPTH || (type != WIDGET_MIXER && type != WIDGET_SELECTOR && type != WIDGET_PIN)) return 0;
    unsigned list[32], count = connections(a, codec, node, list, 32);
    for (unsigned i = 0; i < count; ++i) {
        unsigned dac = find_dac(a, codec, afg, list[i], preferred, depth + 1);
        if (!dac) continue;
        command(a, codec, node, VERB_POWER, 0);
        if (type != WIDGET_MIXER && count > 1) command(a, codec, node, VERB_SELECT | i, 0);
        if (type == WIDGET_MIXER) open_amplifier(a, codec, afg, node, 1, i);
        open_amplifier(a, codec, afg, node, 0, 0);
        return dac;
    }
    return 0;
}

static void use_dac(struct hda *a, unsigned dac) {
    for (unsigned i = 0; i < a->dac_count; ++i) if (a->dacs[i] == dac) return;
    if (a->dac_count < 16) a->dacs[a->dac_count++] = dac;
}

/* Connect every output pin of this function group.  Returns how many. */
static unsigned setup_function(struct hda *a, unsigned codec, unsigned afg) {
    dosxc_u32 nodes = parameter(a, codec, afg, PARAM_NODES);
    unsigned start = nodes >> 16 & 0xffu, count = nodes & 0xffu, pins = 0, dac = 0;
    command(a, codec, afg, VERB_POWER, 0);
    dk->sleep(20000000ull);
    for (unsigned n = start; n < start + count; ++n) {
        if (widget_type(a, codec, n) != WIDGET_PIN) continue;
        dosxc_u32 caps = parameter(a, codec, n, PARAM_PIN), config = 0;
        if (!(caps & 0x10u)) continue;
        command(a, codec, n, VERB_PIN_CONFIG, &config);
        if ((config >> 30) == 1u) continue;
        unsigned device = config >> 20 & 15u;
        if (device > 2u) continue;
        unsigned found = find_dac(a, codec, afg, n, dac, 0);
        if (!found) found = find_dac(a, codec, afg, n, 0, 0);
        if (!found) continue;
        dac = found;
        use_dac(a, found);
        command(a, codec, found, VERB_POWER, 0);
        open_amplifier(a, codec, afg, found, 0, 0);
        command(a, codec, n, VERB_POWER, 0);
        command(a, codec, n, VERB_PIN_CONTROL | 0xc0u, 0);
        if (caps & 0x10000u) command(a, codec, n, VERB_EAPD | 2u, 0);
        ++pins;
    }
    return pins;
}

static unsigned setup_codec(struct hda *a, unsigned codec) {
    dosxc_u32 nodes = parameter(a, codec, 0, PARAM_NODES);
    unsigned start = nodes >> 16 & 0xffu, count = nodes & 0xffu;
    for (unsigned n = start; n < start + count; ++n) {
        if ((parameter(a, codec, n, PARAM_FUNCTION) & 0xffu) != 1u) continue;
        unsigned pins = setup_function(a, codec, n);
        if (pins) return pins;
    }
    return 0;
}

/* ---- the stream ---------------------------------------------------------- */

static void stop_stream(struct hda *a) {
    dk_write8(a->stream, SD_CTL, 0);
    wait_bit8(a->stream, SD_CTL, 2, 0);
    a->running = 0;
}

static int reset_stream(struct hda *a) {
    stop_stream(a);
    dk_write8(a->stream, SD_CTL, 1);
    if (wait_bit8(a->stream, SD_CTL, 1, 1)) return -1;
    dk_write8(a->stream, SD_CTL, 0);
    if (wait_bit8(a->stream, SD_CTL, 1, 0)) return -1;
    dk_write8(a->stream, SD_STS, 0x1c);
    dk_write32(a->stream, SD_CBL, RING_BYTES);
    dk_write16(a->stream, SD_LVI, 1);
    dk_write16(a->stream, SD_FMT, FORMAT_48K_STEREO_16);
    dosxc_u64 list = dk->dma_address(a->bdl);
    dk_write32(a->stream, SD_BDLPL, (dosxc_u32)list);
    dk_write32(a->stream, SD_BDLPU, (dosxc_u32)(list >> 32));
    dk_write8(a->stream, SD_CTL + 2, STREAM_TAG << 4);
    for (unsigned i = 0; i < a->dac_count; ++i) {
        command(a, a->dac_codec, a->dacs[i], VERB_FORMAT | FORMAT_48K_STEREO_16, 0);
        command(a, a->dac_codec, a->dacs[i], VERB_STREAM | STREAM_TAG << 4, 0);
    }
    return 0;
}

static void clear_frames(struct hda *a, dosxc_u64 from, dosxc_u64 to) {
    while (from < to) {
        unsigned at = (unsigned)(from % RING_FRAMES), n = RING_FRAMES - at;
        if ((dosxc_u64)n > to - from) n = (unsigned)(to - from);
        for (unsigned i = 0; i < n * 4u; ++i) a->ring[at * 4u + i] = 0;
        from += n;
    }
}

static void update_position(struct hda *a) {
    if (!a->running) return;
    unsigned position = dk_read32(a->stream, SD_LPIB) & (RING_BYTES - 1u);
    unsigned moved = (position + RING_BYTES - a->last_position) & (RING_BYTES - 1u);
    a->last_position = position;
    a->played += moved / 4u;
    if (a->played > a->cleared) {
        clear_frames(a, a->cleared, a->played);
        a->cleared = a->played;
    }
    if (a->played > a->written) a->written = a->played;
}

static int start_running(struct hda *a) {
    if (a->running) return 0;
    a->running = 1;
    a->last_position = dk_read32(a->stream, SD_LPIB) & (RING_BYTES - 1u);
    dk_write8(a->stream, SD_CTL, 2);
    return 0;
}

static int audio_open(void *instance, unsigned rate, unsigned channels) {
    struct hda *a = instance;
    if (rate != 48000u || channels != 2u) return -DOSXC_EINVAL;
    if (a->is_open) return -DOSXC_EBUSY;
    if (reset_stream(a)) return -DOSXC_EIO;
    for (unsigned i = 0; i < RING_BYTES; ++i) a->ring[i] = 0;
    a->written = a->played = a->cleared = 0;
    a->last_position = 0;
    a->opened_at = dk->nanoseconds();
    a->is_open = 1;
    return 0;
}

static void audio_close(void *instance) {
    struct hda *a = instance;
    if (!a->is_open) return;
    stop_stream(a);
    a->is_open = 0;
}

static unsigned audio_write(void *instance, const void *samples, unsigned frames) {
    struct hda *a = instance;
    if (!a->is_open) return 0;
    update_position(a);
    dosxc_u64 buffered = a->written - a->played;
    unsigned room = buffered >= RING_FRAMES - 256u ? 0 : RING_FRAMES - 256u - (unsigned)buffered;
    if (frames > room) frames = room;
    const unsigned char *from = samples;
    for (unsigned done = 0; done < frames;) {
        unsigned at = (unsigned)(a->written % RING_FRAMES), n = RING_FRAMES - at;
        if (n > frames - done) n = frames - done;
        for (unsigned i = 0; i < n * 4u; ++i) a->ring[at * 4u + i] = from[i];
        from += n * 4u;
        done += n;
        a->written += n;
    }
    dk_barrier();
    if (a->written >= 2048u) start_running(a);
    return frames;
}

static dosxc_u64 audio_position(void *instance) {
    struct hda *a = instance;
    update_position(a);
    return a->played;
}

static const struct dk_audio_ops audio_ops = {audio_open, audio_close, audio_write, audio_position};

/* ---- the controller ------------------------------------------------------ */

static int start_rings(struct hda *a) {
    unsigned char corb = dk_read8(a->regs, CORBSIZE), rirb = dk_read8(a->regs, RIRBSIZE);
    unsigned corb_code = corb & 0x40 ? 2 : corb & 0x20 ? 1 : 0, rirb_code = rirb & 0x40 ? 2 : rirb & 0x20 ? 1 : 0;
    static const unsigned entries[3] = {2, 16, 256};
    a->corb_entries = entries[corb_code];
    a->rirb_entries = entries[rirb_code];
    a->corb = dk->dma_alloc(1024, 1024);
    a->rirb = dk->dma_alloc(2048, 2048);
    if (!a->corb || !a->rirb) return -1;

    dk_write8(a->regs, CORBCTL, 0);
    wait_bit8(a->regs, CORBCTL, 2, 0);
    dk_write8(a->regs, RIRBCTL, 0);
    wait_bit8(a->regs, RIRBCTL, 2, 0);
    dk_write8(a->regs, CORBSIZE, (unsigned char)((corb & 0xf0) | corb_code));
    dk_write8(a->regs, RIRBSIZE, (unsigned char)((rirb & 0xf0) | rirb_code));

    dosxc_u64 address = dk->dma_address((void *)a->corb);
    dk_write32(a->regs, CORBLBASE, (dosxc_u32)address);
    dk_write32(a->regs, CORBUBASE, (dosxc_u32)(address >> 32));
    dk_write16(a->regs, CORBRP, 0x8000);
    dosxc_u64 deadline = dk->nanoseconds() + 10000000ull;
    while (!(dk_read16(a->regs, CORBRP) & 0x8000) && dk->nanoseconds() < deadline) {}
    dk_write16(a->regs, CORBRP, 0);
    deadline = dk->nanoseconds() + 10000000ull;
    while ((dk_read16(a->regs, CORBRP) & 0x8000) && dk->nanoseconds() < deadline) {}
    dk_write16(a->regs, CORBWP, 0);

    address = dk->dma_address((void *)a->rirb);
    dk_write32(a->regs, RIRBLBASE, (dosxc_u32)address);
    dk_write32(a->regs, RIRBUBASE, (dosxc_u32)(address >> 32));
    dk_write16(a->regs, RIRBWP, 0x8000);
    dk_write16(a->regs, RINTCNT, 0xff);
    a->rirb_read = 0;
    dk_write8(a->regs, RIRBCTL, 2);
    dk_write8(a->regs, CORBCTL, 2);
    return 0;
}

static void release(struct hda *a) {
    if (a->ring) dk->dma_free(a->ring);
    if (a->bdl) dk->dma_free(a->bdl);
    if (a->corb) dk->dma_free((void *)a->corb);
    if (a->rirb) dk->dma_free((void *)a->rirb);
    dk->free(a);
}

static int hda_load(const struct dk_api *api) {
    dk = api;
    return 0;
}

static int hda_probe(const struct dk_pci_device *device, void **instance) {
    if (dk->pci_read32(device, 0x08) >> 16 != 0x0403u) return -DOSXC_ENODEV;   /* class: HD Audio */
    int io = 0;
    dosxc_u64 bar = dk->pci_bar(device, 0, &io);
    if (!bar || io) return -DOSXC_ENODEV;
    dk->pci_enable(device, 1, 0);
    struct hda *a = dk->alloc(sizeof(*a));
    if (!a) return -DOSXC_ENOMEM;
    a->pci = *device;
    a->regs = dk->mmio_map(bar, 0x4000);
    if (!a->regs) { dk->free(a); return -DOSXC_ENODEV; }

    dk_write32(a->regs, GCTL, 0);
    dosxc_u64 deadline = dk->nanoseconds() + 100000000ull;
    while ((dk_read32(a->regs, GCTL) & 1u) && dk->nanoseconds() < deadline) {}
    dk->sleep(1000000ull);
    dk_write32(a->regs, GCTL, 1);
    deadline = dk->nanoseconds() + 100000000ull;
    while (!(dk_read32(a->regs, GCTL) & 1u) && dk->nanoseconds() < deadline) {}
    if (!(dk_read32(a->regs, GCTL) & 1u)) { dk->log("HDA: the controller did not leave reset"); dk->free(a); return -DOSXC_EIO; }
    dk->sleep(2000000ull);
    dk_write32(a->regs, INTCTL, 0);

    if (start_rings(a)) { release(a); return -DOSXC_ENOMEM; }
    unsigned codecs = dk_read16(a->regs, STATESTS), pins = 0;
    for (unsigned c = 0; c < 15u && !pins; ++c)
        if (codecs & 1u << c) {
            a->dac_count = 0;
            a->dac_codec = c;
            pins = setup_codec(a, c);
        }
    if (!pins) { dk->log("HDA: no output found on the codecs (%x)", codecs); release(a); return -DOSXC_ENODEV; }

    unsigned caps = dk_read16(a->regs, GCAP), inputs = caps >> 8 & 15u, outputs = caps >> 12 & 15u;
    if (!outputs) { dk->log("HDA: the controller has no output streams"); release(a); return -DOSXC_ENODEV; }
    a->stream = (volatile char *)a->regs + 0x80 + inputs * 0x20;

    a->ring = dk->dma_alloc(RING_BYTES, 4096);
    a->bdl = dk->dma_alloc(4096, 128);
    if (!a->ring || !a->bdl) { release(a); return -DOSXC_ENOMEM; }
    dosxc_u64 ring = dk->dma_address(a->ring);
    a->bdl[0].address = ring;
    a->bdl[0].length = CHUNK_BYTES;
    a->bdl[1].address = ring + CHUNK_BYTES;
    a->bdl[1].length = CHUNK_BYTES;

    a->handle = dk->audio_register(&audio_ops, a, "HD Audio");
    if (a->handle < 0) { release(a); return a->handle; }
    dk->log("HDA: ready, %u output pin(s)", pins);
    *instance = a;
    return 0;
}

static void hda_poll(void *instance) {
    struct hda *a = instance;
    if (!a->is_open) return;
    update_position(a);
    if (!a->running && a->written && dk->nanoseconds() - a->opened_at > 200000000ull) start_running(a);
}

static void hda_remove(void *instance) {
    struct hda *a = instance;
    dk->audio_unregister(a->handle);
    stop_stream(a);
    dk_write8(a->regs, CORBCTL, 0);
    dk_write8(a->regs, RIRBCTL, 0);
    dk_write32(a->regs, GCTL, 0);
    release(a);
}

static const struct dk_pci_id ids[] = {
    {0x8086, DK_ANY}, {0x1002, DK_ANY}, {0x1022, DK_ANY}, {0x10de, DK_ANY}, {0x1106, DK_ANY}, {0, 0},
};

DK_DRIVER = {
    DK_ABI_VERSION, sizeof(struct dk_driver), "HDA", "1.0", "Intel High Definition Audio", ids,
    hda_load, hda_probe, hda_poll, hda_remove, 0,
};
