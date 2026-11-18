/* ============================================================
 *  Mystic OS - audio.c
 *  Playback to /dev/snd/pcmC0D0p through the ALSA UAPI, with no
 *  libasound and no headers beyond libc.
 *
 *  Why this file exists: the kernel probes an Intel HDA card in
 *  QEMU (-device intel-hda -device hda-duplex), devtmpfs creates
 *  /dev/snd/pcmC0D0p, and until now nothing ever opened it. A
 *  desktop that says "sound: 1 card(s), 2 pcm device(s)" and
 *  then cannot beep is not a desktop.
 *
 *  The four steps are the ones ALSA itself performs:
 *
 *   1. SNDRV_PCM_IOCTL_HW_PARAMS with a single requested access,
 *      format (S16_LE), subformat, channel count, rate, period
 *      size and buffer size. The kernel refines the derived
 *      parameters (period_time, periods, bytes, ...) itself and
 *      copies the accepted values back; we read them back rather
 *      than assume they were taken.
 *   2. SNDRV_PCM_IOCTL_PREPARE.
 *   3. SNDRV_PCM_IOCTL_WRITEI_FRAMES until every frame is queued.
 *      The driver returns the count it accepted, which can be
 *      less than asked; the remainder is retried. "No room yet"
 *      is a poll(POLLOUT) wait with a timeout, not a spin.
 *      EPIPE means the ring ran dry: PREPARE again and continue.
 *   4. SNDRV_PCM_IOCTL_DRAIN before close, with O_NONBLOCK
 *      cleared so the ioctl waits for the card instead of
 *      returning immediately and letting close() cut the tone.
 *
 *  musl has no <sound/asound.h>, so the ABI below is written out.
 *  It is not guessed: .work/audio/layout_check.c includes the
 *  real kernel header and fails to compile if sizeof, offsetof or
 *  any ioctl number differs from these definitions. The kernel
 *  side is sound/core/pcm_native.c (HW_PARAMS handler).
 * ============================================================ */
#include "audio.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ------------------------------------------------------------
 *  The ALSA UAPI structures this file needs (sound/asound.h).
 * ------------------------------------------------------------ */

/* snd_pcm_hw_param_t values. 0..2 are the mask parameters, 8..19
 * the interval parameters; the numbers are ABI, not an enum we
 * are free to renumber. */
#define HW_ACCESS      0
#define HW_FORMAT      1
#define HW_SUBFORMAT   2
#define HW_SAMPLE_BITS 8
#define HW_FRAME_BITS  9
#define HW_CHANNELS    10
#define HW_RATE        11
#define HW_PERIOD_SIZE 13
#define HW_BUFFER_SIZE 17

#define FIRST_INTERVAL HW_SAMPLE_BITS
#define LAST_INTERVAL  19               /* TICK_TIME, left for refine */

#define ACCESS_RW_INTERLEAVED 3         /* SNDRV_PCM_ACCESS_RW_INTERLEAVED */
#define FORMAT_S16_LE         2         /* SNDRV_PCM_FORMAT_S16_LE */
#define SUBFORMAT_STD         0         /* SNDRV_PCM_SUBFORMAT_STD */

#define MASK_MAX 256
#define MASK_WORDS (MASK_MAX / 32)

typedef struct {
    unsigned int min, max;
    unsigned int openmin : 1, openmax : 1, integer : 1, empty : 1;
} HwInterval;

typedef struct {
    unsigned int bits[MASK_WORDS];
} HwMask;

typedef struct {
    unsigned int flags;
    HwMask masks[3];                    /* ACCESS, FORMAT, SUBFORMAT */
    HwMask mres[5];
    HwInterval intervals[LAST_INTERVAL - FIRST_INTERVAL + 1];
    HwInterval ires[9];
    unsigned int rmask, cmask, info, msbits, rate_num, rate_den;
    unsigned long long fifo_size;       /* snd_pcm_uframes_t, 8 bytes here */
    unsigned char sync[16];
    unsigned char reserved[48];
} HwParams;

typedef struct {
    long result;                        /* snd_pcm_sframes_t */
    void *buf;
    unsigned long long frames;          /* snd_pcm_uframes_t, 8 bytes here */
} HwXferi;

