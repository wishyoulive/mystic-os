/* ============================================================
 *  Mystic OS - tools/beep.c
 *
 *      beep [frequency] [milliseconds]      (default 440 Hz, 250 ms)
 *      beep -f file.wav                    16-bit PCM WAVE
 *
 *  The smallest thing that proves the audio path is real. It opens
 *  /dev/snd/pcmC0D0p through src/audio.c, prints the device it
 *  actually opened, and prints the number of frames that were
 *  queued. There is no "ok" without a frame count.
 *
 *  A sine with a linear fade in and out: a rectangular gate on a
 *  sine clicks at both ends, and a click is not a test tone.
 *
 *  WAVE support is deliberately narrow and honest: RIFF/WAVE,
 *  PCM (format tag 1, or WAVE_FORMAT_EXTENSIBLE with the PCM
 *  subformat), 16 bits per sample, 1 or 2 channels. Anything else
 *  is rejected with the reason, not played as noise.
 * ============================================================ */
#include "../audio.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAU 6.28318530717958647692
#define AMPLITUDE 0.25              /* -12 dBFS: audible, no clipping */
#define FADE_MS 5.0
#define CHUNK_FRAMES 1024
#define IN_FRAMES    8192
#define MAX_RATE 192000

static void usage(void) {
    fprintf(stderr,
            "usage: beep [frequency] [milliseconds]\n"
            "       beep -f file.wav\n");
}

/* ---------- little-endian WAVE header parsing ---------- */

static unsigned long le32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static unsigned int le16(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* Fill `n` bytes or report why not. Returns 0 on success. */
static int read_exact(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) { errno = EIO; return -1; }   /* short header/file */
        got += (size_t)r;
    }
    return 0;
}

typedef struct {
    int    rate;
    int    channels;
    int    bits;
    long   data_off;
    unsigned long data_len;         /* bytes, already clamped to the file */
} Wav;

/* Returns 0 and fills w on success; -1 with a message in err. */
static int wav_open(const char *path, int *out_fd, Wav *w,
                    char *err, size_t errlen) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errlen, "%s: %s (errno %d)", path, strerror(errno), errno);
        return -1;
    }

    unsigned char h[12];
    if (read_exact(fd, h, sizeof h) != 0 ||
        memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
        snprintf(err, errlen, "%s: not a RIFF/WAVE file", path);
        close(fd);
        return -1;
    }

    int have_fmt = 0;
    unsigned int tag = 0;
    memset(w, 0, sizeof *w);

    for (;;) {
        unsigned char ck[8];
        if (read_exact(fd, ck, sizeof ck) != 0) {
            snprintf(err, errlen, "%s: no data chunk before end of file", path);
            close(fd);
            return -1;
        }
        unsigned long sz = le32(ck + 4);

        if (memcmp(ck, "fmt ", 4) == 0) {
            unsigned char f[40];
            size_t want = sz < sizeof f ? (size_t)sz : sizeof f;
            if (want < 16 || read_exact(fd, f, want) != 0) {
                snprintf(err, errlen, "%s: truncated fmt chunk", path);
                close(fd);
                return -1;
            }
            tag        = le16(f);
            w->channels = (int)le16(f + 2);
            w->rate     = (int)le32(f + 4);
            w->bits     = (int)le16(f + 14);
            have_fmt = 1;

            /* WAVE_FORMAT_EXTENSIBLE: the real format is in the first two
             * bytes of the SubFormat GUID at offset 24. */
            if (tag == 0xfffe) {
                if (want < 40) {
                    snprintf(err, errlen, "%s: extensible fmt chunk too short", path);
                    close(fd);
                    return -1;
                }
                tag = le16(f + 24);
            }
            /* step over the rest of the chunk plus its pad byte */
            if (sz > want) {
                if (lseek(fd, (long)(sz - want) + (long)(sz & 1), SEEK_CUR) < 0) {
                    snprintf(err, errlen, "%s: cannot skip fmt tail: %s",
                             path, strerror(errno));
                    close(fd);
                    return -1;
                }
            } else if (sz & 1) {
                if (lseek(fd, 1, SEEK_CUR) < 0) { /* pad byte */ }
            }
            continue;
        }

        if (memcmp(ck, "data", 4) == 0) {
            if (!have_fmt) {
                snprintf(err, errlen, "%s: data chunk before fmt chunk", path);
                close(fd);
                return -1;
            }
            long off = lseek(fd, 0, SEEK_CUR);
            if (off < 0) {
                snprintf(err, errlen, "%s: %s (errno %d)", path,
                         strerror(errno), errno);
                close(fd);
                return -1;
            }
            long end = lseek(fd, 0, SEEK_END);
            if (end < off) end = off;
            if (sz > (unsigned long)(end - off))
                sz = (unsigned long)(end - off);      /* trust the file size */
            if (lseek(fd, off, SEEK_SET) < 0) {
                snprintf(err, errlen, "%s: %s (errno %d)", path,
                         strerror(errno), errno);
                close(fd);
                return -1;
            }
            w->data_off = off;
            w->data_len = sz;
            break;
        }

        /* unknown chunk: skip it and its pad byte */
        if (lseek(fd, (long)sz + (long)(sz & 1), SEEK_CUR) < 0) {
            snprintf(err, errlen, "%s: cannot skip %.4s chunk", path, ck);
            close(fd);
            return -1;
        }
    }

    if (tag != 1) {
        snprintf(err, errlen,
                 "%s: WAVE format tag %u is not PCM (only 1 and "
                 "extensible-PCM are played)", path, tag);
        close(fd);
        return -1;
    }
    if (w->bits != 16) {
        snprintf(err, errlen, "%s: %d bits per sample, this tool plays 16",
                 path, w->bits);
        close(fd);
        return -1;
    }
    if (w->channels != 1 && w->channels != 2) {
        snprintf(err, errlen, "%s: %d channels, this tool plays 1 or 2",
                 path, w->channels);
        close(fd);
        return -1;
    }
    if (w->rate <= 0 || w->rate > MAX_RATE) {
        snprintf(err, errlen, "%s: impossible sample rate %d", path, w->rate);
        close(fd);
        return -1;
    }

    /* whole frames only: a stray byte would shift every later sample */
    unsigned long frame_bytes = (unsigned long)w->channels * 2ul;
    w->data_len -= w->data_len % frame_bytes;

    *out_fd = fd;
    return 0;
}

