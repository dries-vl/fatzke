#ifdef __linux__
#include "header.h"

/* ============================================================================
   X11 low-latency backend matching the Wayland pf_* public contract
   - Fullscreen, borderless window
   - Opt-out of compositor via _NET_WM_BYPASS_COMPOSITOR + _NET_WM_STATE_FULLSCREEN
   - XInput2 for input (falls back to core)
   - XPresent for vblank-aligned feedback (NotifyMSC + CompleteNotify)
   - Maintains same pf_* surface as your Wayland module
   Build deps (Debian/Ubuntu): libx11-dev libxrandr-dev libxi-dev libxpresent-dev
   Link: -lX11 -lXrandr -lXi -lXpresent -lrt
   ========================================================================== */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xpresent.h>


#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------- time base ------------------------------------------------------ */

static clockid_t clockid;
u64 pf_ns_now(void){
    struct timespec ts; clock_gettime(clockid ? clockid : CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
u64 T0;
void pf_time_reset() {T0=pf_ns_now();}
u64 pf_ns_start() {return T0;};
void pf_timestamp(char *msg) {u64 _t=pf_ns_now(); printf("[+%7.3f ms] %s\n",(_t-T0)/1e6,(msg));}

/* -------- window/state --------------------------------------------------- */

struct x11_window {
    /* X11 surface */
    Display* dpy;
    int      scr;
    Window   root;
    Window   win;

    /* EWMH atoms */
    Atom     atom_net_wm_state;
    Atom     atom_net_wm_state_fullscreen;
    Atom     atom_net_wm_bypass_compositor;

    /* Extensions */
    int      xi2_major, xi2_minor;          /* XInput2 version (if any) */
    int      present_event_base, present_error_base; /* not used for cookies; kept for ref */
    int      present_major_opcode;          /* required to identify GenericEvent cookies */
    int      present_version_major, present_version_minor;

    /* Geometry / visibility */
    int      win_w, win_h;
    int      mouse_x, mouse_y;
    int      visible;

    /* Timing */
    uint64_t refresh_ns;        /* estimated from RandR; fallback 16.666 ms */
    uint64_t last_present_ns;   /* from XPresent UST (µs -> ns) */
    uint64_t phase_ns;
    double   phase_alpha;
    int      in_flight_count;

    /* App callbacks */
    KEYBOARD_CB on_key;
    MOUSE_CB    on_mouse;
    void*       callback_data;

    /* Present token -> FBUserData mapping */
    uint32_t    next_token;
    struct Pending* pending;    /* singly-linked list */
};

struct FBUserData {
    struct x11_window* w;
    uint64_t id;          /* your frame id */
    uint64_t queued_ns;   /* time just before vkQueuePresentKHR (call site) */
};

struct Pending {
    uint32_t token;
    struct FBUserData* ud;
    struct Pending* next;
};

/* -------- helpers -------------------------------------------------------- */

static uint64_t ust_to_ns(uint64_t ust_us){ return ust_us * 1000ull; }

static void pending_add(struct x11_window* w, uint32_t token, struct FBUserData* ud){
    struct Pending* p = (struct Pending*)malloc(sizeof *p);
    p->token = token; p->ud = ud; p->next = w->pending; w->pending = p;
}
static struct FBUserData* pending_take(struct x11_window* w, uint32_t token){
    struct Pending** pp = &w->pending;
    while (*pp){
        if ((*pp)->token == token){
            struct Pending* n = (*pp)->next;
            struct FBUserData* ud = (*pp)->ud;
            free(*pp); *pp = n; return ud;
        }
        pp = &((*pp)->next);
    }
    return NULL;
}

static int x11_get_primary_screen_size(Display* dpy, int* out_w, int* out_h, double* out_hz){
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    XRRScreenResources* res = XRRGetScreenResources(dpy, root);
    int w = DisplayWidth(dpy, scr);
    int h = DisplayHeight(dpy, scr);
    double hz = 60.0;

    if (res){
        RROutput primary = XRRGetOutputPrimary(dpy, root);
        XRROutputInfo* oi = primary ? XRRGetOutputInfo(dpy, res, primary) : NULL;
        XRRCrtcInfo* ci = (oi && oi->crtc) ? XRRGetCrtcInfo(dpy, res, oi->crtc) : NULL;
        if (ci){
            w = ci->width; h = ci->height;
            for (int i=0;i<res->nmode;i++){
                if (res->modes[i].id == ci->mode){
                    XRRModeInfo* mi = &res->modes[i];
                    if (mi->hTotal && mi->vTotal && mi->dotClock){
                        hz = (double)mi->dotClock / (double)(mi->hTotal * mi->vTotal);
                    }
                    break;
                }
            }
        }
        if (ci) XRRFreeCrtcInfo(ci);
        if (oi) XRRFreeOutputInfo(oi);
        XRRFreeScreenResources(res);
    }

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_hz) *out_hz = hz;
    return 1;
}

static void x11_set_fullscreen_and_bypass(struct x11_window* w){
    Display* dpy = w->dpy; Window win = w->win;

    /* Request _NET_WM_STATE_FULLSCREEN */
    XEvent e; memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.message_type = w->atom_net_wm_state;
    e.xclient.display = dpy;
    e.xclient.window = win;
    e.xclient.format = 32;
    e.xclient.data.l[0] = 1; /* _NET_WM_STATE_ADD */
    e.xclient.data.l[1] = w->atom_net_wm_state_fullscreen;
    e.xclient.data.l[2] = 0;
    e.xclient.data.l[3] = 1; /* normal source */
    XSendEvent(dpy, RootWindow(dpy, w->scr), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &e);

    /* Opt-out of compositor if supported */
    unsigned long bypass = 1; /* 1 = bypass compositor */
    XChangeProperty(dpy, win, w->atom_net_wm_bypass_compositor, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char*)&bypass, 1);
}

