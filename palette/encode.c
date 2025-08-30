// cc tga2rle.c -run input.tga output.bin
// Output: [u32 offsets[num_tiles]] [tile0 rows tokens] [tile1 rows tokens] ...
// Token: header byte: bit7=1 => BYTE-RUN, bit7=0 => RAW; low 7 bits store (len-1) => len=1..128.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ---- editables ---- */
#define TILE_SIZE 64            /* square tiles; must divide image w,h; must be even */
#define RUN_THRESH 8           /* r>=4 in middle; r>=3 at row edges is implied by splitting across rows */
static const uint32_t PALETTE[16] = {
    0x00000000, 0x00222222, 0x00444444, 0x00666666, 0x00888888, 0x00AAAAAA, 0x00CCCCCC, 0x00EEEEEE, 0x00FF0000,
    0x0000FF00, 0x000000FF, 0x00FFFF00, 0x00FF00FF, 0x0000FFFF, 0x00FF8800, 0x00FFFFFF
};

/* ---- utils ---- */
static void die(const char* m)
{
    fprintf(stderr, "error: %s\n", m);
    exit(1);
}

static void write_u32_le(FILE* f, uint32_t v)
{
    uint8_t b[4];
    b[0] = v;
    b[1] = v >> 8;
    b[2] = v >> 16;
    b[3] = v >> 24;
    if (fwrite(b, 1, 4, f) != 4) die("write_u32_le");
}

/* ---- TGA ---- */
typedef struct
{
    int w, h, bpp, top_left;
    long pixel_ofs;
} TgaInfo;

static TgaInfo read_tga_header(FILE* f)
{
    uint8_t h[18];
    if (fread(h, 1, 18, f) != 18) die("short TGA");
    int id = h[0], cmap = h[1], it = h[2], bpp = h[16], desc = h[17];
    int w = h[12] | (h[13] << 8), hh = h[14] | (h[15] << 8);
    if (cmap) die("cmap TGA not supported");
    if (!(it == 2 || it == 3)) die("need uncompressed truecolor TGA (type 2/3)");
    if (!(bpp == 24 || bpp == 32)) die("need 24/32 bpp");
    TgaInfo ti;
    ti.w = w;
    ti.h = hh;
    ti.bpp = bpp;
    ti.top_left = (desc & 0x20) ? 1 : 0;
    ti.pixel_ofs = 18 + id;
    return ti;
}

/* ---- palette map ---- */
static inline uint8_t nearest_pal_index(uint8_t r, uint8_t g, uint8_t b)
{
    int best = 0, bd = 0x7FFFFFFF;
    for (int i = 0; i < 16; i++)
    {
        uint32_t c = PALETTE[i];
        int pr = (c >> 16) & 0xFF, pg = (c >> 8) & 0xFF, pb = c & 0xFF;
        int dr = r - pr, dg = g - pg, db = b - pb;
        int d = dr * dr + dg * dg + db * db;
        if (d < bd)
        {
            bd = d;
            best = i;
        }
    }
    return (uint8_t)best;
}

/* ---- quantize + pack 4bpp (two pixels per byte, hi nibble first) ---- */
static uint8_t* quantize_and_pack4(FILE* f, TgaInfo ti)
{
    if (fseek(f, ti.pixel_ofs,SEEK_SET) != 0) die("seek pix");
    int spp = ti.bpp / 8;
    long in_stride = (long)ti.w * spp;
    uint8_t* in = (uint8_t*)malloc(in_stride);
    if (!in) die("oom row");
    long out_stride = (ti.w + 1) / 2;
    uint8_t* out = (uint8_t*)malloc((size_t)out_stride * ti.h);
    if (!out) die("oom out");
    for (int y = 0; y < ti.h; y++)
    {
        int ry = ti.top_left ? y : (ti.h - 1 - y);
        if (fread(in, 1, (size_t)in_stride, f) != (size_t)in_stride) die("short data");
        uint8_t* dst = out + (size_t)ry * out_stride;
        uint8_t cur = 0;
        for (int x = 0; x < ti.w; x++)
        {
            uint8_t B = in[x * spp + 0];
            uint8_t G = (spp >= 2) ? in[x * spp + 1] : B;
            uint8_t R = (spp >= 3) ? in[x * spp + 2] : B;
            uint8_t idx = nearest_pal_index(R, G, B);
            if ((x & 1) == 0)
            {
                cur = (uint8_t)(idx << 4);
                if (x == ti.w - 1) dst[x >> 1] = cur;
            }
            else { dst[x >> 1] = (uint8_t)(cur | idx); }
        }
    }
    free(in);
    return out;
}

