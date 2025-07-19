#ifndef ULTRAFAST_H
#define ULTRAFAST_H
#include <stdint.h>

/* opaque */
typedef struct uf_ctx uf_ctx;

/* user-supplied input callbacks */
typedef void (*uf_key_cb)(void *ud, uint32_t key, uint32_t state);         /* wl_keyboard key */
typedef void (*uf_pointer_cb)(void *ud, int32_t x, int32_t y, uint32_t b); /* wl_pointer button */

/* -------------- API -------------- */
uf_ctx *uf_create_window(int w, int h, const char *title);
void   *uf_get_pixels (uf_ctx *c, int *stride_out); /* mmap’d ARGB8888 */
void    uf_commit     (uf_ctx *c);                  /* attach + damage + commit */
void    uf_set_input_cb(uf_ctx *c, uf_key_cb, uf_pointer_cb, void *userdata);
int     uf_poll       (uf_ctx *c);                  /* 0 → display closed, else keep going */
void    uf_destroy    (uf_ctx *c);

#endif /* ULTRAFAST_H */
