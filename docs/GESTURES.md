# Trackpad gestures

Mystic OS reads a multitouch touchpad straight from evdev, with no libinput in
between. This is what the desktop does with it, and where each number comes
from.

## What is parsed (`src/input.c`)

evdev protocol B, per contact slot:

| event | meaning |
|---|---|
| `EV_ABS ABS_MT_SLOT` | the slot the following values belong to |
| `EV_ABS ABS_MT_TRACKING_ID` | a new contact opens the slot, `-1` closes it |
| `EV_ABS ABS_MT_POSITION_X/Y` | that slot's position, in the device's own units |

A packet ends at `EV_SYN/SYN_REPORT`, and every gesture decision is made at the
end of a packet — never per event, because the second finger's position in the
same packet is still in flight when the first one is read.

The older reporting style has no slots. It is accepted too: the contact count
comes from `BTN_TOOL_FINGER`, `BTN_TOOL_DOUBLETAP`, `BTN_TOOL_TRIPLETAP`,
`BTN_TOOL_QUADTAP` and `BTN_TOOL_QUINTTAP` (the highest one down is the count),
with `BTN_TOUCH` saying a finger is on the pad. All of those are `EV_KEY` codes,
which is why they are consumed in `src/input.c` before the key branch: they used
to be queued as if the user had typed them. That style has a finger **count but
no positions**, so it can report `finger_n` and cannot make a gesture out of it;
such a pad still scrolls through the `REL_WHEEL` its driver emits, which
`src/input.c` has always read.

Positions are delivered as pixels. That is the convention the file already had
for an absolute touch position (`ABS_X` was used as a logical pixel), so a
gesture delta is a distance on screen at 1:1 and the desktop converts it with
its own font metrics.

## The contract (`Input` in `src/mystic.h`)

| field | meaning |
|---|---|
| `gesture` | `GESTURE_NONE`, `SCROLL`, `PINCH` or `SWIPE`, latched for the whole gesture |
| `gesture_fingers` | contacts it was made with |
| `gesture_dx/dy` | centroid travel this frame, px, y grows downwards; summed over a gesture this is the whole travel |
| `gesture_scale` | two-finger gap now / gap when the gesture started (1.0 = no pinch) |
| `gesture_release` | 1 on exactly one frame: the one the gesture ended. `gesture` still names it there |
| `gesture_id` | +1 each time a gesture latches; the desktop fires an action when it changes, so a held swipe cannot fire twice |
| `gesture_tap` / `gesture_tap_fingers` | a tap, with no travel, reported once |
| `finger_n`, `finger_dx[]`, `finger_dy[]` | contacts now, and each one's travel this frame |

A gesture is **latched**: it does not change kind until every finger lifts, so a
two-finger scroll cannot turn into a pinch halfway through. A finger landing or
lifting mid-run ends the run (one release edge) and starts a new one.

Nothing in the parser is named until the contacts have travelled past a
threshold, which is what keeps a tap from arriving as a swipe.

## The mapping (`src/main.c`)

| gesture | action |
|---|---|
| two-finger vertical scroll | scroll the window under the pointer — a terminal in text lines, a client/app in wheel notches (3 lines to a notch) |
| two-finger horizontal | consumed and delivered to nobody: `Input` has no horizontal wheel, `WM_WHEEL` carries one number and no app in this tree scrolls sideways. The axis is locked, so this travel can never also move the list up and down |
| pinch | the global text scale, live, 0.8..1.6 |
| three-finger up | launcher |
| three-finger down | show the desktop (minimise every window) |
| three-finger right | next window (the Alt+Tab order) |
| three-finger left | previous window |
| two-finger tap | right click at the pointer |
| three-finger tap | shortcuts panel (toggles; the same action as F1) |
| four-finger tap | nothing, deliberately: a resting hand must not minimise the desktop |

Scrolling is natural: the content follows the finger, so a delta of +40 px
(downwards) is 40 px of content moving down, which is the same direction the
wheel reports for "back in history".

Gestures do nothing while the launcher, the desktop menu or the shortcuts panel
is open, except that a three-finger tap closes the panel it opened.

## Thresholds and gains (policy, not measurement)

All of them are named constants at the top of the gesture block that uses them.

`src/input.c`:

| constant | value | protects |
|---|---|---|
| `MT_TAP_SLOP` | 8 px | how far a contact may wander and still be a tap |
| `MT_TAP_MS` | 350 ms | tap length (the same window the double-click test uses) |
| `MT_SCROLL_MIN` | 4 px | travel before a scroll latches |
| `MT_SWIPE_MIN` | 30 px | travel before a three-finger swipe latches |
| `MT_PINCH_MIN` / `MT_PINCH_RATIO` | 4 px / 4 % | gap change before a pinch latches |
| `MT_AXIS_LOCK` | 1.15 | one axis must be this many times the other to be "dominant" |

`src/main.c`:

| constant | value | does |
|---|---|---|
| `GEST_LINES_PER_NOTCH` | 3 | the wheel's unit, exactly as `input.wheel * 3` always meant |
| `GEST_ACCEL_PX` | 8 px/frame | below this the content moves exactly with the finger |
| `GEST_ACCEL_MAX` | 1.5 | ... and a flick adds at most 50 % (reached at 24 px in one frame) |
| `GEST_PINCH_DEAD` | 3 % | dead zone at the start of a pinch |

The pinch dead zone is *subtracted*, not swallowed: past 3 % the text follows
the fingers one to one with no step at the edge.

## Text scale

`ui.conf` stores `baseline` (UI_BASE, read back from the file rather than
repeated) and `scale` relative to it. A pinch multiplies the number the file
holds and clamps it to the same 0.8..1.6 the Settings app and `Ctrl+=` /
`Ctrl+-` use. The theme's own `font_scale` is applied by the same
`apply_text_scale()` as always, so a pinch starts from the *user's* number and
cannot compound a theme factor.

The file is written **once**, when the gesture ends, through the same writer
`Ctrl+=` uses (`baseline = ... / scale = ...`), and then duplicated to
`/root/mystic-ui.conf` by `persist_settings()` like every other setting. While
the fingers are down the scale is only applied in memory; the once-a-second
`ui.conf` reload is told to leave the live value alone.

## No touchpad? Nothing changes

Every field above is written only by the multitouch parser. On a machine whose
pointing device is a plain mouse, or the PS/2 mouse QEMU emulates, they stay
zero, `gestures_apply()` is never called, and the pointer/wheel path is exactly
what it was. `input_open()` logs `no touchpad` once so the machine's
capability is on the record.

## Verification

`.work/gestures/` (host harnesses, not shipped):

* `harness.c` `#include`s the shipping `src/input.c` and feeds real
  `<linux/input.h>` packets through `handle_event()`: 108 checks covering a
  40 px two-finger scroll, a 100→160 px pinch, three-finger swipes up and
  left, two-finger taps (together and finger-by-finger), 3 px of jitter, the
  plain-mouse fallback, `BTN_TOOL_*` consumption and per-finger deltas.
* `map_harness.c` extracts the `MYSTIC_GESTURES_BEGIN/END` section of
  `src/main.c` and checks the pixel→line arithmetic, the acceleration curve,
  the pinch curve and what each swipe direction and tap maps to.

What cannot be tested without hardware: that a particular touchpad reports
protocol B at all, its resolution and therefore how many device units a
millimetre of finger travel produces, whether its firmware also emits
`REL_X/REL_Y` (which would move the pointer during a scroll), and the feel of
the gains. Those are the numbers a real machine would tune.
