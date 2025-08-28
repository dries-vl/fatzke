#include "../header/header.inc"

static inline u32 color_from_index(u32 idx)
{
    u32 r = (idx) & 7, g = (idx >> 3) & 7, b = (idx >> 6) & 7;
    return 0xFF000000u | (r << 21) | (g << 13) | (b << 5);
}

static inline u32 index_from_color(u32 argb)
{
    u32 r = (argb >> 21) & 7, g = (argb >> 13) & 7, b = (argb >> 5) & 7;
    return r | (g << 3) | (b << 6);
}
