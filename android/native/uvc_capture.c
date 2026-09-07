
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define VENDOR_ID_CHROMATIC 0x374Eu

#define USB_DESC_INTERFACE 0x04
#define USB_DESC_ENDPOINT 0x05
#define USB_DESC_CS_INTERFACE 0x24
#define USB_CLASS_VIDEO 0x0E
#define USB_VIDEO_CONTROL 0x01
#define USB_VIDEO_STREAMING 0x02
#define UVC_SUBTYPE_HEADER 0x01
#define UVC_SUBTYPE_FORMAT_UNCOMPRESSED 0x04
#define UVC_SUBTYPE_FRAME_UNCOMPRESSED 0x05
#define UVC_GET_CUR 0x81
#define UVC_SET_CUR 0x01
#define UVC_VS_PROBE 0x01
#define UVC_VS_COMMIT 0x02

#define CTRL_TIMEOUT_MS 1000
#define NUM_URBS 8
#define PKTS_PER_URB 16
#define MAX_CONFIG_DESC 16384u
#define MAX_FRAME_BYTES (8u * 1024u * 1024u)
#define MAX_ISO_PACKET 3072
#define DRAIN_TIMEOUT_MS 3000
#define POLL_TIMEOUT_MS 100

static const uint8_t YUY2_GUID[16] = {
    0x59, 0x55, 0x59, 0x32, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};

typedef struct {
    int fd;                 /* dup() of the Java side descriptor, -1 once closed */
    int wake[2];            /* pipe used to interrupt the poll loop */
    pthread_mutex_t lock;   /* serialises fd teardown, never held across streaming */
    atomic_int stop;        /* cancellation flag, checked between packets */
    atomic_int running;     /* guards against two concurrent nRun calls */
    atomic_int refs;        /* 1 until nDestroy, +1 while nRun is executing */
    int vs_if;              /* video streaming interface number */
    int ep_addr;            /* isochronous IN endpoint address */
    int alt;
    int maxpkt;             /* bytes per microframe including transactions */
    int probe_len;          /* 34 for UVC 1.1+, 26 for 1.0, from bcdUVC */
    int fmt_index;
    int frame_idx;
    int width;
    int height;
    uint32_t frame_interval;
    size_t frame_bytes;     /* width * height * 2, one YUY2 frame */
} capture_t;

typedef struct {
    uint8_t *frame;
    size_t frame_bytes;
    size_t got;
    int have_fid;
    int cur_fid;
    int bad;
} frame_parser_t;

typedef struct {
    JNIEnv *env;
    jobject listener;
    jmethodID mid;
    jbyteArray luma;
    frame_parser_t parser;
    struct usbdevfs_urb *urbs[NUM_URBS];
    uint8_t *bufs[NUM_URBS];
    int slot_flight[NUM_URBS];
    int inflight;
} run_ctx_t;

typedef struct {
    int vs_if;
    int have_vc_hdr;
    uint16_t bcd_uvc;
    int have_vs_hdr;
    int vs_hdr_ep;
    int have_vs_ep;
    int vs_alt;
    int vs_ep;
    int vs_maxpkt;
    int have_fmt;
    int fmt_index;
    int def_frame;
    int have_frame;
    int frame_index;
    int frame_w;
    int frame_h;
    uint32_t frame_interval;
} desc_info_t;

/* ---------------- helpers ---------------- */

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void throw_vmsg(JNIEnv *env, const char *class_name, const char *fmt, va_list ap) {
    if ((*env)->ExceptionCheck(env)) {
        return;
    }
    char msg[256];
    vsnprintf(msg, sizeof msg, fmt, ap);
    jclass cls = (*env)->FindClass(env, class_name);
    if (cls != NULL) {
        (*env)->ThrowNew(env, cls, msg);
    }
}

static void throw_io(JNIEnv *env, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    throw_vmsg(env, "java/io/IOException", fmt, ap);
    va_end(ap);
}


