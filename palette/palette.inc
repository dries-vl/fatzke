#include "../header/header.inc"

inline unsigned color_to_index_rgb332(unsigned argb) {
    unsigned rgb = argb & 0x00FFFFFF;
    // RG: 0-31 map to 0, 32-63 to 1, etc., B: 0-63 map to 0, etc.
    return ((rgb >> 16) & 0xE0) | ((rgb >> 11) & 0x1C) | ((rgb >> 6) & 0x03);
}

inline unsigned index_to_color_rgb332(unsigned i){
    unsigned r3=i>>5, g3=(i>>2)&7, b2=i&3;
    unsigned R=(r3<<5)|(r3<<2)|(r3>>1); // {0,36,73,109,146,182,219,255}
    unsigned G=(g3<<5)|(g3<<2)|(g3>>1); // {0,36,73,109,146,182,219,255}
    unsigned B=(b2<<6)|(b2<<4)|(b2<<2)|b2; // {0,85,170,255}
    return (R<<16)|(G<<8)|B;
}