/* _IOC encoding, written out so the request value is pinned even
 * if a libc header grew a different helper. */
#define IOC(dir, type, nr, size) \
    ((unsigned int)((dir) << 30) | ((unsigned int)(size) << 16) | \
     ((unsigned int)(type) << 8) | (unsigned int)(nr))

#define PCM_IOCTL_HW_PARAMS   IOC(3, 'A', 0x11, sizeof(HwParams))
#define PCM_IOCTL_PREPARE     IOC(0, 'A', 0x40, 0)
#define PCM_IOCTL_DRAIN       IOC(0, 'A', 0x44, 0)
#define PCM_IOCTL_WRITEI      IOC(1, 'A', 0x50, sizeof(HwXferi))

_Static_assert(sizeof(HwMask) == 32, "snd_mask is 8 u32");
_Static_assert(sizeof(HwInterval) == 12, "snd_interval is 12 bytes");
_Static_assert(sizeof(HwParams) == 608, "snd_pcm_hw_params is 608 bytes");
_Static_assert(offsetof(HwParams, fifo_size) == 536, "fifo_size offset");
_Static_assert(sizeof(HwXferi) == 24, "snd_xferi is 24 bytes");
_Static_assert(PCM_IOCTL_HW_PARAMS == 0xC2604111u, "HW_PARAMS ioctl number");
_Static_assert(PCM_IOCTL_PREPARE == 0x4140u, "PREPARE ioctl number");
_Static_assert(PCM_IOCTL_DRAIN == 0x4144u, "DRAIN ioctl number");
_Static_assert(PCM_IOCTL_WRITEI == 0x40184150u, "WRITEI ioctl number");

/* ABI check against the kernel's own header. This is a build-time test
 * (compile audio.c with -DMYSTIC_AUDIO_ABI_CHECK); it costs nothing in
 * a normal build and it is the reason the hand-written struct above can
 * be trusted on the real card. */
#if defined(MYSTIC_AUDIO_ABI_CHECK)
#include <sound/asound.h>
_Static_assert(sizeof(HwMask) == sizeof(struct snd_mask), "mask size");
_Static_assert(sizeof(HwInterval) == sizeof(struct snd_interval), "interval size");
_Static_assert(sizeof(HwParams) == sizeof(struct snd_pcm_hw_params), "hw_params size");
_Static_assert(offsetof(HwParams, flags) ==
               offsetof(struct snd_pcm_hw_params, flags), "flags offset");
_Static_assert(offsetof(HwParams, masks) ==
               offsetof(struct snd_pcm_hw_params, masks), "masks offset");
_Static_assert(offsetof(HwParams, intervals) ==
               offsetof(struct snd_pcm_hw_params, intervals), "intervals offset");
_Static_assert(offsetof(HwParams, rmask) ==
               offsetof(struct snd_pcm_hw_params, rmask), "rmask offset");
_Static_assert(offsetof(HwParams, fifo_size) ==
               offsetof(struct snd_pcm_hw_params, fifo_size), "fifo_size offset");
_Static_assert(sizeof(HwXferi) == sizeof(struct snd_xferi), "snd_xferi size");
_Static_assert(offsetof(HwXferi, result) == offsetof(struct snd_xferi, result), "xferi result");
_Static_assert(offsetof(HwXferi, buf) == offsetof(struct snd_xferi, buf), "xferi buf");
_Static_assert(offsetof(HwXferi, frames) == offsetof(struct snd_xferi, frames), "xferi frames");
_Static_assert((unsigned long)PCM_IOCTL_HW_PARAMS ==
               (unsigned long)SNDRV_PCM_IOCTL_HW_PARAMS, "HW_PARAMS number");
_Static_assert((unsigned long)PCM_IOCTL_PREPARE ==
               (unsigned long)SNDRV_PCM_IOCTL_PREPARE, "PREPARE number");
_Static_assert((unsigned long)PCM_IOCTL_DRAIN ==
               (unsigned long)SNDRV_PCM_IOCTL_DRAIN, "DRAIN number");
_Static_assert((unsigned long)PCM_IOCTL_WRITEI ==
               (unsigned long)SNDRV_PCM_IOCTL_WRITEI_FRAMES, "WRITEI number");
