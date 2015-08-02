/*
 * disk/amigados.c
 *
 * AmigaDOS disk format.
 *
 * Written in 2011 by Keir Fraser
 *
 * RAW TRACK LAYOUT:
 *  544 decoded bytes per sector (including sector gap).
 *  11 back-to-back sectors, as encoded below (explicit gap included).
 * Decoded Sector:
 *  u8 0x00,0x00 :: Sector gap
 *  u8 0xa1,0xa1 :: Sync header (encoded as 0x4489 0x4489)
 *  u8 format    :: Always 0xff
 *  u8 track     :: 0-159
 *  u8 sector    :: 0-10
 *  u8 sec_to_gap:: 1-11
 *  u8 label[16] :: usually zero
 *  u32 hdr_csum :: (XOR raw MFM) & 0x55555555
 *  u32 dat_csum
 *  u8 data[512]
 * MFM encoding:
 *  u16 0xaaaa,0xaaaa
 *  u16 0x4489,0x4489
 *  u32 info_even,info_odd
 *  u8  label_even[16],label_odd[16]
 *  u32 hdr_csum_even,hdr_csum_odd
 *  u32 dat_csum_even,dat_csum_odd
 *  u8  data_even[512],data_odd[512]
 *
 * TRKTYP_amigados data layout:
 *  u8 sector_data[11][512]
 *
 * TRKTYP_amigados_extended data layout:
 *  struct sector {
 *   u32 sync;
 *   u8 hdr[4];
 *   u8 label[16];
 *   u8 data[512];
 *  } sector[11];
 *
 * The extended form is used by various games:
 *   New Zealand Story: stashes a custom data checksum in the label area
 *   Graftgold (Fire & Ice, Uridium 2, ...): store cyl# in place of track#
 *   Z Out: custom sync on track 1
 */

#include <libdisk/util.h>
#include <private/disk.h>

/* Sector data sizes for amigados and amigados_extended handlers. */
#define STD_SEC 512
#define EXT_SEC (STD_SEC + offsetof(struct ados_ext, dat))

const static uint32_t syncs[] = {
    0x44894489,
    0x45214521  /* Z Out, track 1 */
};

struct ados_hdr {
    uint8_t  format, track, sector, sectors_to_gap;
    uint8_t  lbl[16];
    uint32_t hdr_checksum;
    uint32_t dat_checksum;
};

struct ados_ext {
    uint32_t sync;
    uint8_t hdr[20];
    uint16_t speed;
    uint8_t dat[0];
};

static void *ados_write_raw(
    struct disk *d, unsigned int tracknr, struct stream *s)
{
    struct track_info *ti = &d->di->track[tracknr];
    char *block;
    struct ados_ext *ext;
    unsigned int i, nr_valid_blocks = 0, has_extended_blocks = 0;
    unsigned int least_block = ~0u;
    uint64_t lat, latency[ti->nr_sectors];

    block = memalloc(EXT_SEC * ti->nr_sectors);
    for (i = 0; i < EXT_SEC * ti->nr_sectors / 4; i++)
        memcpy((uint32_t *)block + i, "NDOS", 4);

    while ((stream_next_bit(s) != -1) &&
           (nr_valid_blocks != ti->nr_sectors)) {

        struct ados_hdr ados_hdr;
        char dat[STD_SEC], raw[2*(sizeof(struct ados_hdr)+STD_SEC)];
        uint32_t sync = s->word, idx_off = s->index_offset_bc - 31;

        for (i = 0; i < ARRAY_SIZE(syncs); i++)
            if (sync == syncs[i])
                break;
        if (i == ARRAY_SIZE(syncs))
            continue;

        lat = s->latency;
        if (stream_next_bytes(s, raw, sizeof(raw)) == -1)
            break;
        lat = s->latency - lat;

        mfm_decode_bytes(bc_mfm_even_odd, 4, &raw[2*0], &ados_hdr);
        mfm_decode_bytes(bc_mfm_even_odd, 16, &raw[2*4], ados_hdr.lbl);
        mfm_decode_bytes(bc_mfm_even_odd, 4, &raw[2*20],
                         &ados_hdr.hdr_checksum);
        mfm_decode_bytes(bc_mfm_even_odd, 4, &raw[2*24],
                         &ados_hdr.dat_checksum);
        mfm_decode_bytes(bc_mfm_even_odd, STD_SEC, &raw[2*28], dat);

        ados_hdr.hdr_checksum = be32toh(ados_hdr.hdr_checksum);
        ados_hdr.dat_checksum = be32toh(ados_hdr.dat_checksum);
        if ((amigados_checksum(&ados_hdr, 20) != ados_hdr.hdr_checksum) ||
            (amigados_checksum(dat, STD_SEC) != ados_hdr.dat_checksum))
            continue;

        if ((ados_hdr.sector >= ti->nr_sectors) ||
            is_valid_sector(ti, ados_hdr.sector))
            continue;

        /* Detect non-standard header info. */
        if ((ados_hdr.format != 0xffu) ||
            (ados_hdr.track != tracknr) ||
            (sync != syncs[0]))
            has_extended_blocks = 1;
        for (i = 0; i < 16; i++)
            if (ados_hdr.lbl[i] != 0)
                has_extended_blocks = 1;

        ext = (struct ados_ext *)(block + ados_hdr.sector * EXT_SEC);
        ext->sync = htobe32(sync);
        memcpy(ext->hdr, &ados_hdr, sizeof(ext->hdr));
        memcpy(ext->dat, dat, STD_SEC);
        latency[ados_hdr.sector] = lat;

        set_sector_valid(ti, ados_hdr.sector);
        nr_valid_blocks++;
        if (least_block > ados_hdr.sector) {
            ti->data_bitoff = idx_off;
            least_block = ados_hdr.sector;
        }
    }