static int x11_init_xi2(struct x11_window* w){
    int maj=2, min=0;
    if (XIQueryVersion(w->dpy, &maj, &min) != Success) return 0;
    w->xi2_major = maj; w->xi2_minor = min;

    XIEventMask mask; unsigned char m[3]={0};
    mask.deviceid = XIAllDevices;
    mask.mask = m; mask.mask_len = sizeof m;
    XISetMask(m, XI_KeyPress);
    XISetMask(m, XI_KeyRelease);
    XISetMask(m, XI_ButtonPress);
    XISetMask(m, XI_ButtonRelease);
    XISetMask(m, XI_Motion);
    XISelectEvents(w->dpy, w->win, &mask, 1);
    return 1;
}

static void x11_query_present(struct x11_window* w){
    int opb=0, evb=0, erb=0;
    if (!XPresentQueryExtension(w->dpy, &opb, &evb, &erb)) return;
    w->present_major_opcode = opb;
    w->present_event_base   = evb;
    w->present_error_base   = erb;
    int maj=1, min=2; /* request ≥ 1.2 */
    if (!XPresentQueryVersion(w->dpy, &maj, &min)) return;
    w->present_version_major = maj;
    w->present_version_minor = min;

    /* Subscribe to Present events for timing feedback */
    XPresentSelectInput(w->dpy, w->win,
        PresentCompleteNotifyMask | PresentConfigureNotifyMask | PresentIdleNotifyMask);
}

/* -------- input helpers -------------------------------------------------- */

static enum KEYBOARD_BUTTON translate_keysym(KeySym sym){
    if (sym == XK_Escape) return KEYBOARD_ESCAPE;
    return KEYBOARD_BUTTON_UNKNOWN;
}
static void handle_core_keyboard(struct x11_window* w, XKeyEvent* kev, int pressed){
    KeySym sym = XLookupKeysym(kev, 0);
    enum KEYBOARD_BUTTON b = translate_keysym(sym);
    if (w->on_key) w->on_key(w->callback_data, b, pressed);
}
static void handle_core_pointer(struct x11_window* w, XButtonEvent* bev, int pressed){
    u32 mb = MOUSE_BUTTON_UNKNOWN;
    if (bev->button == Button1) mb = MOUSE_LEFT;
    if (bev->button == Button2) mb = MOUSE_MIDDLE;
    if (bev->button == Button3) mb = MOUSE_RIGHT;
    if (w->on_mouse) w->on_mouse(w->callback_data, w->mouse_x, w->mouse_y, mb, pressed);
}
static enum KEYBOARD_BUTTON translate_xi2_key(Display* dpy, XIDeviceEvent* ev){
    KeySym sym = XkbKeycodeToKeysym(dpy, ev->detail, 0, 0);
    return translate_keysym(sym);
}
static void handle_xi2_event(struct x11_window* w, XGenericEventCookie* cookie){
    if (cookie->evtype == XI_Motion){
        XIDeviceEvent* ev = (XIDeviceEvent*)cookie->data;
        w->mouse_x = (int)ev->event_x; w->mouse_y = (int)ev->event_y;
        if (w->on_mouse) w->on_mouse(w->callback_data, w->mouse_x, w->mouse_y, MOUSE_MOVED, 0);
    } else if (cookie->evtype == XI_ButtonPress || cookie->evtype == XI_ButtonRelease){
        XIDeviceEvent* ev = (XIDeviceEvent*)cookie->data;
        u32 mb = MOUSE_BUTTON_UNKNOWN;
        if (ev->detail == 1) mb = MOUSE_LEFT;
        if (ev->detail == 2) mb = MOUSE_MIDDLE;
        if (ev->detail == 3) mb = MOUSE_RIGHT;
        if (w->on_mouse) w->on_mouse(w->callback_data, w->mouse_x, w->mouse_y, mb,
                                     cookie->evtype==XI_ButtonPress?1:0);
    } else if (cookie->evtype == XI_KeyPress || cookie->evtype == XI_KeyRelease){
        XIDeviceEvent* ev = (XIDeviceEvent*)cookie->data;
        enum KEYBOARD_BUTTON b = translate_xi2_key(w->dpy, ev);
        if (w->on_key) w->on_key(w->callback_data, b, cookie->evtype==XI_KeyPress?1:0);
    }
}