_Static_assert(HW_ACCESS == SNDRV_PCM_HW_PARAM_ACCESS, "ACCESS number");
_Static_assert(HW_FORMAT == SNDRV_PCM_HW_PARAM_FORMAT, "FORMAT number");
_Static_assert(HW_SUBFORMAT == SNDRV_PCM_HW_PARAM_SUBFORMAT, "SUBFORMAT number");
_Static_assert(HW_SAMPLE_BITS == SNDRV_PCM_HW_PARAM_SAMPLE_BITS, "SAMPLE_BITS number");
_Static_assert(HW_FRAME_BITS == SNDRV_PCM_HW_PARAM_FRAME_BITS, "FRAME_BITS number");
_Static_assert(HW_CHANNELS == SNDRV_PCM_HW_PARAM_CHANNELS, "CHANNELS number");
_Static_assert(HW_RATE == SNDRV_PCM_HW_PARAM_RATE, "RATE number");
_Static_assert(HW_PERIOD_SIZE == SNDRV_PCM_HW_PARAM_PERIOD_SIZE, "PERIOD_SIZE number");
_Static_assert(HW_BUFFER_SIZE == SNDRV_PCM_HW_PARAM_BUFFER_SIZE, "BUFFER_SIZE number");
_Static_assert(FIRST_INTERVAL == SNDRV_PCM_HW_PARAM_FIRST_INTERVAL, "first interval");
_Static_assert(LAST_INTERVAL == SNDRV_PCM_HW_PARAM_LAST_INTERVAL, "last interval");
_Static_assert(ACCESS_RW_INTERLEAVED == SNDRV_PCM_ACCESS_RW_INTERLEAVED, "access value");
_Static_assert(FORMAT_S16_LE == SNDRV_PCM_FORMAT_S16_LE, "format value");
_Static_assert(SUBFORMAT_STD == SNDRV_PCM_SUBFORMAT_STD, "subformat value");
#endif /* MYSTIC_AUDIO_ABI_CHECK */

/* ------------------------------------------------------------
 *  State and error reporting
 * ------------------------------------------------------------ */

#define POLL_TIMEOUT_MS 2000

static int  g_fd = -1;
static int  g_prepared;
static int  g_rate;
static int  g_channels;
static char g_err[192] = "no failure recorded";

/* Keep errno exactly as the failing call left it, then format the
 * message from it. snprintf() may touch errno, so it is saved first. */
static void note_error(const char *what) {
    int e = errno;
    snprintf(g_err, sizeof g_err, "%s: %s: %s (errno %d)",
             AUDIO_DEVICE, what, strerror(e), e);
    errno = e;
}

const char *audio_device(void) { return AUDIO_DEVICE; }
const char *audio_error(void)  { return g_err; }
int audio_ready(void)          { return g_fd >= 0 && g_prepared; }
int audio_rate(void)           { return g_rate; }
int audio_channels(void)       { return g_channels; }

/* ------------------------------------------------------------
 *  Building the hw_params request
 * ------------------------------------------------------------ */

static void mask_any(HwMask *m) {
    for (int i = 0; i < MASK_WORDS; i++) m->bits[i] = 0xffffffffu;
}

static void mask_only(HwMask *m, unsigned bit) {
    for (int i = 0; i < MASK_WORDS; i++) m->bits[i] = 0;
    m->bits[bit / 32] = 1u << (bit % 32);
}

static HwInterval *iv(HwParams *p, int param) {
    return &p->intervals[param - FIRST_INTERVAL];
}

/* snd_interval_any(): every value allowed, no integer requirement yet. */
static void interval_any(HwInterval *i) {
    i->min = 0;
    i->max = 0xffffffffu;
    i->openmin = 0;
    i->openmax = 0;
    i->integer = 0;
    i->empty = 0;
}

static void interval_set(HwInterval *i, unsigned lo, unsigned hi) {
    i->min = lo;
    i->max = hi;
    i->openmin = 0;
    i->openmax = 0;
    i->integer = 1;
    i->empty = 0;
}