/* ---- encoder primitives ---- */
static void emit_raw(FILE* f, const uint8_t* p, int len)
{
    while (len > 0)
    {
        int n = len > 128 ? 128 : len;
        uint8_t h = (uint8_t)(0x00 | (uint8_t)(n - 1));
        if (fputc(h, f) == EOF) die("hdr RAW");
        if ((int)fwrite(p, 1, (size_t)n, f) != n) die("raw bytes");
        p += n;
        len -= n;
    }
}

static void emit_run(FILE* f, uint8_t b, int len)
{
    while (len > 0)
    {
        int n = len > 128 ? 128 : len;
        uint8_t h = (uint8_t)(0x80 | (uint8_t)(n - 1));
        if (fputc(h, f) == EOF) die("hdr RUN");
        if (fputc(b, f) == EOF) die("run byte");
        len -= n;
    }
}

static void encode_row_tokens(FILE* f, const uint8_t* row, int nbytes)
{
    int i = 0, raw0 = 0;
    while (i < nbytes)
    {
        int r = 1;
        while (i + r < nbytes && row[i + r] == row[i] && r < 32767) r++;
        if (r >= RUN_THRESH)
        {
            int raw_len = i - raw0;
            if (raw_len > 0) emit_raw(f, row + raw0, raw_len);
            emit_run(f, row[i], r);
            i += r;
            raw0 = i;
        }
        else i++;
    }
    int tail = i - raw0;
    if (tail > 0) emit_raw(f, row + raw0, tail);
}

/* ---- main ---- */
int main(int argc, char** argv)
{
    if (TILE_SIZE % 2) die("TILE_SIZE must be even");
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s input.tga output.bin\n", argv[0]);
        return 1;
    }

    FILE* fin = fopen(argv[1], "rb");
    if (!fin) die("open input");
    TgaInfo ti = read_tga_header(fin);
    if (ti.w <= 0 || ti.h <= 0) die("bad size");
    if (ti.w % TILE_SIZE || ti.h % TILE_SIZE) die("image dims must be multiples of TILE_SIZE");

    uint8_t* img4 = quantize_and_pack4(fin, ti);
    fclose(fin);

    int tiles_x = ti.w / TILE_SIZE, tiles_y = ti.h / TILE_SIZE, num_tiles = tiles_x * tiles_y;
    long img_stride_bytes = (ti.w + 1) / 2;
    int tile_row_bytes = TILE_SIZE / 2;

    FILE* fout = fopen(argv[2], "wb");
    if (!fout) die("open output");
    for (int i = 0; i < num_tiles; i++) write_u32_le(fout, 0); /* reserve offset table */
    long header_bytes = (long)num_tiles * 4;
    if (fseek(fout, header_bytes,SEEK_SET) != 0) die("seek payload");

    uint32_t* ofs = (uint32_t*)malloc((size_t)num_tiles * 4);
    if (!ofs) die("oom ofs");

    for (int ty = 0; ty < tiles_y; ty++)
    {
        for (int tx = 0; tx < tiles_x; tx++)
        {
            int tindex = ty * tiles_x + tx;
            long pos = ftell(fout);
            if (pos < 0) die("ftell");
            ofs[tindex] = (uint32_t)pos; /* absolute from file start */

            int px0 = tx * TILE_SIZE, py0 = ty * TILE_SIZE;
            for (int ry = 0; ry < TILE_SIZE; ry++)
            {
                const uint8_t* row = img4 + (size_t)(py0 + ry) * img_stride_bytes + (px0 >> 1);
                encode_row_tokens(fout, row, tile_row_bytes);
            }
        }
    }

    if (fseek(fout, 0,SEEK_SET) != 0) die("seek header");
    for (int i = 0; i < num_tiles; i++) write_u32_le(fout, ofs[i]);
    fclose(fout);
    free(ofs);
    free(img4);
    return 0;
}