/* ---------- tone generation ---------- */

/* One interleaved chunk of a sine. `first` is the absolute frame index,
 * so the phase is continuous across chunks and the two fades are at the
 * real ends of the whole tone. */
static void sine_chunk(short *out, int frames, int first, int total,
                       int rate, int channels, double freq) {
    long fade = (long)(rate * FADE_MS / 1000.0);
    if (fade < 8) fade = 8;
    if (fade > total / 2) fade = total / 2;
    if (fade < 1) fade = 1;

    for (int i = 0; i < frames; i++) {
        long n = first + i;
        double env = 1.0;
        if (n < fade) env = (double)n / (double)fade;
        else if (n >= total - fade) env = (double)(total - 1 - n) / (double)fade;
        if (env < 0.0) env = 0.0;
        double phase = TAU * freq * (double)n / (double)rate;
        short s = (short)(sin(phase) * env * AMPLITUDE * 32767.0);
        for (int c = 0; c < channels; c++) out[i * channels + c] = s;
    }
}

static int play_sine(double freq, int ms) {
    if (audio_open(48000, 2) != 0) {
        fprintf(stderr, "beep: %s\n", audio_error());
        return -1;
    }

    int rate = audio_rate();
    int ch = audio_channels();
    if (rate <= 0 || ch < 1) {
        fprintf(stderr, "beep: %s: driver accepted an unusable stream\n",
                audio_device());
        audio_close();
        return -1;
    }

    long total = (long)((double)rate * ms / 1000.0);
    if (total < 1) total = 1;

    printf("beep: %s  %d Hz  %d ch\n", audio_device(), rate, ch);

    short buf[CHUNK_FRAMES * 2];
    long done = 0;
    while (done < total) {
        int n = (total - done) > CHUNK_FRAMES ? CHUNK_FRAMES : (int)(total - done);
        sine_chunk(buf, n, (int)done, (int)total, rate, ch, freq);
        int w = audio_write(buf, n);
        if (w < 0) {
            fprintf(stderr, "beep: %s\n", audio_error());
            audio_close();
            return -1;
        }
        done += w;
    }
    audio_close();

    printf("beep: %ld frames written (%.3f s, %.1f Hz)\n",
           done, (double)done / rate, freq);
    return 0;
}