static int ctrl_xfer(int fd, uint8_t request_type, uint8_t request, uint16_t value,
                     uint16_t index, uint16_t length, void *data, uint32_t timeout_ms) {
    struct usbdevfs_ctrltransfer t;
    memset(&t, 0, sizeof t);
    t.bRequestType = request_type;
    t.bRequest = request;
    t.wValue = value;
    t.wIndex = index;
    t.wLength = length;
    t.timeout = timeout_ms;
    t.data = data;
    return ioctl(fd, USBDEVFS_CONTROL, &t);
}

/* Release the claimed interface and close the fd exactly once. Only the
 * run thread (after draining its URBs) and the final unref get here, and
 * never both: the fd is set to -1 inside the lock. */
static void cap_teardown_fd(capture_t *cap) {
    pthread_mutex_lock(&cap->lock);
    if (cap->fd >= 0) {
        unsigned int ifno = (unsigned int)cap->vs_if;
        (void)ioctl(cap->fd, USBDEVFS_RELEASEINTERFACE, &ifno);
        close(cap->fd);
        cap->fd = -1;
    }
    pthread_mutex_unlock(&cap->lock);
}

static void cap_unref(capture_t *cap) {
    int prev = atomic_fetch_sub(&cap->refs, 1);
    if (prev != 1) {
        return;
    }
    /* Last reference: if the run thread executed, it already tore the fd
     * down after draining its URBs; otherwise do it now. */
    cap_teardown_fd(cap);
    if (cap->wake[0] >= 0) {
        close(cap->wake[0]);
    }
    if (cap->wake[1] >= 0) {
        close(cap->wake[1]);
    }
    pthread_mutex_destroy(&cap->lock);
    free(cap);
}

static void cap_request_stop(capture_t *cap) {
    atomic_store(&cap->stop, 1);
    if (cap->wake[1] >= 0) {
        (void)write(cap->wake[1], "x", 1);
    }
}

/* ---------------- descriptor parsing ---------------- */

#define PARSE_ERR(...) do { snprintf(err, errsz, __VA_ARGS__); return -1; } while (0)

