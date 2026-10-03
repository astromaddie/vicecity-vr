# sfvr

A small C99 library shared by the Steam Frame VR game ports: Halo CE (C), F-Zero X
and Star Fox (C++). It holds the logic every port must do identically, so the
user moves between games without relearning anything. Daggerfall (Unity) mirrors
it in C#.

No OpenXR, GL or SDL dependency; no `malloc`; no mutable globals (const tables
only); only `<math.h>`, `<string.h>`, `<stdio.h>`, `<stddef.h>`. No assumptions
about pointer size (usable in an ILP32 AArch64 guest). Compiles warning-free with
`-std=c99 -Wall -Wextra -Werror -pedantic` under clang and gcc, and every header
is usable from C++ (`extern "C"` guards).

Units: seconds (`double` for absolute time, `float` for dt), metres, degrees
unless a name ends `_rad`. Everything is prefixed `sfvr_` / `SFVR_`.

## The standard it implements

`~/.claude/skills/steam-frame-vr-port/references/standard.md`

| Section | Module |
| --- | --- |
| 1 System layer (L View short/hold) | `sfvr_view.h` |
| 5 Comfort turn and roomscale | `sfvr_turn.h`, `sfvr_room.h` |
| 6 Haptics vocabulary | `sfvr_haptics.h` |
| 7 Settings key registry | `sfvr_settings.h` |
| 8 `[vr-perf]` line | `sfvr_perf.h` |

Not here (each port owns it): the OpenXR session, settings file formats and IO,
the display-time scheduler.

## API summary

Include `sfvr/sfvr.h` for everything (also `SFVR_VERSION_MAJOR/MINOR/PATCH` =
0.1.0), or one module header. Every header documents its rules in a top comment.

```c
/* sfvr_view.h: bitmask of SFVR_VIEW_SHORT_PRESS | RECENTRE | RECALIBRATE_HEIGHT */
void     sfvr_view_config_default(sfvr_view_config *c);   /* 1.0 s, 3.0 s */
void     sfvr_view_reset(sfvr_view_button *b);            /* focus loss: cancel, no events */
unsigned sfvr_view_update(sfvr_view_button *b, const sfvr_view_config *c, int pressed, double now_s);

/* sfvr_turn.h: yaw delta in degrees, positive = turn right (clockwise from above) */
void  sfvr_turn_config_default(sfvr_turn_config *c);
void  sfvr_turn_reset(sfvr_turn_state *s);
float sfvr_turn_update(sfvr_turn_state *s, const sfvr_turn_config *c, float stick_x, float dt_s);
int   sfvr_turn_normalise_snap_angle(float degrees);      /* 15 30 45 60 90 */
float sfvr_turn_normalise_smooth_speed(float dps);        /* 30..360 */
float sfvr_turn_deadzoned(float axis, float deadzone);

/* sfvr_haptics.h: hand 0 = left, 1 = right, SFVR_HAND_BOTH = 2 */
sfvr_haptic_pulse sfvr_haptic_default(sfvr_haptic_event_id event);
void  sfvr_haptic_queue_clear(sfvr_haptic_queue *q);
void  sfvr_haptic_submit(sfvr_haptic_queue *q, int hand, float amplitude, float seconds);
void  sfvr_haptic_event(sfvr_haptic_queue *q, int hand, sfvr_haptic_event_id event);
int   sfvr_haptic_flush(sfvr_haptic_queue *q, float strength, sfvr_haptic_callback cb, void *user);
float sfvr_rumble_to_haptic(unsigned low, unsigned high); /* max/65535; pulse 0.040 s */
void  sfvr_haptic_rumble(sfvr_haptic_queue *q, int hand, unsigned low, unsigned high);

/* sfvr_room.h: tracking-space x,z metres */
void sfvr_room_init(sfvr_room_state *s);
void sfvr_room_hold(sfvr_room_state *s);
void sfvr_room_recentre(sfvr_room_state *s, const float head_xz[2]);
int  sfvr_room_step(sfvr_room_state *s, const sfvr_room_config *c, const float head_xz[2], int tracking_valid, float out_step_xz[2]);
void sfvr_room_moved(sfvr_room_state *s, const float head_xz[2]);
void sfvr_room_lean(const sfvr_room_state *s, const sfvr_room_config *c, const float head_xyz[3], float blend_t, float out_offset_xyz[3]);
void sfvr_room_lean_fixed(const float origin_xyz[3], const float head_xyz[3], float lean_limit, float out_offset_xyz[3]);

/* sfvr_perf.h: "[vr-perf] fps=71.9 missed=3 cpu=7.31ms logic=1.20 render=5.02 eye=2.21/2.19ms gpu=8.40ms" */
void sfvr_perf_init(sfvr_perf *p, double window_s, const char *const *stage_names, int stage_count);
void sfvr_perf_add_frame(sfvr_perf *p, double now_s, int missed, const float *stage_ms, const float eye_ms[2], float gpu_ms);
int  sfvr_perf_poll(sfvr_perf *p, double now_s, char *buf, size_t n);

/* sfvr_settings.h: registry only, no file IO */
size_t sfvr_settings_count(void);
const sfvr_setting_key *sfvr_settings_at(size_t i);
const sfvr_setting_key *sfvr_settings_find(const char *name);
float sfvr_settings_clamp_float(const sfvr_setting_key *k, float v);
int   sfvr_settings_clamp_int(const sfvr_setting_key *k, int v);
int   sfvr_settings_parse_enum(const sfvr_setting_key *k, const char *text);
int   sfvr_settings_parse_bool(const char *text, int fallback);
int   sfvr_settings_env_name(const char *prefix, const char *key, char *buf, size_t n); /* HALO, snap_angle -> HALO_VR_SNAP_ANGLE */
```

## Build and test

```
make test            # clang strict C99 + each header as a C++17 TU + all tests
make test CC=gcc     # the C parts with gcc
make sanitize        # tests under ASan + UBSan
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

`make test` also compiles every public header on its own as C and as C++
(`clang++ -std=c++17 -Wall -Wextra -Werror`), and links a small C++ program
against the library.

## Vendoring into a port

sfvr is vendored by copying, not as a submodule. Its canonical source is this repo.

1. Copy `include/` and `src/` to `<port>/third_party/sfvr/` (so
   `third_party/sfvr/include/sfvr/*.h` and `third_party/sfvr/src/*.c`).
2. Add `<port>/third_party/sfvr/VERSION` containing the sfvr git SHA
   (`git rev-parse HEAD` here) as its only line.
3. Add `third_party/sfvr/include` to the include path and the `src/*.c` files to
   the build (they need only `-lm`).
4. **Never edit the vendored copy.** Change it here, add a test, then re-vendor
   (copy again and update `VERSION`).

```
sh -c 'dst=<port>/third_party/sfvr; rm -rf "$dst" && mkdir -p "$dst" &&
       cp -R include src "$dst"/ && git rev-parse HEAD > "$dst/VERSION"'
```
