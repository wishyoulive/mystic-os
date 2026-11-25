/* ============================================================
 *  Mystic OS - vlcmin.h
 *
 *  The small slice of the libVLC 3 ABI that src/client/apps/vlcplay.c
 *  uses, declared here because the image has no VLC development
 *  headers and no C library that ships them.
 *
 *  Every declaration below is transcribed from VLC 3.0.23's own
 *  public headers - vlc/libvlc.h, vlc/libvlc_media.h and
 *  vlc/libvlc_media_player.h - which are the ABI that libvlc.so.5
 *  exports. The types are the ABI: an opaque struct is only ever
 *  handled as a pointer, libvlc_time_t is int64_t, and the three
 *  vmem callbacks are the exact function-pointer types the library
 *  will call back through. Nothing here is invented and nothing is
 *  linked: vlcplay.c resolves each function with dlsym() and reports
 *  the missing symbol's name instead of pretending it has it.
 *
 *  The function list is one macro, VLC_MIN_FUNCTIONS, so the
 *  declarations, the dlsym() table and the binder cannot drift apart.
 *  vlcplay.c uses __typeof__(&libvlc_new) for each table member, so
 *  the compiler checks every pointer against the prototype printed
 *  below.
 *
 *  PIXEL FORMAT.  The app asks for "RV32" through
 *  libvlc_video_set_format(): modules/video_output/vmem.c sets
 *      rmask 0xff0000, gmask 0x00ff00, bmask 0x0000ff
 *  so one pixel is 0x00RRGGBB in a uint32_t. On little-endian the
 *  bytes in memory are B, G, R, X. Mystic's colour is 0xAARRGGBB
 *  (src/mystic.h), whose bytes are B, G, R, A, and the compositor
 *  copies the low 24 bits and ignores the top byte. An RV32 frame is
 *  therefore copied into the window with no channel conversion.
 * ============================================================ */
#ifndef MYSTIC_VLCMIN_H
#define MYSTIC_VLCMIN_H

#include <stdint.h>

/* Mixing this with the real VLC SDK would redefine libvlc_state_t and
 * the opaque typedefs. Refuse loudly instead of compiling something
 * whose second half is a different header's idea of the ABI. */
#if defined(VLC_LIBVLC_H) || defined(VLC_LIBVLC_MEDIA_H) || \
    defined(VLC_LIBVLC_MEDIA_PLAYER_H)
# error "vlcmin.h declares the libVLC ABI itself; do not include the VLC SDK headers too"
#endif

/* The soname libvlc 3.x installs and the major version its ABI is. */
#define VLC_MIN_SONAME   "libvlc.so.5"
#define VLC_MIN_ABI_MAJOR 3

/* libvlc.h: `typedef int64_t libvlc_time_t;` - milliseconds. */
typedef int64_t libvlc_time_t;

/* libvlc.h:77 and libvlc_media.h:48 - opaque, never dereferenced. */
typedef struct libvlc_instance_t     libvlc_instance_t;
typedef struct libvlc_media_t        libvlc_media_t;
typedef struct libvlc_media_player_t libvlc_media_player_t;

/* libvlc_media.h:90 - the enum is int-sized in the ABI. */
typedef enum libvlc_state_t {
    libvlc_NothingSpecial = 0,
    libvlc_Opening,
    libvlc_Buffering,
    libvlc_Playing,
    libvlc_Paused,
    libvlc_Stopped,
    libvlc_Ended,
    libvlc_Error
} libvlc_state_t;

/* libvlc_media_player.h:307-338 - the vmem callback types.
 *
 *   lock()   returns the private pointer that unlock()/display() get back
 *            and fills planes[0] with the frame surface;
 *   unlock() is called once a decoded picture has been copied into that
 *            surface;
 *   display() is called on the playback clock when the picture is due. */
typedef void *(*libvlc_video_lock_cb)(void *opaque, void **planes);
typedef void (*libvlc_video_unlock_cb)(void *opaque, void *picture,
                                       void *const *planes);
typedef void (*libvlc_video_display_cb)(void *opaque, void *picture);

/* ------------------------------------------------------------
 *  The whole ABI this player uses, in the order vlcplay.c binds it.
 *  X(symbol, struct field) - the symbol is resolved with dlsym and
 *  a missing one is reported by its name, never called.
 * ------------------------------------------------------------ */
