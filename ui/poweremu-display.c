/*
 * PowerEmu display: shows the guest in the PowerEmu app's own window.
 *
 *   -display none -object poweremu-display,id=pd0,path=/tmp/x.sock
 *
 * PowerEmu listens on the Unix socket at `path`; QEMU connects at startup.
 * The guest's screen is copied (as 32-bit BGRA) into shared memory whose
 * file descriptor is passed to PowerEmu with SCM_RIGHTS; afterwards QEMU
 * only says which rectangle changed.  The guest's hardware cursor and its
 * position go the same way.  PowerEmu sends keyboard and mouse input back.
 *
 * Every message is a header { uint32 type, uint32 length } followed by
 * `length` bytes; integers in host order (both ends run on the same Mac).
 *
 *   QEMU -> PowerEmu
 *     1 SURFACE  u32 width, height, stride     (+ the shared memory's fd)
 *     2 DAMAGE   u32 x, y, width, height
 *     3 CURSOR   u32 width, height, hot_x, hot_y, then width*height BGRA
 *     4 MOUSE    i32 x, y, u32 visible          (the guest cursor's place)
 *   PowerEmu -> QEMU
 *    10 KEY      u32 macOS virtual key code, u32 down
 *    11 MOTION   i32 dx, dy                     (relative, pixels)
 *    12 BUTTONS  u32 mask: 1 left, 2 right, 4 middle
 *    13 WHEEL    i32 dy (lines; positive = away from the user), i32 dx
 *    14 POINT    u32 x, y                       (absolute, guest pixels:
 *                                                for the USB tablet)
 *
 * Copyright (c) 2026 Spartan0285
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qom/object_interfaces.h"
#include "io/channel-socket.h"
#include "ui/console.h"
#include "ui/input.h"
#include "system/system.h"
#include <sys/mman.h>

#define TYPE_POWEREMU_DISPLAY "poweremu-display"
OBJECT_DECLARE_SIMPLE_TYPE(PowerEmuDisplay, POWEREMU_DISPLAY)

enum {
    PE_SURFACE = 1, PE_DAMAGE = 2, PE_CURSOR = 3, PE_MOUSE = 4,
    PE_KEY = 10, PE_MOTION = 11, PE_BUTTONS = 12, PE_WHEEL = 13, PE_POINT = 14,
};

struct PowerEmuDisplay {
    Object parent_obj;
    char *path;

    QIOChannelSocket *sioc;
    guint watch;
    bool connected;

    DisplayChangeListener dcl;
    bool registered;
    Notifier machine_done;

    /* The shared copy of the screen. */
    void *shm;
    size_t shm_size;
    int width, height, stride;
    pixman_image_t *shm_image;

    /* Damage gathered since the last refresh. */
    bool dirty;
    int dx0, dy0, dx1, dy1;

    uint8_t in[256];
    size_t in_len;
    uint32_t buttons;
};

/* ---- sending ---- */

static void pe_send(PowerEmuDisplay *pd, uint32_t type, const void *payload,
                    uint32_t len, int fd)
{
    uint32_t hdr[2] = { type, len };
    struct iovec iov[2] = {
        { .iov_base = hdr, .iov_len = sizeof(hdr) },
        { .iov_base = (void *)payload, .iov_len = len },
    };
    Error *err = NULL;

    if (!pd->connected) {
        return;
    }
    if (qio_channel_writev_full_all(QIO_CHANNEL(pd->sioc), iov, len ? 2 : 1,
                                    fd >= 0 ? &fd : NULL, fd >= 0 ? 1 : 0,
                                    0, &err) < 0) {
        /* PowerEmu went away; the guest keeps running without a screen. */
        error_free(err);
        pd->connected = false;
    }
}

/* ---- the screen ---- */

static void pe_free_shm(PowerEmuDisplay *pd)
{
    if (pd->shm_image) {
        qemu_pixman_image_unref(pd->shm_image);
        pd->shm_image = NULL;
    }
    if (pd->shm) {
        munmap(pd->shm, pd->shm_size);
        pd->shm = NULL;
    }
}

static void pe_damage(PowerEmuDisplay *pd, int x, int y, int w, int h)
{
    if (!pd->dirty) {
        pd->dx0 = x; pd->dy0 = y; pd->dx1 = x + w; pd->dy1 = y + h;
        pd->dirty = true;
        return;
    }
    pd->dx0 = MIN(pd->dx0, x);
    pd->dy0 = MIN(pd->dy0, y);
    pd->dx1 = MAX(pd->dx1, x + w);
    pd->dy1 = MAX(pd->dy1, y + h);
}