/* One exact value for the seven parameters the caller asked for.
 * Everything derived from them (sample_bits, frame_bits, period_time,
 * period_bytes, periods, buffer_time, buffer_bytes, tick_time) is
 * left as an open interval: the kernel's refine rules compute those
 * from rate/period_size/buffer_size, and if we pinned them ourselves
 * a single rounding difference would make the intersection empty
 * and turn a valid request into EINVAL. */
static void params_build(HwParams *p, int rate, int channels,
                         unsigned period_size, unsigned periods) {
    memset(p, 0, sizeof *p);
    for (int i = 0; i < 3; i++) mask_any(&p->masks[i]);
    for (int i = 0; i < 5; i++) mask_any(&p->mres[i]);
    for (int i = 0; i <= LAST_INTERVAL - FIRST_INTERVAL; i++)
        interval_any(&p->intervals[i]);
    for (int i = 0; i < 9; i++)
        interval_any(&p->ires[i]);

    mask_only(&p->masks[HW_ACCESS], ACCESS_RW_INTERLEAVED);
    mask_only(&p->masks[HW_FORMAT], FORMAT_S16_LE);
    mask_only(&p->masks[HW_SUBFORMAT], SUBFORMAT_STD);

    interval_set(iv(p, HW_CHANNELS), (unsigned)channels, (unsigned)channels);
    interval_set(iv(p, HW_RATE), (unsigned)rate, (unsigned)rate);
    interval_set(iv(p, HW_PERIOD_SIZE), period_size, period_size);
    interval_set(iv(p, HW_BUFFER_SIZE), period_size * periods,
                 period_size * periods);
}

/* Read back what the card accepted, not what we asked for. */
static void params_read_back(const HwParams *p) {
    const HwInterval *r = &p->intervals[HW_RATE - FIRST_INTERVAL];
    const HwInterval *c = &p->intervals[HW_CHANNELS - FIRST_INTERVAL];
    g_rate = (int)r->min;
    g_channels = (int)c->min;
    if (g_rate < 1) g_rate = 0;
    /* The API accepts 1 or 2 channels and the request above pinned the
     * count, so this only guards callers' interleaved buffers against a
     * driver that copied back something impossible. */
    if (g_channels < 1) g_channels = 1;
    if (g_channels > 2) g_channels = 2;
}

/* ------------------------------------------------------------
 *  Waits
 * ------------------------------------------------------------ */

/* Wait until the PCM reports room. Returns 0 when writable, -1 when
 * poll failed or the timeout really elapsed (errno set). */
static int wait_writable(void) {
    struct pollfd p;
    p.fd = g_fd;
    p.events = POLLOUT;
    p.revents = 0;
    for (;;) {
        int n = poll(&p, 1, POLL_TIMEOUT_MS);
        if (n > 0) {
            if (p.revents & (POLLNVAL | POLLHUP)) {
                errno = EIO;
                note_error("wait for POLLOUT: device went away");
                return -1;
            }
            /* POLLERR is how an XRUN shows up. Re-arm the stream here so
             * a driver that reports the error without failing the next
             * write cannot turn this wait into a spin. */
            if (p.revents & POLLERR) {
                if (ioctl(g_fd, PCM_IOCTL_PREPARE) < 0) {
                    note_error("PREPARE after POLLERR");
                    return -1;
                }
            }
            return 0;
        }
        if (n == 0) {
            int e = ETIMEDOUT;
            snprintf(g_err, sizeof g_err,
                     "%s: write: no room in the ring for %d ms "
                     "(errno %d, ETIMEDOUT)", AUDIO_DEVICE,
                     POLL_TIMEOUT_MS, e);
            errno = e;
            return -1;
        }
        if (errno != EINTR) {
            note_error("wait for POLLOUT");
            return -1;
        }
    }
}

/* ------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------ */

