/* ============================================================
 *  Mystic OS - audio.h
 *  Stereo PCM playback to the kernel's HDA card, with no ALSA
 *  library and no external dependency.
 *
 *  The device is the first playback PCM: /dev/snd/pcmC0D0p. It
 *  is opened O_RDWR | O_NONBLOCK so that "the buffer is full"
 *  is a poll(POLLOUT) wait with a timeout, never a spin.
 *
 *  The protocol spoken to the kernel is ALSA's UAPI:
 *
 *      SNDRV_PCM_IOCTL_HW_PARAMS     pick access/format/rate/
 *                                    channels/period/buffer
 *      SNDRV_PCM_IOCTL_PREPARE       arm the stream
 *      SNDRV_PCM_IOCTL_WRITEI_FRAMES copy frames in
 *      SNDRV_PCM_IOCTL_DRAIN         wait until the queued audio
 *                                    has left the card
 *
 *  musl ships no <sound/asound.h>, so the four structures and
 *  the ioctl numbers this file needs are defined here from the
 *  UAPI itself. They are ABI, not preference: the layout is
 *  checked against the real header by .work/audio/layout_check.c
 *  (a compile-time compare of sizeof/offsetof/ioctl values).
 *
 *  Everything returns the truth: 0 on success, -1 on failure
 *  with errno set by the syscall or ioctl that failed, and
 *  audio_error() describing that exact failure.
 * ============================================================ */
#ifndef MYSTIC_AUDIO_H
#define MYSTIC_AUDIO_H

/* The one playback device this OS has (card 0, device 0, playback). */
#define AUDIO_DEVICE "/dev/snd/pcmC0D0p"

/* Open and configure the PCM. `rate` in Hz, `channels` 1 or 2.
 * Returns 0 on success, -1 on failure (errno set, audio_error() filled). */
int  audio_open(int rate, int channels);

/* Queue `nframes` interleaved 16-bit frames. Loops over partial writes
 * and waits for POLLOUT with a timeout; never spins. Returns the number
 * of frames queued (== nframes) or -1 on failure. */
int  audio_write(const short *frames, int nframes);

/* Drain what is queued (blocking until the card has played it) and close.
 * Safe to call when nothing is open. */
void audio_close(void);

/* 1 while the PCM is open and prepared, 0 otherwise. */
int  audio_ready(void);

/* The path that audio_open() actually opened. Never NULL. */
const char *audio_device(void);

/* The last failure, e.g.
 *   "/dev/snd/pcmC0D0p: open: No such file or directory (errno 2)"
 * or "no failure recorded". Never NULL; valid until the next call. */
const char *audio_error(void);

/* What the hardware/driver settled on after HW_PARAMS. Valid while open. */
int  audio_rate(void);
int  audio_channels(void);

#endif /* MYSTIC_AUDIO_H */