static void pe_gfx_update(DisplayChangeListener *dcl, int x, int y, int w, int h)
{
    PowerEmuDisplay *pd = container_of(dcl, PowerEmuDisplay, dcl);
    DisplaySurface *ds = dcl->con ? qemu_console_surface(dcl->con) : NULL;

    if (!ds || !pd->shm_image) {
        return;
    }
    x = MAX(x, 0);
    y = MAX(y, 0);
    w = MIN(w, pd->width - x);
    h = MIN(h, pd->height - y);
    if (w <= 0 || h <= 0) {
        return;
    }
    /*
     * Many updates repaint what is already there (the GPU model reports the
     * whole screen each refresh).  For 32-bit screens, find the rows that
     * really changed, comparing colour bytes only (x8r8g8b8's spare byte is
     * not kept), and pass on just those; an idle screen then costs nothing.
     */
    pixman_format_code_t fmt = pixman_image_get_format(ds->image);
    if (fmt == PIXMAN_x8r8g8b8 || fmt == PIXMAN_a8r8g8b8) {
        const uint8_t *src = (const uint8_t *)pixman_image_get_data(ds->image);
        int sstride = pixman_image_get_stride(ds->image);
        int first = -1, last = -1;

        for (int row = y; row < y + h; row++) {
            const uint32_t *a = (const uint32_t *)(src + (size_t)row * sstride) + x;
            const uint32_t *b = (const uint32_t *)((uint8_t *)pd->shm + (size_t)row * pd->stride) + x;
            for (int i = 0; i < w; i++) {
                if ((a[i] ^ b[i]) & 0x00ffffff) {
                    if (first < 0) {
                        first = row;
                    }
                    last = row;
                    break;
                }
            }
        }
        if (first < 0) {
            return;
        }
        y = first;
        h = last - first + 1;
    }
    /* Converts whatever depth the guest uses to BGRA. */
    pixman_image_composite(PIXMAN_OP_SRC, ds->image, NULL, pd->shm_image,
                           x, y, 0, 0, x, y, w, h);
    pe_damage(pd, x, y, w, h);
}

static void pe_gfx_switch(DisplayChangeListener *dcl, DisplaySurface *ds)
{
    PowerEmuDisplay *pd = container_of(dcl, PowerEmuDisplay, dcl);
    char name[32];
    int fd;

    if (!ds) {
        pe_free_shm(pd);
        pd->dirty = false;
        return;
    }
    /*
     * The GPU model hands over a new surface whenever the guest rewrites
     * the display start (every page flip, among others).  If the size is
     * the same, keep the shared memory PowerEmu already shows and refresh
     * its contents: re-announcing it made PowerEmu show a blank frame.
     */
    if (pd->shm_image && surface_width(ds) == pd->width && surface_height(ds) == pd->height) {
        pixman_image_composite(PIXMAN_OP_SRC, ds->image, NULL, pd->shm_image,
                               0, 0, 0, 0, 0, 0, pd->width, pd->height);
        pe_damage(pd, 0, 0, pd->width, pd->height);
        return;
    }
    pe_free_shm(pd);
    pd->dirty = false;
    pd->width = surface_width(ds);
    pd->height = surface_height(ds);
    pd->stride = pd->width * 4;
    pd->shm_size = ROUND_UP((size_t)pd->stride * pd->height, qemu_real_host_page_size());

    /* An anonymous shared-memory object: named only long enough to open it. */
    snprintf(name, sizeof(name), "/poweremu.%d.%u", getpid(), g_random_int());
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        error_report("poweremu-display: shm_open: %s", strerror(errno));
        return;
    }
    shm_unlink(name);
    if (ftruncate(fd, pd->shm_size) < 0) {
        error_report("poweremu-display: ftruncate: %s", strerror(errno));
        close(fd);
        return;
    }
    pd->shm = mmap(NULL, pd->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pd->shm == MAP_FAILED) {
        pd->shm = NULL;
        close(fd);
        return;
    }
    /* a8r8g8b8 so the alpha byte is written as opaque. */
    pd->shm_image = pixman_image_create_bits(PIXMAN_a8r8g8b8, pd->width, pd->height,
                                             pd->shm, pd->stride);

    /* Fill it before PowerEmu sees it, so the first frame isn't blank. */
    pixman_image_composite(PIXMAN_OP_SRC, ds->image, NULL, pd->shm_image,
                           0, 0, 0, 0, 0, 0, pd->width, pd->height);

    uint32_t msg[3] = { pd->width, pd->height, pd->stride };
    pe_send(pd, PE_SURFACE, msg, sizeof(msg), fd);
    close(fd);
    pe_damage(pd, 0, 0, pd->width, pd->height);
}