int audio_open(int rate, int channels) {
    if (rate <= 0 || channels <= 0 || channels > 2) {
        errno = EINVAL;
        snprintf(g_err, sizeof g_err,
                 "%s: invalid request %d Hz / %d ch (errno %d, EINVAL)",
                 AUDIO_DEVICE, rate, channels, errno);
        return -1;
    }

    if (g_fd >= 0) audio_close();       /* one stream at a time */

    g_prepared = 0;
    g_rate = 0;
    g_channels = 0;
    errno = 0;

    int fd = open(AUDIO_DEVICE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        note_error("open");
        return -1;
    }
    g_fd = fd;

    /* 20 ms periods, four of them: 80 ms of slack for an app that is
     * also drawing a window, while a 500 ms tone still leaves the card
     * without a long tail to drain when it is dismissed. */
    unsigned period = (unsigned)(rate / 50);
    if (period < 32) period = 32;
    unsigned periods = 4;

    HwParams p;
    params_build(&p, rate, channels, period, periods);
    if (ioctl(g_fd, PCM_IOCTL_HW_PARAMS, &p) < 0) {
        note_error("HW_PARAMS");
        close(g_fd);
        g_fd = -1;
        return -1;
    }
    params_read_back(&p);

    if (ioctl(g_fd, PCM_IOCTL_PREPARE) < 0) {
        note_error("PREPARE");
        close(g_fd);
        g_fd = -1;
        return -1;
    }

    g_prepared = 1;
    g_err[0] = 0;
    return 0;
}

int audio_write(const short *frames, int nframes) {
    if (g_fd < 0 || !g_prepared) {
        errno = EBADF;
        snprintf(g_err, sizeof g_err,
                 "%s: write: no open stream (errno %d, EBADF)",
                 AUDIO_DEVICE, errno);
        return -1;
    }
    if (!frames || nframes < 0) {
        errno = EINVAL;
        note_error("write: invalid buffer");
        return -1;
    }
    if (nframes == 0) return 0;

    int done = 0;
    int stalled = 0;
    int waits = 0;
    while (done < nframes) {
        HwXferi x;
        x.result = 0;
        x.buf = (void *)(frames + (size_t)done * (size_t)g_channels);
        x.frames = (unsigned long)(nframes - done);

        if (ioctl(g_fd, PCM_IOCTL_WRITEI, &x) < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                /* A companion bound to the poll timeout: even a driver
                 * that always claims room must not keep us here forever. */
                if (++waits > 256) {
                    errno = EIO;
                    note_error("write: 256 waits and no progress");
                    return -1;
                }
                if (wait_writable() < 0) return -1;
                continue;
            }
            if (errno == EPIPE) {
                /* The ring ran dry while the app was busy. Not fatal:
                 * re-arm and keep this same buffer. */
                if (ioctl(g_fd, PCM_IOCTL_PREPARE) < 0) {
                    note_error("PREPARE after underrun");
                    return -1;
                }
                continue;
            }
            note_error("WRITEI_FRAMES");
            return -1;
        }

        if (x.result <= 0) {
            /* A zero-length accept is not progress; wait like EAGAIN.
             * A driver that keeps claiming room and accepting nothing is
             * broken: fail after a bounded number of waits, never spin. */
            if (++stalled > 16) {
                errno = EIO;
                note_error("write: driver accepts no frames");
                return -1;
            }
            if (wait_writable() < 0) return -1;
            continue;
        }
        stalled = 0;
        done += (int)x.result;
    }
    return done;
}

void audio_close(void) {
    if (g_fd < 0) return;

    /* DRAIN on a non-blocking fd returns as soon as draining starts,
     * and close() would then throw the queued audio away. Switch the
     * descriptor back to blocking for the drain only, so the 500 ms
     * beep is really finished when this returns. */
    int fl = fcntl(g_fd, F_GETFL);
    if (fl >= 0) {
        if (fcntl(g_fd, F_SETFL, fl & ~O_NONBLOCK) < 0)
            note_error("fcntl(F_SETFL, blocking)");
    } else {
        note_error("fcntl(F_GETFL)");
    }

    if (ioctl(g_fd, PCM_IOCTL_DRAIN) < 0) {
        int e = errno;
        note_error("DRAIN");
        /* audio_close() cannot return an error, so the real one is
         * printed once rather than swallowed. EPIPE means the ring was
         * already dry - nothing was lost. */
        if (e != EPIPE)
            fprintf(stderr, "audio: %s\n", g_err);
    }

    close(g_fd);
    g_fd = -1;
    g_prepared = 0;
}