static int parse_config(const uint8_t *buf, size_t len, desc_info_t *out,
                        char *err, size_t errsz) {
    memset(out, 0, sizeof *out);
    out->vs_if = -1;
    int cur_class = 0;
    int cur_sub = 0;
    int cur_alt = -1;
    int yuy2_format_pending = 0;
    size_t pos = 0;
    while (pos + 2 <= len) {
        uint8_t blen = buf[pos];
        uint8_t btype = buf[pos + 1];
        if (blen < 2 || pos + blen > len) {
            PARSE_ERR("malformed USB descriptor at offset %zu", pos);
        }
        if (btype == USB_DESC_INTERFACE) {
            if (blen < 9) {
                PARSE_ERR("short USB interface descriptor");
            }
            int if_num = buf[pos + 2];
            cur_alt = buf[pos + 3];
            cur_class = buf[pos + 5];
            cur_sub = buf[pos + 6];
            if (cur_class == USB_CLASS_VIDEO && cur_sub == USB_VIDEO_STREAMING
                    && out->vs_if < 0) {
                out->vs_if = if_num;
            }
            yuy2_format_pending = 0;
        } else if (btype == USB_DESC_ENDPOINT) {
            if (blen < 7) {
                PARSE_ERR("short USB endpoint descriptor");
            }
            if (cur_class == USB_CLASS_VIDEO && cur_sub == USB_VIDEO_STREAMING
                    && cur_alt > 0 && !out->have_vs_ep) {
                uint8_t addr = buf[pos + 2];
                uint8_t attr = buf[pos + 3];
                if ((addr & 0x80) != 0 && (attr & 0x03) == 0x01
                        && (!out->have_vs_hdr || addr == (uint8_t)out->vs_hdr_ep)) {
                    unsigned mps = le16(buf + pos + 4);
                    unsigned transactions = 1u + ((mps >> 11) & 3u);
                    out->vs_alt = cur_alt;
                    out->vs_ep = addr;
                    out->vs_maxpkt = (int)((mps & 0x07FFu) * transactions);
                    out->have_vs_ep = 1;
                }
            }
        } else if (btype == USB_DESC_CS_INTERFACE) {
            if (blen < 3) {
                PARSE_ERR("short class specific descriptor");
            }
            uint8_t sub = buf[pos + 2];
            if (cur_class == USB_CLASS_VIDEO && cur_sub == USB_VIDEO_CONTROL
                    && cur_alt == 0 && sub == UVC_SUBTYPE_HEADER && !out->have_vc_hdr) {
                if (blen < 5) {
                    PARSE_ERR("short video control header");
                }
                out->bcd_uvc = le16(buf + pos + 3);
                out->have_vc_hdr = 1;
            } else if (cur_class == USB_CLASS_VIDEO && cur_sub == USB_VIDEO_STREAMING
                    && cur_alt == 0) {
                if (sub == UVC_SUBTYPE_HEADER && !out->have_vs_hdr) {
                    if (blen < 13) {
                        PARSE_ERR("short video streaming input header");
                    }
                    out->vs_hdr_ep = buf[pos + 6];
                    out->have_vs_hdr = 1;
                } else if (sub == UVC_SUBTYPE_FORMAT_UNCOMPRESSED) {
                    if (blen < 27) {
                        PARSE_ERR("short uncompressed format descriptor");
                    }
                    yuy2_format_pending = memcmp(buf + pos + 5, YUY2_GUID, 16) == 0;
                    if (yuy2_format_pending && !out->have_fmt) {
                        out->fmt_index = buf[pos + 3];
                        out->def_frame = buf[pos + 22];
                        out->have_fmt = 1;
                    }
                } else if (sub == UVC_SUBTYPE_FRAME_UNCOMPRESSED
                        && yuy2_format_pending && !out->have_frame) {
                    if (blen < 25) {
                        PARSE_ERR("short uncompressed frame descriptor");
                    }
                    if (buf[pos + 3] == (uint8_t)out->def_frame) {
                        out->frame_index = buf[pos + 3];
                        out->frame_w = le16(buf + pos + 5);
                        out->frame_h = le16(buf + pos + 7);
                        out->frame_interval = le32(buf + pos + 21);
                        out->have_frame = 1;
                    }
                }
            }
        }
        pos += blen;
    }
    if (pos != len) {
        PARSE_ERR("trailing bytes after the configuration descriptor");
    }
    return 0;
}

/* ---------------- frame assembly ---------------- */

/*
 * Feed one isochronous packet (UVC payload header + data) into the assembler.
 * Returns 1 when this packet completed a full frame, whose YUY2 bytes now
 * sit in p->frame. Corrupt, incomplete and overflowed frames are discarded.
 */
static int parse_packet(frame_parser_t *p, const uint8_t *pkt, size_t len) {
    if (len < 2) {
        return 0;
    }
    uint8_t hlen = pkt[0];
    if (hlen < 2 || (size_t)hlen > len) {
        p->bad = 1;
        p->got = 0;
        return 0;
    }
    uint8_t info = pkt[1];
    int fid = info & 1;
    int eof = (info >> 1) & 1;
    size_t plen = len - (size_t)hlen;
    if (!p->have_fid) {
        p->have_fid = 1;
        p->cur_fid = fid;
        p->got = 0;
        p->bad = 0;
    } else if (fid != p->cur_fid) {
        /* the frame being assembled was lost or corrupt; resynchronise */
        p->cur_fid = fid;
        p->got = 0;
        p->bad = 0;
    }
    if ((info & 0x40) != 0) {
        p->bad = 1;
        p->got = 0;
    }
    if (!p->bad && plen > 0) {
        if (p->got + plen > p->frame_bytes) {
            p->bad = 1;
            p->got = 0;
        } else {
            memcpy(p->frame + p->got, pkt + hlen, plen);
            p->got += plen;
        }
    }
    if (eof) {
        int complete = !p->bad && p->got == p->frame_bytes;
        p->have_fid = 0;
        p->got = 0;
        p->bad = 0;
        return complete;
    }
    return 0;
}