/* -------- public API parity --------------------------------------------- */

int pf_window_width(void *p)  { struct x11_window* w = p; return w ? w->win_w : 0; }
int pf_window_height(void *p) { struct x11_window* w = p; return w ? w->win_h : 0; }
void *pf_surface_or_hwnd(void *p) { struct x11_window* w = p; return w ? (void*)(uintptr_t)w->win : NULL; }
void *pf_display_or_instance(void *p) { struct x11_window* w = p; return w ? (void*)w->dpy : NULL; }
int pf_window_visible(void *p) { struct x11_window* w = p; return w ? w->visible : 0; }

/* Non-blocking pump */
int pf_poll_events(void* p){
    struct x11_window* w = p; if (!w) return 0;
    while (XPending(w->dpy)){
        XEvent ev; XNextEvent(w->dpy, &ev);
        switch (ev.type){
            case MapNotify:    w->visible = 1; break;
            case UnmapNotify:  w->visible = 0; break;
            case ConfigureNotify: {
                XConfigureEvent* ce = &ev.xconfigure;
                w->win_w = ce->width; w->win_h = ce->height;
            } break;
            case KeyPress:   handle_core_keyboard(w, &ev.xkey, 1); break;
            case KeyRelease: handle_core_keyboard(w, &ev.xkey, 0); break;
            case MotionNotify: {
                XMotionEvent* me = &ev.xmotion;
                w->mouse_x = me->x; w->mouse_y = me->y;
                if (w->on_mouse) w->on_mouse(w->callback_data, w->mouse_x, w->mouse_y, MOUSE_MOVED, 0);
            } break;
            case ButtonPress:   handle_core_pointer(w, &ev.xbutton, 1); break;
            case ButtonRelease: handle_core_pointer(w, &ev.xbutton, 0); break;
            case GenericEvent: {
                XGenericEventCookie* c = &ev.xcookie;
                if (XGetEventData(w->dpy, c)){
                    /* XPresent and XI2 both deliver GenericEvent cookies; distinguish by opcode */
                    if (w->present_major_opcode && c->extension == w->present_major_opcode){
                        if (c->evtype == PresentCompleteNotify){
                            XPresentCompleteNotifyEvent* pe = (XPresentCompleteNotifyEvent*)c->data;
                            struct FBUserData* ud = pending_take(w, pe->serial_number);
                            uint64_t ust_ns = ust_to_ns(pe->ust);
                            w->last_present_ns = ust_ns;
                            if (w->in_flight_count > 0) w->in_flight_count--;

                            if (ud){
                                double queued_ms = (double)(int64_t)(ud->queued_ns - T0)/1e6;
                                double presented_ms = (double)(int64_t)(ust_ns - T0)/1e6;
                                double fbqueue_to_present_ms = (double)(int64_t)(ust_ns - ud->queued_ns)/1e6;
                                double predicted_next_ms = (double)(int64_t)(w->refresh_ns + ust_ns - T0)/1e6;
                                free(ud);
                            }
                        }
                    } else {
                        handle_xi2_event(w, c);
                    }
                    XFreeEventData(w->dpy, c);
                }
            } break;
            default: break;
        }
    }
    XFlush(w->dpy);
    return 1;
}