#define VLC_MIN_FUNCTIONS(X)                        \
    X(libvlc_new,                     new_instance) \
    X(libvlc_release,                 release)      \
    X(libvlc_errmsg,                  errmsg)       \
    X(libvlc_get_version,             version)      \
    X(libvlc_media_new_path,          media_new_path) \
    X(libvlc_media_release,           media_release) \
    X(libvlc_media_player_new,        player_new)   \
    X(libvlc_media_player_release,    player_release) \
    X(libvlc_media_player_set_media,  player_set_media) \
    X(libvlc_media_player_play,       player_play)  \
    X(libvlc_media_player_stop,       player_stop)  \
    X(libvlc_media_player_set_rate,  player_set_rate) \
    X(libvlc_media_player_get_rate,  player_get_rate) \
    X(libvlc_media_player_is_playing, player_is_playing) \
    X(libvlc_media_player_set_pause,  player_set_pause) \
    X(libvlc_media_player_get_time,   player_get_time) \
    X(libvlc_media_player_set_time,   player_set_time) \
    X(libvlc_media_player_get_length, player_get_length) \
    X(libvlc_media_player_get_state,  player_get_state) \
    X(libvlc_audio_set_volume,        audio_set_volume) \
    X(libvlc_audio_get_volume,        audio_get_volume) \
    X(libvlc_video_get_size,          video_get_size) \
    X(libvlc_video_set_callbacks,     video_set_callbacks) \
    X(libvlc_video_set_format,        video_set_format)

/* ---- libvlc.h ------------------------------------------------ */

/* argc should be 0 and argv NULL for a normal embedder; the app passes
 * two options (--no-video-title-show and --avcodec-hw=none) because its
 * pixels are CPU memory, not a GPU surface. Returns NULL on failure,
 * with libvlc_errmsg(). */
libvlc_instance_t *libvlc_new(int argc, const char *const *argv);
void libvlc_release(libvlc_instance_t *p_instance);

/* Last error on this thread, or NULL. Valid until the next libVLC call
 * on the same thread; the app copies it immediately. */
const char *libvlc_errmsg(void);
const char *libvlc_get_version(void);

/* ---- libvlc_media.h ------------------------------------------ */

libvlc_media_t *libvlc_media_new_path(libvlc_instance_t *p_instance,
                                      const char *path);
void libvlc_media_release(libvlc_media_t *p_md);

/* ---- libvlc_media_player.h ----------------------------------- */

/* libvlc_media_player.h:177, :197, :215 */
libvlc_media_player_t *libvlc_media_player_new(libvlc_instance_t *p_libvlc_instance);
void libvlc_media_player_release(libvlc_media_player_t *p_mi);
void libvlc_media_player_set_media(libvlc_media_player_t *p_mi,
                                   libvlc_media_t *p_md);

/* :251, :275, :243, :260 */
int  libvlc_media_player_play(libvlc_media_player_t *p_mi);
int  libvlc_media_player_set_rate(libvlc_media_player_t *p_mi, float rate);
float libvlc_media_player_get_rate(libvlc_media_player_t *p_mi);
void libvlc_media_player_stop(libvlc_media_player_t *p_mi);
int  libvlc_media_player_is_playing(libvlc_media_player_t *p_mi);
void libvlc_media_player_set_pause(libvlc_media_player_t *mp, int do_pause);

/* :796, :805, :788, :933 - times are milliseconds */
libvlc_time_t libvlc_media_player_get_time(libvlc_media_player_t *p_mi);
void libvlc_media_player_set_time(libvlc_media_player_t *p_mi,
                                  libvlc_time_t i_time);
libvlc_time_t libvlc_media_player_get_length(libvlc_media_player_t *p_mi);
libvlc_state_t libvlc_media_player_get_state(libvlc_media_player_t *p_mi);

/* :1807 and :1819 - set returns 0 when libVLC accepted the value and -1
 * when there is no audio output; get returns the output's own volume, or
 * -1 when there is no output. The app shows the getter's answer, because
 * that is libVLC's answer. */
int libvlc_audio_set_volume(libvlc_media_player_t *p_mi, int i_volume);
int libvlc_audio_get_volume(libvlc_media_player_t *p_mi);

/* :1112 - the video's own size in pixels, from the media's track info
 * (so it answers as soon as the track is parsed); 0 on success. */
int libvlc_video_get_size(libvlc_media_player_t *p_mi, unsigned num,
                          unsigned *px, unsigned *py);

/* :418 and :441 - these two must be called together, before play, and
 * are mutually exclusive with the libvlc_video_set_format_callbacks()
 * (setup/cleanup) variant this app does not use. */
void libvlc_video_set_callbacks(libvlc_media_player_t *mp,
                                libvlc_video_lock_cb lock,
                                libvlc_video_unlock_cb unlock,
                                libvlc_video_display_cb display,
                                void *opaque);
void libvlc_video_set_format(libvlc_media_player_t *mp, const char *chroma,
                             unsigned width, unsigned height,
                             unsigned pitch);

#endif /* MYSTIC_VLCMIN_H */