static void extract_y(const uint8_t *frame, uint8_t *dst, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = frame[2 * i];
    }
}

/* ---------------- open: descriptors, claim ---------------- */

static jlong open_capture(JNIEnv *env, jint user_fd) {
    int fd = dup(user_fd);
    if (fd < 0) {
        throw_io(env, "duplicating the USB descriptor failed: %s", strerror(errno));
        return 0;
    }
    capture_t *cap = calloc(1, sizeof *cap);
    if (cap == NULL) {
        throw_io(env, "out of memory");
        close(fd);
        return 0;
    }
    cap->fd = fd;
    cap->wake[0] = -1;
    cap->wake[1] = -1;
    atomic_init(&cap->stop, 0);
    atomic_init(&cap->running, 0);
    atomic_init(&cap->refs, 1);
    if (pthread_mutex_init(&cap->lock, NULL) != 0
            || pipe2(cap->wake, O_NONBLOCK | O_CLOEXEC) < 0) {
        throw_io(env, "initialising the capture failed: %s", strerror(errno));
        goto fail;
    }

    uint8_t dev[18];
    int r = ctrl_xfer(fd, 0x80, 0x06, 0x0100, 0, (uint16_t)sizeof dev, dev, CTRL_TIMEOUT_MS);
    if (r != (int)sizeof dev) {
        throw_io(env, "reading the USB device descriptor failed");
        goto fail;
    }
    if (le16(dev + 8) != VENDOR_ID_CHROMATIC) {
        throw_io(env, "device is not a Chromatic (vendor 0x%04x)", le16(dev + 8));
        goto fail;
    }

    uint8_t head[9];
    r = ctrl_xfer(fd, 0x80, 0x06, 0x0200, 0, (uint16_t)sizeof head, head, CTRL_TIMEOUT_MS);
    if (r != (int)sizeof head) {
        throw_io(env, "reading the USB configuration header failed");
        goto fail;
    }
    unsigned total = le16(head + 2);
    if (total < 9 || total > MAX_CONFIG_DESC) {
        throw_io(env, "implausible USB configuration size %u", total);
        goto fail;
    }
    uint8_t *cfg = malloc(total);
    if (cfg == NULL) {
        throw_io(env, "out of memory for descriptors");
        goto fail;
    }
    r = ctrl_xfer(fd, 0x80, 0x06, 0x0200, 0, (uint16_t)total, cfg, CTRL_TIMEOUT_MS);
    if (r != (int)total) {
        throw_io(env, "reading the USB configuration descriptor failed");
        free(cfg);
        goto fail;
    }

    desc_info_t info;
    char err[128];
    if (parse_config(cfg, total, &info, err, sizeof err) != 0) {
        throw_io(env, "%s", err);
        free(cfg);
        goto fail;
    }
    free(cfg);

    if (!info.have_vc_hdr || !info.have_vs_hdr || !info.have_fmt || !info.have_frame
            || !info.have_vs_ep || info.vs_if < 0) {
        throw_io(env, "no supported USB video streaming interface found");
        goto fail;
    }
    if (info.frame_w < 1 || info.frame_h < 1 || info.frame_w > 4096 || info.frame_h > 4096
            || (uint64_t)info.frame_w * info.frame_h * 2 > MAX_FRAME_BYTES) {
        throw_io(env, "unsupported video frame size %dx%d", info.frame_w, info.frame_h);
        goto fail;
    }
    if (info.vs_maxpkt < 1 || info.vs_maxpkt > MAX_ISO_PACKET) {
        throw_io(env, "unsupported isochronous packet size %d", info.vs_maxpkt);
        goto fail;
    }

    cap->vs_if = info.vs_if;
    cap->ep_addr = info.vs_ep;
    cap->alt = info.vs_alt;
    cap->maxpkt = info.vs_maxpkt;
    cap->fmt_index = info.fmt_index;
    cap->frame_idx = info.frame_index;
    cap->width = info.frame_w;
    cap->height = info.frame_h;
    cap->frame_interval = info.frame_interval;
    cap->probe_len = (info.bcd_uvc >= 0x0110) ? 34 : 26;
    cap->frame_bytes = (size_t)info.frame_w * (size_t)info.frame_h * 2u;

#ifdef USBDEVFS_GET_SPEED
    int speed = ioctl(fd, USBDEVFS_GET_SPEED);
    if (speed >= 0 && speed < 3) {
        /* unknown=0, low=1, full=2: a 1024 byte isochronous budget needs
         * at least high speed, and the stock stream is high speed only */
        throw_io(env, "video needs a high speed USB link (link speed %d)", speed);
        goto fail;
    }
#endif

    /* detach any kernel driver (e.g. uvcvideo) from the streaming interface
     * only; the CDC and audio interfaces keep their drivers */
    struct usbdevfs_ioctl detach;
    memset(&detach, 0, sizeof detach);
    detach.ifno = cap->vs_if;
    detach.ioctl_code = USBDEVFS_DISCONNECT;
    (void)ioctl(fd, USBDEVFS_IOCTL, &detach);
    unsigned int ifno = (unsigned int)cap->vs_if;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifno) < 0) {
        throw_io(env, "claiming the video streaming interface failed: %s", strerror(errno));
        goto fail;
    }