static int play_wav(const char *path) {
    int fd = -1;
    Wav w;
    char err[256];
    if (wav_open(path, &fd, &w, err, sizeof err) != 0) {
        fprintf(stderr, "beep: %s\n", err);
        return -1;
    }

    unsigned long frames_in_file = w.data_len / ((unsigned long)w.channels * 2ul);
    unsigned long frames_limit = (unsigned long)w.rate * 10ul;   /* 10 s */
    if (frames_in_file > frames_limit) {
        fprintf(stderr, "beep: %s: %lu frames is longer than 10 s at %d Hz\n",
                path, frames_in_file, w.rate);
        close(fd);
        return -1;
    }

    /* /dev/snd/pcmC0D0p is a stereo DAC: always ask for two channels and
     * upmix a mono file rather than let the card reject it. Try the file's
     * own rate first (no conversion, best quality); if the card refuses
     * that rate, fall back to 48000 Hz, which every HDA controller and
     * QEMU's hda-duplex accept, and resample. */
    int dev_want = w.rate;
    char first_error[224] = "";
    if (audio_open(dev_want, 2) != 0) {
        snprintf(first_error, sizeof first_error, "%s", audio_error());
        dev_want = 48000;
        if (audio_open(dev_want, 2) != 0) {
            fprintf(stderr, "beep: %s\n", first_error);
            fprintf(stderr, "beep: %s\n", audio_error());
            close(fd);
            return -1;
        }
        printf("beep: file is %d Hz; the card refused it, playing at 48000 Hz\n",
               w.rate);
    }

    int rate = audio_rate() > 0 ? audio_rate() : dev_want;
    int ch = audio_channels() > 0 ? audio_channels() : 2;
    printf("beep: %s  %d Hz  %d ch\n", audio_device(), rate, ch);

    unsigned long long num = (unsigned long long)frames_in_file * (unsigned)rate;
    unsigned long out_frames =
        (unsigned long)((num + (unsigned)w.rate - 1) / (unsigned)w.rate);
    if (out_frames == 0 && frames_in_file > 0) out_frames = 1;

    /* A sliding window of source frames, so a 10 s file is never held in
     * memory. 8192 frames is more than one 1024-frame output chunk needs
     * even at the 4:1 ratio of a 192 kHz file into a 48 kHz device. */
    short inbuf[IN_FRAMES * 2];
    short outbuf[CHUNK_FRAMES * 2];
    long in_start = 0, in_len = 0;
    unsigned long done = 0;

    while (done < out_frames) {
        int n = (out_frames - done) > CHUNK_FRAMES
                    ? CHUNK_FRAMES : (int)(out_frames - done);
        long want_start = (long)((double)done * w.rate / rate);
        long want_end = (long)((double)(done + (unsigned)n - 1) * w.rate / rate) + 2;
        if (want_end > (long)frames_in_file) want_end = (long)frames_in_file;
        if (want_start >= (long)frames_in_file) break;

        while (in_start + in_len < want_end) {
            if (in_len >= IN_FRAMES) {
                long drop = want_start - in_start;
                if (drop <= 0) {
                    fprintf(stderr,
                            "beep: %s: cannot follow the rate conversion\n", path);
                    audio_close();
                    close(fd);
                    return -1;
                }
                memmove(inbuf, inbuf + drop * w.channels,
                        (size_t)(in_len - drop) * w.channels * sizeof(short));
                in_start += drop;
                in_len -= drop;
            }
            long room = IN_FRAMES - in_len;
            long need = want_end - (in_start + in_len);
            long left = (long)frames_in_file - (in_start + in_len);
            if (need > left) need = left;
            if (need <= 0) break;
            if (need > room) need = room;
            if (read_exact(fd, inbuf + in_len * w.channels,
                           (size_t)need * w.channels * sizeof(short)) != 0) {
                fprintf(stderr, "beep: %s: short read after %ld source frames: %s\n",
                        path, in_start + in_len, strerror(errno));
                audio_close();
                close(fd);
                return -1;
            }
            in_len += need;
        }
        if (in_len <= 0) break;

        for (int i = 0; i < n; i++) {
            double p = (double)(done + (unsigned)i) * w.rate / rate;
            long i0 = (long)p;
            double frac = p - (double)i0;
            long a = i0 - in_start;
            long b = a + 1;
            if (a < 0) { a = 0; frac = 0.0; }
            if (a >= in_len) a = in_len - 1;
            if (b >= in_len) b = in_len - 1;
            if (b < 0) b = 0;
            for (int c = 0; c < ch; c++) {
                int sc = c < w.channels ? c : 0;   /* mono -> both channels */
                double va = inbuf[a * w.channels + sc];
                double vb = inbuf[b * w.channels + sc];
                outbuf[i * ch + c] =
                    (short)lround((1.0 - frac) * va + frac * vb);
            }
        }

        int wr = audio_write(outbuf, n);
        if (wr < 0) {
            fprintf(stderr, "beep: %s\n", audio_error());
            audio_close();
            close(fd);
            return -1;
        }
        done += (unsigned long)wr;
    }

    audio_close();
    close(fd);
    printf("beep: %lu frames written from %s (%.3f s%s)\n",
           done, path, (double)done / rate,
           rate != w.rate ? ", resampled" : "");
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage();
        return 0;
    }

    if (argc >= 2 && !strcmp(argv[1], "-f")) {
        if (argc != 3) { usage(); return 2; }
        return play_wav(argv[2]) == 0 ? 0 : 1;
    }

    double freq = 440.0;
    int ms = 250;

    if (argc >= 2) {
        char *end = NULL;
        freq = strtod(argv[1], &end);
        if (!end || *end || !(freq >= 20.0 && freq <= 20000.0)) {
            fprintf(stderr, "beep: frequency must be 20..20000 Hz\n");
            return 2;
        }
    }
    if (argc >= 3) {
        char *end = NULL;
        long v = strtol(argv[2], &end, 10);
        if (!end || *end || v < 1 || v > 60000) {
            fprintf(stderr, "beep: duration must be 1..60000 ms\n");
            return 2;
        }
        ms = (int)v;
    }
    if (argc > 3) { usage(); return 2; }

    return play_sine(freq, ms) == 0 ? 0 : 1;
}