static void pe_refresh(DisplayChangeListener *dcl)
{
    PowerEmuDisplay *pd = container_of(dcl, PowerEmuDisplay, dcl);

    graphic_hw_update(dcl->con);
    if (pd->dirty) {
        uint32_t msg[4] = { pd->dx0, pd->dy0, pd->dx1 - pd->dx0, pd->dy1 - pd->dy0 };
        pd->dirty = false;
        pe_send(pd, PE_DAMAGE, msg, sizeof(msg), -1);
    }
}

static void pe_mouse_set(DisplayChangeListener *dcl, int x, int y, bool on)
{
    PowerEmuDisplay *pd = container_of(dcl, PowerEmuDisplay, dcl);
    int32_t msg[3] = { x, y, on };

    pe_send(pd, PE_MOUSE, msg, sizeof(msg), -1);
}

static void pe_cursor_define(DisplayChangeListener *dcl, QEMUCursor *c)
{
    PowerEmuDisplay *pd = container_of(dcl, PowerEmuDisplay, dcl);
    size_t pixels = (size_t)c->width * c->height * 4;
    g_autofree uint8_t *msg = g_malloc(16 + pixels);
    uint32_t head[4] = { c->width, c->height, c->hot_x, c->hot_y };

    memcpy(msg, head, 16);
    memcpy(msg + 16, c->data, pixels);        /* ARGB words = BGRA bytes */
    pe_send(pd, PE_CURSOR, msg, 16 + pixels, -1);
}

static bool pe_check_format(DisplayChangeListener *dcl, pixman_format_code_t f)
{
    return true;                              /* pixman converts any of them */
}

static const DisplayChangeListenerOps pe_ops = {
    .dpy_name = "poweremu",
    .dpy_gfx_update = pe_gfx_update,
    .dpy_gfx_switch = pe_gfx_switch,
    .dpy_gfx_check_format = pe_check_format,
    .dpy_refresh = pe_refresh,
    .dpy_mouse_set = pe_mouse_set,
    .dpy_cursor_define = pe_cursor_define,
};

/* ---- input ---- */

static void pe_button(PowerEmuDisplay *pd, uint32_t mask)
{
    static const struct { uint32_t bit; InputButton btn; } map[] = {
        { 1, INPUT_BUTTON_LEFT }, { 2, INPUT_BUTTON_RIGHT }, { 4, INPUT_BUTTON_MIDDLE },
    };
    for (int i = 0; i < ARRAY_SIZE(map); i++) {
        if ((mask ^ pd->buttons) & map[i].bit) {
            qemu_input_queue_btn(pd->dcl.con, map[i].btn, mask & map[i].bit);
        }
    }
    pd->buttons = mask;
    qemu_input_event_sync();
}

static void pe_input(PowerEmuDisplay *pd, uint32_t type, const uint8_t *p, uint32_t len)
{
    const int32_t *v = (const int32_t *)p;

    switch (type) {
    case PE_KEY:
        if (len >= 8 && (uint32_t)v[0] < qemu_input_map_osx_to_qcode_len) {
            int qcode = qemu_input_map_osx_to_qcode[v[0]];
            if (qcode) {
                qemu_input_event_send_key_qcode(pd->dcl.con, qcode, v[1] != 0);
            }
        }
        break;
    case PE_MOTION:
        if (len >= 8) {
            qemu_input_handler_activate_kind(INPUT_EVENT_MASK_REL);  /* buttons follow */
            qemu_input_queue_rel(pd->dcl.con, INPUT_AXIS_X, v[0]);
            qemu_input_queue_rel(pd->dcl.con, INPUT_AXIS_Y, v[1]);
            qemu_input_event_sync();
        }
        break;
    case PE_POINT:
        /* Goes to the absolute pointer (usb-tablet): no capture needed. */
        if (len >= 8 && pd->width > 0 && pd->height > 0) {
            qemu_input_handler_activate_kind(INPUT_EVENT_MASK_ABS);  /* buttons follow */
            qemu_input_queue_abs(pd->dcl.con, INPUT_AXIS_X, v[0], 0, pd->width - 1);
            qemu_input_queue_abs(pd->dcl.con, INPUT_AXIS_Y, v[1], 0, pd->height - 1);
            qemu_input_event_sync();
        }
        break;
    case PE_BUTTONS:
        if (len >= 4) {
            pe_button(pd, (uint32_t)v[0]);
        }
        break;
    case PE_WHEEL:
        if (len >= 4 && v[0]) {
            InputButton b = v[0] > 0 ? INPUT_BUTTON_WHEEL_UP : INPUT_BUTTON_WHEEL_DOWN;
            for (int n = MIN(abs(v[0]), 10); n > 0; n--) {
                qemu_input_queue_btn(pd->dcl.con, b, true);
                qemu_input_event_sync();
                qemu_input_queue_btn(pd->dcl.con, b, false);
                qemu_input_event_sync();
            }
        }
        break;
    }
}