    if (nr_valid_blocks == 0) {
        memfree(block);
        return NULL;
    }

    /* Calculate average block latency. */
    lat = 0;
    for (i = 0; i < ti->nr_sectors; i++) {
        ext = (struct ados_ext *)(block + i * EXT_SEC);
        if (is_valid_sector(ti, i)) 
            lat += latency[i];
    }
    lat /= nr_valid_blocks;

    /* Check if we have any long or short blocks. */
    for (i = 0; i < ti->nr_sectors; i++) {
        ext = (struct ados_ext *)(block + i * EXT_SEC);
        if (!is_valid_sector(ti, i)) {
            ext->speed = SPEED_AVG;
            continue;
        }
        ext->speed = (latency[i] * SPEED_AVG) / lat;
        if (ext->speed > (SPEED_AVG*102)/100) {
            /* Long block: normalise to +5% */
            ext->speed = (SPEED_AVG*105)/100;
            has_extended_blocks = 1;
        } else if (ext->speed < (SPEED_AVG*98)/100) {
            /* Short block: normalise to -5% */
            ext->speed = (SPEED_AVG*95)/100;
            has_extended_blocks = 1;
        } else {
            /* Roughly average block: make it dead on */
            ext->speed = SPEED_AVG;
        }
    }

    if (!has_extended_blocks)
        for (i = 0; i < ti->nr_sectors; i++)
            memmove(block + i * STD_SEC,
                    block + i * EXT_SEC + offsetof(struct ados_ext, dat),
                    STD_SEC);

    init_track_info(
        ti, has_extended_blocks ? TRKTYP_amigados_extended : TRKTYP_amigados);

    for (i = 0; i < ti->nr_sectors; i++)
        if (is_valid_sector(ti, i))
            break;
    ti->data_bitoff -= i * 544*8*2;
    ti->data_bitoff -= 32; /* initial gap */

    return block;
}

static void ados_read_raw(
    struct disk *d, unsigned int tracknr, struct tbuf *tbuf)
{
    struct track_info *ti = &d->di->track[tracknr];
    struct ados_hdr ados_hdr;
    uint8_t *dat = ti->dat;
    unsigned int i, speed;
    uint32_t sync, csum;

    for (i = 0; i < ti->nr_sectors; i++) {

        speed = SPEED_AVG;
        sync = syncs[0];
        memset(&ados_hdr, 0, sizeof(ados_hdr));
        ados_hdr.format = 0xffu;
        ados_hdr.track = tracknr;

        if (ti->type == TRKTYP_amigados_extended) {
            struct ados_ext *ext = (struct ados_ext *)dat;
            speed = ext->speed;
            sync = be32toh(ext->sync);
            memcpy(&ados_hdr, ext->hdr, sizeof(ext->hdr));
            dat += offsetof(struct ados_ext, dat);
        }

        ados_hdr.sector = i;
        ados_hdr.sectors_to_gap = 11 - i;

        /* gap */
        tbuf_bits(tbuf, speed, bc_mfm, 16, 0);
        /* sync mark */
        tbuf_bits(tbuf, speed, bc_raw, 32, sync);
        /* info */
        tbuf_bytes(tbuf, speed, bc_mfm_even_odd, 4, &ados_hdr);
        /* lbl */
        tbuf_bytes(tbuf, speed, bc_mfm_even_odd, 16, ados_hdr.lbl);
        /* header checksum */
        csum = amigados_checksum(&ados_hdr, 20);
        tbuf_bits(tbuf, speed, bc_mfm_even_odd, 32, csum);
        /* data checksum */
        csum = amigados_checksum(dat, STD_SEC);
        if (!is_valid_sector(ti, i))
            csum ^= 1; /* bad checksum for an invalid sector */
        tbuf_bits(tbuf, speed, bc_mfm_even_odd, 32, csum);
        /* data */
        tbuf_bytes(tbuf, speed, bc_mfm_even_odd, STD_SEC, dat);
        dat += STD_SEC;
    }
}

struct track_handler amigados_handler = {
    .bytes_per_sector = STD_SEC,
    .nr_sectors = 11,
    .write_raw = ados_write_raw,
    .read_raw = ados_read_raw
};

struct track_handler amigados_extended_handler = {
    .bytes_per_sector = EXT_SEC,
    .nr_sectors = 11,
    .write_raw = ados_write_raw,
    .read_raw = ados_read_raw
};

/* AmigaDOS Long Tracks:
 * Dummy types and write handler which increase track gap by a defined amount.
 * These are used where the protection routine does not check for any data
 * in the track gap, or expects only (MFM-encoded) zeros. */

static void *ados_longtrack_write_raw(
    struct disk *d, unsigned int tracknr, struct stream *s)
{
    struct track_info *ti = &d->di->track[tracknr];
    /* handler.bytes_per_sector is overloaded to contain track bit length */
    unsigned int total_bits = handlers[ti->type]->bytes_per_sector;
    const char *typename = ti->typename;
    char *ablk;

    init_track_info(ti, TRKTYP_amigados);
    ablk = handlers[TRKTYP_amigados]->write_raw(d, tracknr, s);
    if (ablk == NULL)
        return NULL;

    ti->total_bits = total_bits;
    ti->typename = typename;
    return ablk;
}

struct track_handler amigados_long_103300_handler = {
    .bytes_per_sector = 103300,
    .write_raw = ados_longtrack_write_raw,
};

struct track_handler amigados_long_105500_handler = {
    .bytes_per_sector = 105500,
    .write_raw = ados_longtrack_write_raw,
};

struct track_handler amigados_long_111000_handler = {
    .bytes_per_sector = 111000,
    .write_raw = ados_longtrack_write_raw,
};

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
