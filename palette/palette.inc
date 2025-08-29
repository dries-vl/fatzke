#include "../header/header.inc"

// READ 0.5 byte 1k texture + WRITE 0.5 byte 1k buffer + READ 0.5 byte 1k buffer + WRITE 4 byte 2k upscale
// READ 1 byte 1k texture + WRITE 1 byte 1k buffer + READ 1 byte 1k buffer + WRITE 4 byte 2k upscale
// READ 4 byte 2k texture + WRITE 4 byte 2k buffer

// 16 colors (4bit, two indices fit in one byte)
inline unsigned color_to_index_rgb121(unsigned argb)
{
    unsigned rgb = argb & 0x00FFFFFF;
    return ((rgb >> 23) & 1) << 3 | ((rgb >> 13) & 3) << 1 | ((rgb >> 7) & 1);
}
inline unsigned index_to_color_rgb121(unsigned i)
{
    unsigned r1 = i >> 3, g2 = (i >> 1) & 3, b1 = i & 1;
    unsigned R = (unsigned)(-(int)r1) & 255; // {0,255}
    unsigned G = (g2 << 6) | (g2 << 4) | (g2 << 2) | g2; // {0,85,170,255}
    unsigned B = (unsigned)(-(int)b1) & 255; // {0,255}
    return (R << 16) | (G << 8) | B;
}

// 256 colors (8bit, index fits in a byte)
inline unsigned color_to_index_rgb332(unsigned argb)
{
    unsigned rgb = argb & 0x00FFFFFF;
    return ((rgb >> 16) & 0xE0) | ((rgb >> 11) & 0x1C) | ((rgb >> 6) & 0x03);
}
inline unsigned index_to_color_rgb332(unsigned i)
{
    unsigned r3 = i >> 5, g3 = (i >> 2) & 7, b2 = i & 3;
    unsigned R = (r3 << 5) | (r3 << 2) | (r3 >> 1); // {0,36,73,109,146,182,219,255}
    unsigned G = (g3 << 5) | (g3 << 2) | (g3 >> 1); // {0,36,73,109,146,182,219,255}
    unsigned B = (b2 << 6) | (b2 << 4) | (b2 << 2) | b2; // {0,85,170,255}
    return (R << 16) | (G << 8) | B;
}

// 4096 colors (12bit, can add 4bit alpha and fit in 2 bytes)
inline unsigned color_to_index_rgb444(unsigned argb)
{
    unsigned rgb = argb & 0x00FFFFFF;
    return ((rgb >> 20) & 0x0F) << 8 | ((rgb >> 12) & 0x0F) << 4 | ((rgb >> 4) & 0x0F);
}
inline unsigned index_to_color_rgb444(unsigned i)
{
    unsigned r4 = i >> 8, g4 = (i >> 4) & 15, b4 = i & 15;
    unsigned R = (r4 << 4) | r4; // {0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255}
    unsigned G = (g4 << 4) | g4; // {0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255}
    unsigned B = (b4 << 4) | b4; // {0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255}
    return (R << 16) | (G << 8) | B;
}
