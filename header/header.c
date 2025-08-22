#ifndef HEADER
#define HEADER 0
#define TRANSLATION_UNIT 1

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned u32;
typedef unsigned long long u64;
typedef signed char i8;
typedef short i16;
typedef int i32;
typedef long long i64;
typedef float f32;
typedef double f64;
typedef long long isize;
typedef unsigned long long usize;

#else
#undef TRANSLATION_UNIT
#define TRANSLATION_UNIT 0
#endif

#undef HEADER
#define HEADER 1