#ifdef USBDEVFS_FORBID_SUSPEND
    (void)ioctl(fd, USBDEVFS_FORBID_SUSPEND);
#endif
    return (jlong)(intptr_t)cap;

fail:
    if (cap->wake[0] >= 0) {
        close(cap->wake[0]);
    }
    if (cap->wake[1] >= 0) {
        close(cap->wake[1]);
    }
    close(fd);
    free(cap);
    return 0;
}

/* ---------------- negotiation ---------------- */

static int probe_layout_matches(const uint8_t *cur, int off,
                                int fmt_index, int frame_idx, uint32_t interval) {
    return cur[off] == (uint8_t)fmt_index
        && cur[off + 1] == (uint8_t)frame_idx
        && le32(cur + off + 2) == interval;
}

/* 0 ok, 1 cancelled, -1 error (exception thrown) */
static int negotiate(JNIEnv *env, capture_t *cap) {
    if (atomic_load(&cap->stop)) {
        return 1;
    }
    uint8_t probe[34];
    uint8_t verify[34];
    memset(probe, 0, sizeof probe);
    int plen = cap->probe_len;
    uint16_t windex = (uint16_t)cap->vs_if;
    int r = ctrl_xfer(cap->fd, 0xA1, UVC_GET_CUR, (uint16_t)(UVC_VS_PROBE << 8), windex,
                      (uint16_t)plen, probe, CTRL_TIMEOUT_MS);
    if (r != plen) {
        if (r < 0) {
            throw_io(env, "querying the camera stream settings failed: %s", strerror(errno));
        } else {
            throw_io(env, "camera returned %d bytes of stream settings, expected %d", r, plen);
        }
        return -1;
    }
    /* spec layout has bmHint in bytes 0..1; stock firmware sends only one
     * bmHint byte, shifting the rest down. Accept either. */
    if (!probe_layout_matches(probe, 2, cap->fmt_index, cap->frame_idx, cap->frame_interval)
            && !probe_layout_matches(probe, 1, cap->fmt_index, cap->frame_idx, cap->frame_interval)) {
        throw_io(env, "camera will not stream its advertised %dx%d YUY2 format",
                 cap->width, cap->height);
        return -1;
    }
    if (atomic_load(&cap->stop)) {
        return 1;
    }
    r = ctrl_xfer(cap->fd, 0x21, UVC_SET_CUR, (uint16_t)(UVC_VS_PROBE << 8), windex,
                  (uint16_t)plen, probe, CTRL_TIMEOUT_MS);
    if (r < 0) {
        throw_io(env, "camera rejected the stream settings: %s", strerror(errno));
        return -1;
    }
    if (atomic_load(&cap->stop)) {
        return 1;
    }
    memset(verify, 0, sizeof verify);
    r = ctrl_xfer(cap->fd, 0xA1, UVC_GET_CUR, (uint16_t)(UVC_VS_PROBE << 8), windex,
                  (uint16_t)plen, verify, CTRL_TIMEOUT_MS);
    if (r != plen || memcmp(probe, verify, (size_t)plen) != 0) {
        throw_io(env, "camera did not accept the negotiated stream settings");
        return -1;
    }
    if (atomic_load(&cap->stop)) {
        return 1;
    }
    r = ctrl_xfer(cap->fd, 0x21, UVC_SET_CUR, (uint16_t)(UVC_VS_COMMIT << 8), windex,
                  (uint16_t)plen, probe, CTRL_TIMEOUT_MS);
    if (r < 0) {
        throw_io(env, "starting the camera stream failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------------- streaming ---------------- */

static int set_alt(capture_t *cap, int alt) {
    struct usbdevfs_setinterface si;
    memset(&si, 0, sizeof si);
    si.interface = (unsigned int)cap->vs_if;
    si.altsetting = (unsigned int)alt;
    return ioctl(cap->fd, USBDEVFS_SETINTERFACE, &si);
}

static int submit_urb(JNIEnv *env, capture_t *cap, run_ctx_t *rc, int slot) {
    if (slot < 0 || slot >= NUM_URBS || rc->urbs[slot] == NULL) {
        throw_io(env, "internal transfer slot error");
        return -1;
    }
    struct usbdevfs_urb *u = rc->urbs[slot];
    memset(u, 0, sizeof *u);
    u->type = USBDEVFS_URB_TYPE_ISO;
    u->endpoint = (unsigned char)cap->ep_addr;
    u->flags = USBDEVFS_URB_ISO_ASAP;
    u->buffer = rc->bufs[slot];
    u->buffer_length = cap->maxpkt * PKTS_PER_URB;
    u->number_of_packets = PKTS_PER_URB;
    for (int k = 0; k < PKTS_PER_URB; k++) {
        u->iso_frame_desc[k].length = (unsigned int)cap->maxpkt;
    }
    if (ioctl(cap->fd, USBDEVFS_SUBMITURB, u) < 0) {
        if (atomic_load(&cap->stop) || errno == ENODEV || errno == ENOENT
                || errno == ECONNRESET || errno == ESHUTDOWN) {
            return 0;
        }
        throw_io(env, "submitting video transfers failed: %s", strerror(errno));
        return -1;
    }
    rc->slot_flight[slot] = 1;
    rc->inflight++;
    return 0;
}

static int emit_frame(JNIEnv *env, run_ctx_t *rc, int width, int height) {
    jbyte *el = (*env)->GetByteArrayElements(env, rc->luma, NULL);
    if (el == NULL) {
        return -1;
    }
    extract_y(rc->parser.frame, (uint8_t *)el, width * height);
    (*env)->ReleaseByteArrayElements(env, rc->luma, el, 0);
    (*env)->CallVoidMethod(env, rc->listener, rc->mid, rc->luma,
                           (jint)width, (jint)height);
    if ((*env)->ExceptionCheck(env)) {
        return -1;
    }
    return 0;
}

static int process_urb(JNIEnv *env, capture_t *cap, run_ctx_t *rc, struct usbdevfs_urb *u) {
    if (u->status != 0 && u->status != -EXDEV) {
        if (atomic_load(&cap->stop)) {
            return 0;
        }
        throw_io(env, "USB video stream failed (status %d)", u->status);
        return -1;
    }
    size_t off = 0;
    for (int i = 0; i < u->number_of_packets; i++) {
        struct usbdevfs_iso_packet_desc *d = &u->iso_frame_desc[i];
        unsigned actual = d->actual_length;
        if (actual > d->length) {
            actual = d->length;
        }
        if (d->status == 0 && actual >= 2) {
            const uint8_t *pkt = (const uint8_t *)u->buffer + off;
            if (parse_packet(&rc->parser, pkt, actual) == 1) {
                if (emit_frame(env, rc, cap->width, cap->height) != 0) {
                    return -1;
                }
                if (atomic_load(&cap->stop)) {
                    return 0;
                }
            }
        } else if (d->status != 0) {
            /* lost or corrupted microframe: the frame being assembled
             * is unusable and gets dropped at its end */
            rc->parser.bad = 1;
            rc->parser.got = 0;
        }
        off += d->length;
    }
    return 0;
}

static int stream_loop(JNIEnv *env, capture_t *cap, run_ctx_t *rc) {
    while (!atomic_load(&cap->stop)) {
        struct pollfd fds[2];
        fds[0].fd = cap->fd;
        fds[0].events = POLLOUT;
        fds[0].revents = 0;
        fds[1].fd = cap->wake[0];
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        int pr = poll(fds, 2, POLL_TIMEOUT_MS);
        if (pr < 0 && errno != EINTR) {
            throw_io(env, "waiting for video data failed: %s", strerror(errno));
            return -1;
        }
        if (pr > 0 && (fds[1].revents & POLLIN) != 0) {
            char drain[32];
            (void)read(cap->wake[0], drain, sizeof drain);
        }
        for (;;) {
            if (atomic_load(&cap->stop)) {
                break;
            }
            struct usbdevfs_urb *u = NULL;
            if (ioctl(cap->fd, USBDEVFS_REAPURBNDELAY, &u) < 0) {
                if (errno != EAGAIN) {
                    if (atomic_load(&cap->stop)) {
                        break;
                    }
                    throw_io(env, "receiving video data failed: %s", strerror(errno));
                    return -1;
                }
                break;
            }
            int slot = -1;
            for (int i = 0; i < NUM_URBS; i++) {
                if (rc->urbs[i] == u) {
                    slot = i;
                    rc->slot_flight[i] = 0;
                    break;
                }
            }
            rc->inflight--;
            if (process_urb(env, cap, rc, u) != 0) {
                return -1;
            }
            if (atomic_load(&cap->stop)) {
                break;
            }
            if (submit_urb(env, cap, rc, slot) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

/*
 * Cancel every queued URB and reap until none is outstanding, bounded in
 * time. Draining before close keeps the kernel from freeing URB memory
 * while transfers are still in flight (the historic usbfs use-after-free).
 * The run thread calls this with its teardown reference held, before it
 * releases the interface and closes the fd.
 */
static void drain_urbs(capture_t *cap, run_ctx_t *rc) {
    for (int i = 0; i < NUM_URBS; i++) {
        if (rc->slot_flight[i] && rc->urbs[i] != NULL) {
            (void)ioctl(cap->fd, USBDEVFS_DISCARDURB, rc->urbs[i]);
        }
    }
    uint64_t deadline = now_ms() + DRAIN_TIMEOUT_MS;
    while (rc->inflight > 0) {
        struct usbdevfs_urb *u = NULL;
        if (ioctl(cap->fd, USBDEVFS_REAPURBNDELAY, &u) == 0 && u != NULL) {
            for (int i = 0; i < NUM_URBS; i++) {
                if (rc->urbs[i] == u) {
                    rc->slot_flight[i] = 0;
                    break;
                }
            }
            rc->inflight--;
            continue;
        }
        if (now_ms() >= deadline) {
            break;
        }
        struct pollfd pfd;
        pfd.fd = cap->fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        (void)poll(&pfd, 1, 20);
    }
}

static void free_run_ctx(run_ctx_t *rc) {
    for (int i = 0; i < NUM_URBS; i++) {
        free(rc->bufs[i]);
        free(rc->urbs[i]);
        rc->bufs[i] = NULL;
        rc->urbs[i] = NULL;
    }
    free(rc->parser.frame);
    rc->parser.frame = NULL;
}

/* ---------------- JNI entry points ---------------- */

JNIEXPORT jlong JNICALL
Java_dev_borges_chromaticproof_StockVideoCapture_nOpen(JNIEnv *env, jclass cls, jint fd) {
    (void)cls;
    return open_capture(env, fd);
}

JNIEXPORT void JNICALL
Java_dev_borges_chromaticproof_StockVideoCapture_nRun(JNIEnv *env, jclass cls,
                                                      jlong handle, jobject listener) {
    (void)cls;
    capture_t *cap = (capture_t *)(intptr_t)handle;
    if (cap == NULL || listener == NULL) {
        throw_io(env, "capture handle or listener missing");
        return;
    }
    /* Take the run reference before any state check: a concurrent close()
     * may drop the handle reference at any moment, and the checks below
     * must not race with the final unref freeing this object. */
    atomic_fetch_add(&cap->refs, 1);
    if (cap->fd < 0) {
        throw_io(env, "capture is already closed");
        goto out;
    }
    if (atomic_exchange(&cap->running, 1)) {
        throw_io(env, "capture is already running");
        goto out;
    }

    jclass lc = (*env)->GetObjectClass(env, listener);
    if (lc == NULL) {
        goto out;
    }
    jmethodID mid = (*env)->GetMethodID(env, lc, "frame", "([BII)V");
    if (mid == NULL) {
        goto out;
    }

    run_ctx_t rc;
    memset(&rc, 0, sizeof rc);
    rc.env = env;
    rc.listener = listener;
    rc.mid = mid;
    rc.parser.frame_bytes = cap->frame_bytes;
    rc.luma = (*env)->NewByteArray(env, (jsize)(cap->width * cap->height));
    if (rc.luma != NULL) {
        rc.parser.frame = malloc(cap->frame_bytes);
    }
    int setup_ok = rc.luma != NULL && rc.parser.frame != NULL;
    if (!setup_ok && !(*env)->ExceptionCheck(env)) {
        throw_io(env, "out of memory for frame buffers");
    }
    for (int i = 0; setup_ok && i < NUM_URBS; i++) {
        rc.bufs[i] = malloc((size_t)cap->maxpkt * PKTS_PER_URB);
        rc.urbs[i] = malloc(sizeof(struct usbdevfs_urb)
                            + (size_t)PKTS_PER_URB * sizeof(struct usbdevfs_iso_packet_desc));
        if (rc.bufs[i] == NULL || rc.urbs[i] == NULL) {
            setup_ok = 0;
            throw_io(env, "out of memory for transfer buffers");
        }
    }

    if (setup_ok) {
        int phase = negotiate(env, cap);
        if (phase == 0 && set_alt(cap, cap->alt) < 0) {
            if (!atomic_load(&cap->stop)) {
                throw_io(env, "selecting the video streaming alternate failed: %s",
                         strerror(errno));
                phase = -1;
            } else {
                phase = 1;
            }
        }
        if (phase == 0) {
            int ok = 1;
            for (int i = 0; ok && i < NUM_URBS; i++) {
                if (submit_urb(env, cap, &rc, i) != 0) {
                    ok = 0;
                }
            }
            if (ok) {
                stream_loop(env, cap, &rc);
            }
        }
    }

    /* Teardown under the run reference: the fd is still valid, all URB
     * buffers are drained before the release and close. cap_teardown_fd
     * makes the close idempotent in case the final unref also runs. */
    drain_urbs(cap, &rc);
    cap_teardown_fd(cap);
    free_run_ctx(&rc);

out:
    atomic_store(&cap->running, 0);
    cap_unref(cap);
}

JNIEXPORT void JNICALL
Java_dev_borges_chromaticproof_StockVideoCapture_nCancel(JNIEnv *env, jclass cls, jlong handle) {
    (void)env;
    (void)cls;
    capture_t *cap = (capture_t *)(intptr_t)handle;
    if (cap == NULL) {
        return;
    }
    cap_request_stop(cap);
}

JNIEXPORT void JNICALL
Java_dev_borges_chromaticproof_StockVideoCapture_nDestroy(JNIEnv *env, jclass cls, jlong handle) {
    (void)env;
    (void)cls;
    capture_t *cap = (capture_t *)(intptr_t)handle;
    if (cap == NULL) {
        return;
    }
    cap_request_stop(cap);
    cap_unref(cap);
}