/* Ask for a Present timing callback aligned to the next MSC (vblank).
   Call this immediately before vkQueuePresentKHR and pass the same frame_id. */
void pf_request_present_feedback(void *win, u64 frame_id) {
    struct x11_window* w = win;
    if (!w || !w->dpy || !w->win || !w->present_major_opcode) return;

    struct FBUserData* ud = (struct FBUserData*)calloc(1, sizeof *ud);
    ud->w = w; ud->id = frame_id; ud->queued_ns = pf_ns_now();

    uint32_t token = ++w->next_token;
    pending_add(w, token, ud);
    w->in_flight_count++;

    /* Next MSC (target=0, divisor=1, remainder=0) -> notify at the very next vblank */
    XPresentNotifyMSC(w->dpy, w->win, token, 0, 1, 0);
}

/* Wayland had wl_callback frame fences; not applicable here, but keep symbol for parity. */
static void request_frame_callback(struct x11_window *w) { (void)w; }

/* Create fullscreen window, request compositor bypass, init XI2 + Present, discover refresh */
void *pf_create_window(void *ud, KEYBOARD_CB key_cb, MOUSE_CB mouse_cb){
    clockid = CLOCK_MONOTONIC;

    struct x11_window* w = (struct x11_window*)calloc(1, sizeof *w);
    w->on_key = key_cb; w->on_mouse = mouse_cb; w->callback_data = ud;
    w->phase_alpha = 0.10;
    w->refresh_ns = 16666667ull; /* default 60 Hz */

    w->dpy = XOpenDisplay(NULL);
    if (!w->dpy){ printf("XOpenDisplay failed\n"); _exit(1); }
    w->scr = DefaultScreen(w->dpy);
    w->root = RootWindow(w->dpy, w->scr);

    /* Primary geometry + refresh */
    int sw=0, sh=0; double hz=60.0;
    x11_get_primary_screen_size(w->dpy, &sw, &sh, &hz);
    if (hz > 1.0) w->refresh_ns = (uint64_t)(1e9 / hz);

    /* Window create */
    XSetWindowAttributes swa;
    swa.event_mask =
        StructureNotifyMask | KeyPressMask | KeyReleaseMask |
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ExposureMask;
    swa.override_redirect = False; /* let WM honor _NET_WM_STATE_FULLSCREEN */

    w->win = XCreateWindow(
        w->dpy, w->root,
        0, 0, (unsigned)sw, (unsigned)sh, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWEventMask | CWOverrideRedirect, &swa);

    XStoreName(w->dpy, w->win, APP_NAME);

    /* EWMH atoms */
    w->atom_net_wm_state = XInternAtom(w->dpy, "_NET_WM_STATE", False);
    w->atom_net_wm_state_fullscreen = XInternAtom(w->dpy, "_NET_WM_STATE_FULLSCREEN", False);
    w->atom_net_wm_bypass_compositor = XInternAtom(w->dpy, "_NET_WM_BYPASS_COMPOSITOR", False);

    /* Remove decorations (best-effort via Motif hints) */
    struct {
        unsigned long flags, functions, decorations;
        long input_mode;
        unsigned long status;
    } MWMHints = { 2 /* MWM_HINTS_DECORATIONS */, 0, 0, 0, 0 };
    Atom _MOTIF_WM_HINTS = XInternAtom(w->dpy, "_MOTIF_WM_HINTS", False);
    XChangeProperty(w->dpy, w->win, _MOTIF_WM_HINTS, _MOTIF_WM_HINTS, 32,
                    PropModeReplace, (unsigned char *)&MWMHints, 5);

    XMapWindow(w->dpy, w->win);
    XFlush(w->dpy);

    x11_set_fullscreen_and_bypass(w);

    /* Input & Present extensions (best-effort) */
    x11_init_xi2(w);
    x11_query_present(w);

    /* Pump a few events to learn final size/visibility */
    for (int i=0; i<200; ++i){
        while (XPending(w->dpy)){
            XEvent ev; XNextEvent(w->dpy, &ev);
            if (ev.type == MapNotify) w->visible = 1;
            else if (ev.type == ConfigureNotify){
                XConfigureEvent* ce = &ev.xconfigure;
                w->win_w = ce->width; w->win_h = ce->height;
            }
        }
        if (w->visible && w->win_w && w->win_h) break;
        usleep(1000);
    }

    pf_time_reset();
    pf_timestamp("x11 window ready");
    pf_timestamp("set up x11");
    return w;
}

#endif /* __linux__ */