static gboolean pe_readable(QIOChannel *ioc, GIOCondition cond, gpointer opaque)
{
    PowerEmuDisplay *pd = opaque;
    ssize_t n = qio_channel_read(ioc, (char *)pd->in + pd->in_len,
                                 sizeof(pd->in) - pd->in_len, NULL);

    if (n == QIO_CHANNEL_ERR_BLOCK) {
        return G_SOURCE_CONTINUE;
    }
    if (n <= 0) {
        pd->connected = false;
        pd->watch = 0;
        return G_SOURCE_REMOVE;
    }
    pd->in_len += n;
    while (pd->in_len >= 8) {
        uint32_t hdr[2];
        memcpy(hdr, pd->in, 8);
        if (hdr[1] > sizeof(pd->in) - 8) {      /* nonsense: drop the connection */
            pd->connected = false;
            pd->watch = 0;
            return G_SOURCE_REMOVE;
        }
        if (pd->in_len < 8 + hdr[1]) {
            break;
        }
        pe_input(pd, hdr[0], pd->in + 8, hdr[1]);
        memmove(pd->in, pd->in + 8 + hdr[1], pd->in_len - 8 - hdr[1]);
        pd->in_len -= 8 + hdr[1];
    }
    return G_SOURCE_CONTINUE;
}

/* ---- setting up ---- */

/* Objects are created before the machine (and the block layer runs bottom
 * halves while opening disks, earlier still): attach to the screen once the
 * machine has been built. */
static void pe_attach(Notifier *n, void *data)
{
    PowerEmuDisplay *pd = container_of(n, PowerEmuDisplay, machine_done);
    QemuConsole *con = qemu_console_lookup_default();     /* the first graphic one */

    if (!con) {
        error_report("poweremu-display: the machine has no screen");
        return;
    }
    pd->dcl.ops = &pe_ops;
    pd->dcl.con = con;
    register_displaychangelistener(&pd->dcl);
    update_displaychangelistener(&pd->dcl, 16);        /* about 60 Hz */
    pd->registered = true;
}

static void pe_complete(UserCreatable *uc, Error **errp)
{
    PowerEmuDisplay *pd = POWEREMU_DISPLAY(uc);
    SocketAddress addr = { .type = SOCKET_ADDRESS_TYPE_UNIX };

    if (!pd->path) {
        error_setg(errp, "poweremu-display needs path=");
        return;
    }
    addr.u.q_unix.path = pd->path;
    pd->sioc = qio_channel_socket_new();
    qio_channel_set_name(QIO_CHANNEL(pd->sioc), "poweremu-display");
    if (qio_channel_socket_connect_sync(pd->sioc, &addr, errp) < 0) {
        return;
    }
    pd->connected = true;
    pd->watch = qio_channel_add_watch(QIO_CHANNEL(pd->sioc), G_IO_IN, pe_readable, pd, NULL);
    pd->machine_done.notify = pe_attach;
    qemu_add_machine_init_done_notifier(&pd->machine_done);
}

static char *pe_get_path(Object *obj, Error **errp)
{
    return g_strdup(POWEREMU_DISPLAY(obj)->path);
}

static void pe_set_path(Object *obj, const char *value, Error **errp)
{
    PowerEmuDisplay *pd = POWEREMU_DISPLAY(obj);
    g_free(pd->path);
    pd->path = g_strdup(value);
}

static void pe_finalize(Object *obj)
{
    PowerEmuDisplay *pd = POWEREMU_DISPLAY(obj);

    if (pd->registered) {
        unregister_displaychangelistener(&pd->dcl);
    }
    if (pd->watch) {
        g_source_remove(pd->watch);
    }
    if (pd->sioc) {
        qio_channel_close(QIO_CHANNEL(pd->sioc), NULL);
        object_unref(OBJECT(pd->sioc));
    }
    pe_free_shm(pd);
    g_free(pd->path);
}

static void pe_class_init(ObjectClass *oc, void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);

    ucc->complete = pe_complete;
    object_class_property_add_str(oc, "path", pe_get_path, pe_set_path);
}

static const TypeInfo pe_info = {
    .name = TYPE_POWEREMU_DISPLAY,
    .parent = TYPE_OBJECT,
    .class_init = pe_class_init,
    .instance_size = sizeof(PowerEmuDisplay),
    .instance_finalize = pe_finalize,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { }
    }
};

static void pe_register_types(void)
{
    type_register_static(&pe_info);
}

type_init(pe_register_types);
