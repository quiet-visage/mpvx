# Mini Player — UI, Motion & Technical Implementation Specification

> **Document Status:** `IMPLEMENTED (v1: Phase 0 + Phase 1; v2: Drag & Four-Corner Snapping, §12)`; Phase 2 verification pending (Design, Motion, Performance & Rendering Specification)  
> **Related Specs:** [Video Picker UI Spec](video-picker-ui-spec.md) · [Main Media Controls UI Spec](main-media-controls-ui-spec.md)  
> **Target Toolchain & Dependencies:** C++26, Slint `1.19.0` ([cmake/Dependencies.cmake](../cmake/Dependencies.cmake)), `libmpv` 2.5 software render API (`MPV_RENDER_API_TYPE_SW`, [src/renderer.cpp](../src/renderer.cpp))

This specification defines the **Mini Player** for **mpvx**: when the user opens the Video Picker while media is playing, playback **continues** in a floating, rounded video tile anchored to the bottom-right corner instead of pausing. The full-bleed video **morphs** into the tile (and back) with a single continuous, GPU-scaled animation.

It supersedes **Player → Picker** step 2 of [Video Picker UI Spec §5.2](video-picker-ui-spec.md) ("Playback pauses").

---

## 1. Goals & Non-Goals

### 1.1 Goals
1. **Uninterrupted playback while browsing.** Opening the library never pauses video or audio.
2. **100% smooth morph.** The video shrinks to, and grows back from, the bottom-right tile with no dropped frames, no visible letterbox jumps and no content flash, at the display's refresh rate.
3. **Cheaper than the full player.** The mini player must use *less* CPU and memory bandwidth than full-window playback (see §2 for numbers).
4. **One-gesture return.** Clicking the tile, the Esc key or the top-bar "Return to Player" button restores the full player exactly where playback is.
5. **Never lose the session by accident.** Closing the tile pauses and minimizes it to a pill; it never stops or unloads the file (§3.5).
6. **Global to picker mode.** The mini player survives all picker navigation: folders, breadcrumbs, sidebar views, search and sort (§3.7).
7. **No new threads, no new locks on the GUI thread.** The feature reuses the existing render thread and `invoke_from_event_loop` frame hand-off (see [Video Picker UI Spec §9](video-picker-ui-spec.md)).

### 1.2 Non-Goals (v1)
- Drag-to-reposition / corner snapping (v2, §12).
- A separate always-on-top OS window (desktop PiP). The mini player lives inside the mpvx window.
- Switching to the mpv OpenGL render API. It is the only way below the decode floor (§2.3) but is a separate project.
- Seek bar, volume, track menus inside the tile. The tile is deliberately minimal; full controls are one click away.
- An affordance that stops playback and dismisses the mini player entirely. The pill persists until another file is loaded or playback goes idle (§3.5).
- Persisting mini-player settings across launches. The 30 fps cap is session-only, like every other Settings popover option today (§3.6).

### 1.3 Resolved Decisions (2026-11-20)

| # | Question | Decision | Where specified |
| :--- | :--- | :--- | :--- |
| 1 | What does the tile's close button do? | **Pauses and minimizes** the tile to a pill. Does not stop or unload. | §3.5, §4.4, §7.4 |
| 2 | Does the mini player persist across picker navigation? | **Yes.** It is global to picker mode. | §3.7 |
| 3 | 30 fps cap: default on battery, or opt-in? | **Opt-in** setting, off by default, no automatic battery switching. | §3.6, §7.6 |

---

## 2. Performance Evidence

### 2.1 Method
A standalone benchmark reproduces the exact `VideoRenderer::render_frame()` path from [src/renderer.cpp](../src/renderer.cpp): `rgb0` software render, a freshly value-initialised buffer per frame (as `SharedPixelBuffer(w, h)` does), and the per-pixel alpha fix-up loop. It plays a 12 s 1080p60 VP9 clip with `hwdec=auto-safe` (resolved to `vaapi-copy`), discards 1 s of warm-up and measures 6 s.

- **Machine:** Intel i5-1135G7 (Tiger Lake, 8 threads, Iris Xe), Fedora 45, libmpv 2.5 from `.deps/libmpv`, `-O2`.
- **Recorded:** 2026-11-20.

### 2.2 Results

| Render target | Render thread ms/frame (mean / p95) | Alloc + alpha share | Process CPU | Pixel upload to GPU |
| :--- | :--- | :--- | :--- | :--- |
| `1920×1080` (today, fullscreen) | 8.0 / 11.6 | 2.6 ms | 162–192% | 498 MB/s |
| `1280×720` (today, default window) | 6.6 / 9.9 | 1.4 ms | 174% | 222 MB/s |
| `480×270` | 3.6 / 4.9 | 0.15 ms | 98% | 31 MB/s |
| **`384×216`, reused buffers** | **3.4–4.0 / 4.6–6.1** | **0.07 ms** | **83–95%** | **20 MB/s** |

Process CPU varies by about ±30 points between runs (note the 720p/1080p inversion), so read it as a trend.

Supplementary experiments:

| Experiment | Result | Conclusion |
| :--- | :--- | :--- |
| `sw-fast=yes` at `384×216` | 3.8 ms, 92% CPU | No meaningful gain. |
| GPU downscale before copy-back (`vf=scale_vaapi=w=384:h=216`, `hwdec=vaapi`) | Render 1.5 ms, but mpv fell back to `hwdec=no`; CPU 90–93% | Not viable with the SW render API. |
| `rgba` output format (to skip the alpha loop) | Render 4.05 ms vs 3.43 ms | Slower; mpv converts internally. Keep `rgb0`. |
| Alpha loop, byte store vs `uint32 \|= 0xFF000000`, hot cache | 0.35 ms vs 0.33 ms | Loop is memory-bound. In the real pipeline it costs 1.9 ms at 1080p only because the 8 MB frame is cold. Only fewer pixels help. |

### 2.3 Conclusions (Normative)
1. **The mini player MUST shrink the mpv render target.** Scaling the existing full-size `Image` in Slint keeps every full-size cost (8 ms/frame, 500 MB/s upload) for a 384 px tile.
2. **With the target shrunk, the mini player costs about half the CPU of full-window playback** and about 25× less upload bandwidth. Continuing playback is cheaper than today's normal playback.
3. **The decode floor is about 85–95% CPU**, independent of target size: hardware decode plus 1080p copy-back to system memory, and mpv's scaler reading the full source frame. Nothing in this spec can go below it.
4. **Render-target changes MUST NOT happen during an animation.** Each change costs a full re-render at the new size and alters scaler output; doing it per animation frame would be both expensive and visibly jittery.

### 2.4 Reproduction
The benchmark sources (`bench.cpp`, `alpha.cpp`) are listed in §11.3 and should be committed under `bench/` (not built by default). Usage: `bench <file> <w> <h> <seconds> <rgb0|rgba> <pool 0|1> [vf] [hwdec] [sw-fast]`.

---

## 3. UX Specification

### 3.1 Geometry
All values are logical pixels and bound reactively to the window size, so window resizes, maximize and fullscreen re-layout the tile automatically.

| Property | Value |
| :--- | :--- |
| Tile width `mini-w` | `clamp(window.width × 0.26, 280px, 480px)` |
| Tile height `mini-h` | `mini-w / video-aspect` (`video-aspect` clamped to `[0.5, 3.0]`; `16/9` until known) |
| Right / bottom margin | `16px` |
| Corner radius | `Theme.card-radius` (`10px`) |
| Border | `1px Theme.card-border` (`#ffffff14`), `Theme.card-border-hover` on hover |
| Shadow | `drop-shadow-blur: 24px`, `drop-shadow-color: #00000099`, `drop-shadow-offset-y: 8px`; opacity driven by `mini-shadow` (§4.5) |

Portrait video (`video-aspect < 1`): width is computed from a height cap instead, `mini-h = clamp(window.height × 0.40, 220px, 420px)`, `mini-w = mini-h × video-aspect`.

### 3.2 Tile Anatomy

```
┌──────────────────────────────────────────┐
│                               [⤢]  [✕]  │  ← top-right: expand, close = pause + minimize (hover only)
│                                          │
│                  ( ▶ / ❚❚ )              │  ← centre: play/pause, 44px circle (hover only)
│                                          │
│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░░░░░░░░░░░░░░░░░│  ← 3px progress line, always visible
└──────────────────────────────────────────┘
```

- **Hover scrim:** `#00000066` full-tile overlay, `150ms ease-out` fade, shown on hover only.
- **Icons:** existing Lucide assets (`play.svg`, `pause.svg`, `maximize-2.svg`, `x.svg`), `Theme.icon-primary`, via `IconButton` from [ui/components/icon-button.slint](../ui/components/icon-button.slint).
- **Progress line:** `Theme.progress-fill` over `Theme.progress-track`, bound to `root.progress`. It is the only always-visible chrome.
- **Buffering:** the existing arc spinner from [ui/components/video-view.slint](../ui/components/video-view.slint) renders at `28px` when `mini-active`.
- **Accessibility:** tile `accessible-role: button`, `accessible-label: "Mini player: " + media-title + ". Activate to expand."`. The `✕` button's label is `"Pause and minimize"`, because it does not close the media.

### 3.3 Interactions

| Input | Target | Action |
| :--- | :--- | :--- |
| Click | Tile body | Expand to full player (§4.2). |
| Click | Centre play/pause | Toggle pause. Does **not** expand. |
| Click | `⤢` | Expand. |
| Click | `✕` | **Pause and minimize**: forced progress save, `Player::pause()`, tile collapses to the pill (§3.5, §4.4). The file stays loaded. |
| Double-click | Tile body | Same as single click. No fullscreen toggle in mini mode. The first click expands; the second lands on the full player, which ignores clicks on the video for `Theme.mini-expand-click-guard` (`500ms`) after leaving mini mode (§6.4), so it neither toggles pause nor fullscreen. |
| Hover | Tile | Show scrim and buttons; HUD timer does not apply. |
| `Esc` | Picker, search empty, tile **or** pill | Expand to the full player (existing behaviour, [ui/app-window.slint](../ui/app-window.slint) key-scope). From the pill, playback stays paused. |
| `Space` | Picker, search field **not** focused, tile | Toggle pause. |
| `Space` | Picker, search field **not** focused, pill | Resume playback and restore the tile. |
| `Ctrl+W` | Picker, tile | Pause and minimize (same as `✕`). |
| Click a video card | Picker | Load it into the same player and expand (existing `picker-video-clicked` flow). From the tile, the morph starts at the tile; from the pill, it runs pill → full window in one continuous motion, the same as `⤢` on the pill (§6.3). |

### 3.4 Picker Layout Adjustments
- While the tile is showing, the picker main pane adds `padding-bottom: mini-h + 32px` so the last grid row and shelves can be scrolled clear of the tile. While the pill is showing, the inset is `40px + 32px`. The padding changes once at the start of each transition, never per frame.
- The sidebar is not padded; the tile sits over the main pane only. (v2 top corners instead push the whole picker, sidebar included, below the tile: `top-clearance`, §12.1.)
- The top bar shows the picker layout (search, sort). Its existing **Return to Player** button stays and expands the tile.

### 3.5 Minimized State (Pill)
Closing the tile (`✕` or `Ctrl+W`) pauses playback and collapses the tile into a compact pill in the same corner. The pill keeps the session one click away without covering picker content.

```
                                   ╭───────────────────────────────╮
                                   │ (▶)  Episode 04 — The Fal…  ⤢ │   ← 40px tall, bottom-right
                                   ╰───────────────────────────────╯
```

| Property | Value |
| :--- | :--- |
| Height | `40px` |
| Width | `clamp(title.preferred-width + 96px, 200px, 320px)` |
| Corner radius | `20px` |
| Surface | `Theme.popover-bg` (`#12141ae6`), `1px Theme.popover-border` |
| Position | Right and bottom margins `Theme.mini-margin` (`16px`), same corner as the tile |
| Contents | Left: `play.svg` icon button (`28px` circle). Centre: `media-title`, `13px`, `Theme.text-primary`, elided. Right: `maximize-2.svg` icon button. |

| Input | Pill target | Action |
| :--- | :--- | :--- |
| Click | `▶` | Resume playback and restore the tile (§4.4 reverse). |
| Click | Body / title | Restore the tile, **still paused**, showing the paused frame. |
| Click | `⤢` | Expand straight to the full player, still paused (§4.2). |

**Invariant:** pill ⇒ paused. If playback resumes by any route while the pill is showing (`Space`, a media key, mpv IPC), the tile is restored. The tile itself may be paused or playing.

**Accessibility:** pill `accessible-role: button`, `accessible-label: "Mini player minimized: " + media-title + ". Activate to restore."`.

### 3.6 Optional 30 fps Cap (Opt-In)
- **Off by default.** No automatic switching on battery or power profile.
- **Control:** a new **MINI PLAYER** section in the existing Settings popover ([ui/components/popovers.slint](../ui/components/popovers.slint), `ActivePopover.Settings`) with two `SettingsPill`s, `Full rate` (default) and `30 fps`, matching the existing PLAYBACK SPEED, HWDEC and ASPECT RATIO sections.
- **Scope:** affects only the playing tile. It has no effect in the full player (which always runs at full rate), and the pill is paused anyway.
- **Lifetime:** session-only, like the other Settings popover options today (none are persisted). Persistence is future work (§12).
- **Mechanism:** §7.6.

### 3.7 Persistence Across Picker Navigation
The mini player (tile or pill) is global to picker mode. These actions **never** change `mini-state` or touch the render target:
- sidebar navigation (Home, Continue Watching, Recent, folders), breadcrumbs, `Backspace` (navigate up), opening an external folder;
- search input and clearing it (`Esc` clears search **first**, so a non-empty search consumes the first `Esc` and the second expands);
- sort mode changes, grid column changes and sidebar toggle (`Ctrl+B`);
- library refreshes and thumbnail updates.

Because the tile is a sibling of `VideoPicker` (not a child), picker model swaps and list rebuilds cannot destroy or re-create it. Only the following leave mini mode: expand (§4.2), loading another file (§3.3), and playback going idle or failing (§8).

---

## 4. Transition Choreography

### 4.1 Shrink (Player → Picker with Mini Player)

| t | Event |
| :--- | :--- |
| 0 ms | `return-to-picker-clicked`. C++ records progress (forced flush) and **does not pause**. Sets `view-mode = Picker`, `mini-state = Tile`. |
| 0 ms | Picker becomes visible at **full opacity, beneath** the video (no crossfade; see §4.5). Controls bar hidden; top bar switches to picker layout over its existing `220ms` fade. |
| 0–300 ms | `mini-t` animates `0 → 1`, `300ms`, `ease-out-expo`. The video rect interpolates from the window rect to the tile rect; corner radius `0 → 10px`. The render target stays at **full size**; the GPU downscales. |
| 300 ms | Animation settles (§5.3). C++ switches the render target to the tile's physical size, applies the mini mpv profile (§7.3), and sets `mini-shadow = 1` (`160ms` fade). |

### 4.2 Expand (Mini Player → Player)

| t | Event |
| :--- | :--- |
| 0 ms | Expand requested. C++ **first** sets the render target to full size and forces a redraw (works while paused, §7.1). Restores the full mpv profile. Sets `mini-shadow = 0` instantly. |
| 0 ms | Sets `mini-state = Hidden` (from `Tile` or `Pill`), `view-mode = Player`, resets the HUD timer. |
| 0–260 ms | `mini-t` animates `1 → 0`, `260ms`, `ease-out-expo`. Until the first full-size frame arrives (one frame period, ≤ 17 ms at 60 fps), the tile-size frame is upscaled while the rect is still near tile size, which is visually indistinguishable. |
| 260 ms | Picker hidden (`visible: false`) to skip its layout and hit-testing during playback. |

### 4.3 Interruption & Reversal
- Slint animations retarget from the **current** value, so expanding mid-shrink (or the reverse) produces one continuous motion with no jump. Durations restart from the current position.
- Render-target timing is guarded by a generation counter (§5.3): a shrink that never settled never shrinks the target, and a stale settle timer can never fire after an expand.

### 4.4 Minimize (Tile ↔ Pill)
Collapsing needs no render-target change and no new video frames, because playback is paused first. It is a pure GPU transform of the last frame plus a small pill fade.

**Collapse (tile → pill), `220ms`, `ease-in-out-cubic`:**

| t | Event |
| :--- | :--- |
| 0 ms | `✕` / `Ctrl+W`. C++ forces a progress save, calls `Player::pause()`, sets `mini-shadow = 0` instantly, then sets `mini-state = Pill`. Picker inset switches to the pill value. |
| 0–220 ms | `collapse-t` animates `0 → 1`. The video rect interpolates from the tile rect to the pill rect; corner radius `10px → 20px`. Tile chrome (`MiniOverlay`) is hidden at `collapse-t > 0`. |
| 88–220 ms | Pill surface fades in on top (`opacity = clamp((collapse-t − 0.4) / 0.6, 0, 1)`), covering the shrinking video. |
| 220 ms | `VideoView` becomes `visible: false`. The last frame stays in the `video-frame` property, so restoring shows it immediately. |

**Restore (pill → tile), `220ms`, `ease-in-out-cubic`:** the exact reverse. `VideoView` becomes visible at `collapse-t = 1`, the pill fades out over the first 60%, and the rect grows back to the tile. `mini-shadow` fades in after settle (`160ms`). If restore was triggered by `▶` or `Space`, C++ calls `Player::play()` at t = 0; frames arrive at tile size because the render target never left `Mini`.

**Why this stays smooth:** the moving element is the same persistent `VideoView` (§4.5 rule 1); no frames are produced while collapsing; the only opacity animation is on the pill (≤ `320×40` px), which is cheap even if Slint composes it through a layer; the shadow is off during motion (§4.5 rule 3).

### 4.5 Motion Rules (Normative)
1. **Exactly one element moves:** the single persistent `VideoView`. It MUST NOT be recreated (no `if` swap) and MUST NOT be reparented.
2. **No opacity on the picker during the morph.** Fading an element that has children may make Slint compose it through an offscreen layer every frame. The picker is revealed by the shrinking rect instead. The existing `picker-opacity` crossfade remains only for the no-video case.
3. **No blurred shadow while geometry animates.** The shadow fades in only after settle (`mini-shadow`) and drops to 0 at the start of expand. A shadow whose size changes every frame is likely re-blurred every frame. The one exception is v2 drag and corner snap (§12.2): the tile keeps its size and only translates, and the shadow keeps a fixed `24px` radius, so it never changes size. Any morph that resizes the tile (shrink, expand, collapse) still runs without it.
4. **No layout-dependent children inside the moving rect.** `VideoView` children use absolute or percentage geometry only, so each animation frame is a pure transform.
5. **Render target constant during any animation** (§2.3 rule 4).
6. **Letterboxing is done by Slint, not mpv** (§7.2), so the frame's aspect ratio equals the tile's aspect ratio and no bars appear or vanish at settle.

---

## 5. State Model

### 5.1 UI State (`MainWindow`)

```slint
// ui/theme.slint
export enum MiniState { Hidden, Tile, Pill }
```

| `view-mode` | `mini-state` | Playback | Meaning |
| :--- | :--- | :--- | :--- |
| `Player` | `Hidden` | any | Full player (unchanged behaviour). |
| `Picker` | `Hidden` | idle / no video | Plain picker: nothing loaded, audio-only, or playback went idle. |
| `Picker` | `Tile` | playing or paused | Picker with the mini player tile. |
| `Picker` | `Pill` | **paused** | Picker with the minimized mini player (§3.5). |

`mini-active` is derived: `mini-state != Hidden`.

**Invalid combinations**, prevented by assigning `view-mode` and `mini-state` together in C++:
- `Player` with `Tile` or `Pill`.
- `Pill` while playing. Any transition to playing while in `Pill` restores `Tile` (§3.5 invariant), enforced in the `on_state_changed` handler.

**Transitions:**

| From | Trigger | To |
| :--- | :--- | :--- |
| `Player` / `Hidden` | Return to picker, video present and not idle | `Picker` / `Tile` (§4.1) |
| `Player` / `Hidden` | Return to picker, no video or idle | `Picker` / `Hidden` (existing crossfade) |
| `Picker` / `Tile` | `✕`, `Ctrl+W` | `Picker` / `Pill` (pause first, §4.4) |
| `Picker` / `Pill` | Pill body click | `Picker` / `Tile`, still paused |
| `Picker` / `Pill` | `▶`, `Space`, any external resume | `Picker` / `Tile`, playing |
| `Picker` / `Tile` or `Pill` | Tile click, `⤢`, `Esc`, Return to Player, card click | `Player` / `Hidden` (§4.2) |
| `Picker` / `Tile` or `Pill` | Playback idle (mpv `idle-active`) or error | `Picker` / `Hidden` (fade out, `160ms`) |
| `Picker` / `Tile` or `Pill` | Playlist advance, `loop-playlist` wrap | unchanged (`Idle` between entries is not idleness, §8) |
| `Picker` / any | Picker navigation, search, sort (§3.7) | unchanged |

### 5.2 Render-Target State (C++, GUI thread)

```
            shrink requested                   settle timer (gen matches)
  ┌──────┐ ─────────────────▶ ┌────────────┐ ─────────────────────────▶ ┌──────┐
  │ Full │                    │ FullToMini │                            │ Mini │
  └──────┘ ◀───────────────── └────────────┘                            └──────┘
      ▲      expand requested         (gen++)                               │
      │      (target already full)                                         │
      └────────────────────── expand requested: set Full + force redraw ◀──┘
                                       (gen++)
```

```cpp
enum class RenderTargetMode : uint8_t { Full, FullToMini, Mini };
```

`Tile ↔ Pill` transitions do **not** change `RenderTargetMode`: it stays `Mini`, because the pill is paused and the restored tile needs the same target size.

### 5.3 Settle Detection
Slint `changed` handlers on an animated property would fire per tick, so settle is detected in C++:

```cpp
const uint64_t gen = ++mini_generation_;
slint::Timer::single_shot(std::chrono::milliseconds(300 + 40), [=, this] {
    if (gen != mini_generation_ || mode_ != RenderTargetMode::FullToMini) return;
    mode_ = RenderTargetMode::Mini;
    renderer->set_target_size(mini_target_px());
    (void)player->set_mini_profile(true);
    if (auto ui = ui_weak.lock()) (*ui)->set_mini_shadow(1.0f);
});
```

The duration constant is shared with Slint through a `Theme` token (`mini-shrink-duration`) read by C++ via a generated global getter, so the two cannot drift.

---

## 6. Slint UI Architecture

### 6.1 Theme Tokens (`ui/theme.slint`)

| Token | Type | Value |
| :--- | :--- | :--- |
| `mini-shrink-duration` | `duration` | `300ms` |
| `mini-expand-duration` | `duration` | `260ms` |
| `mini-margin` | `length` | `16px` |
| `mini-min-width` / `mini-max-width` | `length` | `280px` / `480px` |
| `mini-width-ratio` | `float` | `0.26` |
| `mini-scrim` | `color` | `#00000066` |
| `mini-collapse-duration` | `duration` | `220ms` |
| `mini-pill-height` | `length` | `40px` |
| `mini-pill-min-width` / `mini-pill-max-width` | `length` | `200px` / `320px` |
| `mini-pill-radius` | `length` | `20px` |
| `mini-portrait-min-height` / `mini-portrait-max-height` | `length` | `220px` / `420px` |
| `mini-portrait-height-ratio` | `float` | `0.40` |
| `mini-close-fade-duration` | `duration` | `160ms` (idle dismissal, §14 note 3) |
| `mini-expand-click-guard` | `duration` | `500ms` (§3.3, §6.4) |

### 6.2 `MainWindow` Additions (`ui/app-window.slint`)

```slint
// --- Mini Player State ---
in-out property <MiniState> mini-state: MiniState.Hidden;
out property <bool> mini-active: root.mini-state != MiniState.Hidden;
in property <float> video-aspect: 16 / 9;          // from mpv dwidth/dheight
in property <float> mini-shadow: 0.0;               // 0..1, C++ sets after settle
out property <length> mini-w: ...;                  // §3.1 formula
out property <length> mini-h: ...;
out property <length> pill-w: ...;                  // §3.5 formula

// Player ↔ mini morph (§4.1, §4.2)
property <float> mini-t: root.mini-active ? 1.0 : 0.0;
animate mini-t {
    duration: root.mini-active ? Theme.mini-shrink-duration : Theme.mini-expand-duration;
    easing: ease-out-expo;
}

// Tile ↔ pill collapse (§4.4)
property <float> collapse-t: root.mini-state == MiniState.Pill ? 1.0 : 0.0;
animate collapse-t { duration: Theme.mini-collapse-duration; easing: ease-in-out-cubic; }

// Opt-in 30 fps cap (§3.6), session-only
in-out property <bool> mini-fps-cap: false;

callback mini-expand-clicked();
callback mini-minimize-clicked();                   // ✕ / Ctrl+W: pause + collapse to pill
callback mini-restore-clicked(bool /* resume */);   // pill body (false) or ▶ / Space (true)
callback mini-geometry-changed(int /* physical w */, int /* physical h */);
callback mini-fps-cap-changed(bool);
```

`mini-geometry-changed` fires from `changed mini-w` / `changed mini-h` (driven by window size and aspect, never animated), with values pre-multiplied by the window scale factor in C++.

### 6.3 Tree Order Change
`VideoView` moves **after** `VideoPicker` inside `window-shell`, so it draws above the picker. In player mode the picker is invisible, so ordering is irrelevant there. Final order:

1. `VideoPicker`
2. `VideoView` (geometry interpolated by `mini-t`) with `MiniOverlay` as a child
3. `MiniPill` (`ui/components/mini-pill.slint`), drawn above the video so it covers the collapsing tile
4. Popover backdrop
5. `TopBar`
6. `ControlsBar`
7. `PopoverLayer`
8. Border ring

```slint
video-view := VideoView {
    // Tile rect
    property <length> tile-x: window-shell.width  - root.mini-w - Theme.mini-margin;
    property <length> tile-y: window-shell.height - root.mini-h - Theme.mini-margin;
    // Pill rect
    property <length> pill-x: window-shell.width  - root.pill-w - Theme.mini-margin;
    property <length> pill-y: window-shell.height - Theme.mini-pill-height - Theme.mini-margin;
    // Mini rect = tile rect interpolated toward the pill rect by collapse-t
    property <length> m-x: tile-x + (pill-x - tile-x) * root.collapse-t;
    property <length> m-y: tile-y + (pill-y - tile-y) * root.collapse-t;
    property <length> m-w: root.mini-w + (root.pill-w - root.mini-w) * root.collapse-t;
    property <length> m-h: root.mini-h + (Theme.mini-pill-height - root.mini-h) * root.collapse-t;

    // Full window rect interpolated toward the mini rect by mini-t
    x: m-x * root.mini-t;
    y: m-y * root.mini-t;
    width:  window-shell.width  + (m-w - window-shell.width)  * root.mini-t;
    height: window-shell.height + (m-h - window-shell.height) * root.mini-t;
    border-radius: (Theme.card-radius + (Theme.mini-pill-radius - Theme.card-radius) * root.collapse-t) * root.mini-t;

    visible: (root.view-mode == AppViewMode.Player || root.mini-active || root.mini-t > 0.001)
          && root.collapse-t < 0.999;
    mini-mode: root.mini-t > 0.5;
    suppress-clicks: expand-click-guard.running;   // §6.4
    // ...existing bindings
}

MiniPill {
    x: video-view.pill-x;
    y: video-view.pill-y;
    width: root.pill-w;
    // Drawn while it fades out over an expand (view-mode becomes Player), but not after the
    // idle close (still Picker, mini no longer active), where collapse-t also runs back to 0.
    visible: root.collapse-t > 0.001 && (root.mini-active || root.view-mode == AppViewMode.Player);
    opacity: clamp((root.collapse-t - 0.4) / 0.6, 0, 1);
    title: root.media-title;
    restore-clicked => { root.mini-restore-clicked(false); }
    play-clicked    => { root.mini-restore-clicked(true); }
    expand-clicked  => { root.mini-expand-clicked(); }
}
```

When the pill expands straight to the player (`⤢`, `Esc`), `collapse-t` and `mini-t` both animate to 0 at once. The combined interpolation runs pill → full window in one continuous motion, with the pill fading out over the first 60% of the collapse curve.

### 6.4 `VideoView` Changes (`ui/components/video-view.slint`)
- New `in property <bool> mini-mode`. When true:
  - single-click emits `expand-clicked` instead of `single-clicked`; double-click is ignored;
  - the idle placeholder is suppressed and the spinner uses `28px`;
  - the cursor is `pointer` and never hidden.
- New `in property <bool> suppress-clicks`. When true, `clicked` and `double-clicked` on the video are ignored. `MainWindow` binds it to an `expand-click-guard` `Timer` (`Theme.mini-expand-click-guard`, `500ms`) that starts when `mini-active` turns false with `view-mode == Player`. `mini-mode` drops within about 30 ms of an expand (`ease-out-expo`), so without the guard the second click of a double-click on the tile would reach the full player and toggle pause and fullscreen (§3.3). The guard does not change how the player looks, unlike keeping `mini-mode` on for longer would.
- `clip: true` with `border-radius` produces the rounded tile.
- New child `MiniOverlay` (`ui/components/mini-overlay.slint`): hover scrim, play/pause, expand, `✕` (emits `mini-minimize-clicked`), progress line. It is shown only when `mini-t ≥ 0.95 && collapse-t == 0`, so chrome appears only after the tile has settled and is never composited during either morph.
- New component `MiniPill` (`ui/components/mini-pill.slint`): §3.5 anatomy. Callbacks `restore-clicked`, `play-clicked`, `expand-clicked`; `in property <string> title`.

### 6.5 Picker Padding (`ui/components/picker/video-picker.slint`)
New `in property <length> bottom-inset`. `MainWindow` binds it to:

```slint
bottom-inset: root.mini-state == MiniState.Tile ? root.mini-h + 2 * Theme.mini-margin
            : root.mini-state == MiniState.Pill ? Theme.mini-pill-height + 2 * Theme.mini-margin
            : 0px;
```

The inset is not animated.

### 6.6 Keyboard (`key-scope` in `ui/app-window.slint`)
Picker branch additions, evaluated before `return reject` and after the existing search-clearing `Esc` rule (§3.7):
- `Space`, when `!top-bar.search-focused`: in `Tile` → `root.play-pause-clicked()`; in `Pill` → `root.mini-restore-clicked(true)`.
- `Ctrl+W`, in `Tile` → `root.mini-minimize-clicked()`.
- `Esc` already calls `return-to-player-clicked()` when `has-video`; C++ routes that through the expand path from either `Tile` or `Pill`.

### 6.7 Settings Popover (`ui/components/popovers.slint`)
Add a **MINI PLAYER** section after ASPECT RATIO in the `ActivePopover.Settings` block, using the existing `SettingsPill`:

```slint
Text { text: "MINI PLAYER"; /* same style as "ASPECT RATIO" */ }
HorizontalLayout {
    for opt[i] in ["Full rate", "30 fps"] : SettingsPill {
        text: opt;
        active: (i == 1) == root.mini-fps-cap;
        clicked => { root.mini-fps-cap = (i == 1); root.mini-fps-cap-changed(root.mini-fps-cap); }
    }
}
```

`PopoverLayer` gains `in-out property <bool> mini-fps-cap` (two-way bound to `MainWindow.mini-fps-cap`) and `callback mini-fps-cap-changed(bool)`.

---

## 7. C++ Backend Changes

### 7.1 `VideoRenderer` (`include/mpvx/renderer.hpp`, `src/renderer.cpp`)

| Change | Reason |
| :--- | :--- |
| `void set_target_size(int w, int h)` replaces `resize()`; takes **physical** pixels and sets `force_redraw_` before notifying. | Today `render_frame()` returns early unless mpv reports `MPV_RENDER_UPDATE_FRAME` ([src/renderer.cpp](../src/renderer.cpp)), so a resize while paused is never re-rendered. Expanding a paused tile would otherwise leave the low-res frame up. |
| `void request_redraw() noexcept`: sets `force_redraw_` and notifies. | Same; used after profile changes. |
| `render_frame()` renders when `(flags & MPV_RENDER_UPDATE_FRAME) \|\| force_redraw_.exchange(false)`. | — |
| 3-slot `SharedPixelBuffer` ring, reallocated only on size change. | Removes the per-frame zeroed 8 MB allocation (0.72 ms at 1080p, §2.2). Safe by construction: `SharedVector::begin()` (mutable) calls `detach()`, which copies if the GUI still holds the buffer (`slint_sharedvector.h`), so reuse can never tear a displayed frame. Worst case equals today's cost. |
| Size clamp moves from `main.cpp` into the renderer: `[16, 3840] × [16, 2160]`, with a uniform downscale above the maximum so the aspect ratio is preserved. | The old `320×240` minimum is larger than the tile on some aspects, and the old per-axis clamp distorted the aspect of oversize targets. |
| `suspend_output()` / `resume_output(ResumeFrom)`: frame delivery is suspended before `Player::stop()` and on idle. Frames mpv renders while suspended are acknowledged but not delivered. On `FILE_LOADED`, delivery resumes with `ResumeFrom::NextFrame`: everything rendered while suspended is forgotten (it can only be the previous file's clear frame, because `FILE_LOADED` precedes the new file's first frame) and delivery starts at the next rendered frame. When a valid video size is reported (e.g. after a track change), delivery resumes with `ResumeFrom::Latest`, which re-renders the latest frame so a first frame rendered just before the resume is not lost. | Measured (`stopprobe`, 6 runs): in 5 of 6 runs libmpv renders one black clear frame 2–4 ms after `stop`, after `idle-active` and before `video-out-params` becomes unavailable. Without suspension it reaches the UI and sets `has-video` back to true, and a later forced redraw would re-render it. Suspending on the stop request (not on the idle event) wins that race by construction. |

### 7.2 Video-Aspect Render Target
The render target is the **largest rectangle with the video's display aspect** that fits the destination (window in full mode, tile in mini mode), in physical pixels:

```cpp
[[nodiscard]] constexpr std::pair<int, int> fit_aspect(int dst_w, int dst_h, double aspect) noexcept {
    if (aspect <= 0.0) return {dst_w, dst_h};
    const int w_from_h = static_cast<int>(dst_h * aspect + 0.5);
    return w_from_h <= dst_w ? std::pair{w_from_h, dst_h}
                             : std::pair{dst_w, static_cast<int>(dst_w / aspect + 0.5)};
}
```

- mpv then never draws letterbox bars into the buffer. `Image { image-fit: contain }` letterboxes on the GPU, so the frame and tile aspects match and nothing jumps at settle (§4.5 rule 6).
- Side benefit in full mode: a 16:9 video in a 16:10 window renders about 10% fewer pixels.
- `video-aspect-override` set via the aspect popover feeds the same `aspect` value.

### 7.3 `Player` Additions (`include/mpvx/player.hpp`, `src/player.cpp`)
- Observe `dwidth` and `dheight` (`MPV_FORMAT_INT64`). New callback `on_video_size(int dw, int dh)` in [include/mpvx/events.hpp](../include/mpvx/events.hpp). `main.cpp` forwards `dw / dh` to `MainWindow::video-aspect` and the renderer sizing logic.
- `bool is_idle_active() const noexcept`: mpv's `idle-active` (no file loaded or loading). `PlaybackState::Idle` is also reported on every `END_FILE`, including between playlist entries, so it cannot by itself mean "playback is over". When `idle-active` turns true, `Player` sends a second, forced `Idle` notification, because the first one (from `END_FILE`) would otherwise have been dropped as a duplicate.
- `bool is_paused() const noexcept`: true as soon as `pause()` is called, before mpv confirms it, and false as soon as `play()` is called; otherwise it follows mpv's `pause` property.
- `Result<void> set_mini_profile(bool mini)`: asynchronously sets `sub-visibility` (`no` in mini) and `osd-level` (`0` in mini), restoring the user's prior `sub-visibility` on exit. libmpv documents that subtitles and OSD make SW rendering slower ([render.h](../.deps/libmpv/include/mpv/render.h)), and they are unreadable at tile size. Uses `mpv_set_property_async` only (no synchronous mpv calls on the GUI thread).

### 7.4 `main.cpp` Wiring

| Handler | New behaviour |
| :--- | :--- |
| `on_return_to_picker_clicked` | Remove `player->pause()`. If `has-video && !idle`: forced progress flush, `mode_ = FullToMini`, `++gen`, set `view-mode = Picker` and `mini-state = Tile`, arm the settle timer (§5.3). Otherwise keep today's plain crossfade with `mini-state = Hidden`. |
| `on_return_to_player_clicked` / `on_mini_expand_clicked` | `++gen`; `mode_ = Full`; `renderer->set_target_size(full_target_px())`; `set_mini_profile(false)`; `renderer->set_frame_cap(false)`; `mini-shadow = 0`; `mini-state = Hidden`; `view-mode = Player`; reset HUD. Playback state is untouched, so expanding from the pill stays paused. |
| `on_mini_minimize_clicked` | Forced progress flush; `player->pause()`; `renderer->set_frame_cap(false)`; `mini-shadow = 0`; `mini-state = Pill`. No `Player::stop()`, no render-target change, `mode_` stays `Mini`. |
| `on_mini_restore_clicked(resume)` | `mini-state = Tile`; arm a `mini-collapse-duration + 40ms` settle timer (same generation guard) that sets `mini-shadow = 1`. If `resume`: `player->play()` and re-apply the frame cap (§7.6). |
| `on_state_changed` (existing) | Additionally, while `mini-active`: if state becomes `Playing` in `Pill` and `!player->is_paused()`, run `on_mini_restore_clicked(false)` to enforce the pill ⇒ paused invariant. The `is_paused()` check keeps a notification queued before a minimize (whose `pause()` mpv has not confirmed yet) from undoing it. If state becomes `Idle` and `player->is_idle_active()`, fade out and leave mini mode (§14 note 3); an `Idle` between playlist entries is ignored. If state becomes non-`Idle` during that fade, cancel it (`cancel_close()`). |
| `on_mini_fps_cap_changed(enabled)` | Store the flag; `renderer->set_frame_cap(enabled && mode_ == Mini && mini-state == Tile)`. |
| `on_picker_video_clicked` | If `mini-active`: run the expand path first, then the existing load flow. |
| `on_viewport_resized` | Convert to physical pixels (`× window().scale_factor()`). Today it passes logical pixels ([ui/app-window.slint](../ui/app-window.slint) `changed width`), which makes video blurry on HiDPI. Only apply when `mode_ == Full`: not in `Mini`, and not in `FullToMini` either, because the render target must stay constant during the shrink (§2.3 rule 4). `settle_shrink()` and expand recompute the target from the current geometry. |
| `on_mini_geometry_changed` | Store the tile size; apply only when `mode_ == Mini`. |
| `on_video_size` | Update `video-aspect`; recompute and apply the target for the current mode (only when not animating). |

**Progress persistence:** `on_time_pos` already calls `record_playback_progress(..., false)` continuously ([src/main.cpp](../src/main.cpp)), so watch position keeps updating during mini playback with no change. Forced flushes happen on shrink, minimize and app exit (existing).

**Continue Watching freshness:** `library->request_refresh()` on shrink is kept, so the currently playing item appears in Continue Watching with its position at shrink time. Live per-second card updates are out of scope.

### 7.5 Threading
- All new state (`mode_`, `mini_generation_`, stored sizes) lives on the GUI thread. No locks.
- `set_target_size` writes atomics read by the render thread (existing pattern). A size change racing with an in-flight render is harmless: the next frame uses the new size, and the `frame_in_flight` coalescing in `main.cpp` is unchanged.
- The settle timer is a `slint::Timer` on the GUI thread.

### 7.6 Opt-In 30 fps Cap (`VideoRenderer`)
- New `void set_frame_cap(bool enabled) noexcept` stores `min_handoff_interval_` (`std::atomic<int64_t>`, nanoseconds): `29 ms` when enabled, `0` when disabled. The cap is active only while `mode_ == Mini` and `mini-state == Tile`; every handler that leaves that state disables it (§7.4).
- In `render_frame()`, after a successful render, the frame callback is skipped when `now − last_handoff < min_handoff_interval_`. Effect by source rate:

| Source fps | Frames handed to the GUI |
| :--- | :--- |
| 60 | 30 (every second frame; frames are 16.7 ms apart, so 2 × 16.7 ≥ 29) |
| 50 | 25 |
| 30 / 25 / 24 | Unchanged (spacing already > 29 ms). The `4 ms` slack under 33.3 ms absorbs timing jitter so 30 fps sources never lose frames. |

- **What it saves:** half the GUI repaints of the window (including the picker behind the tile, §9 unmeasured item 1), half the texture uploads, and half the alpha loop work at 60 fps sources.
- **What it does not save:** mpv still renders every frame on the render thread (about 3.5 ms per frame at tile size, §2.2). Skipping `mpv_render_context_render()` would leave frame pacing to vo_libmpv's internal behaviour, which is not verified here and could disturb A/V sync. v1 therefore caps only the hand-off.
- **Phase 2 experiment:** apply `vf-add @minicap:fps=30` at settle and `vf-remove @minicap` at the start of expand, so mpv itself decimates. Adopt only if the filter reinitialisation causes no visible hitch on enter or exit (§10.3) and the CPU saving is measurable.

---

## 8. Edge Cases

| Case | Behaviour |
| :--- | :--- |
| Audio-only file (`has-video` false, playing) | No tile. Shrink path not taken; audio continues; top bar **Return to Player** is the way back. (v2: compact audio pill.) |
| Paused when entering the picker | Tile appears showing the paused frame; stays paused. |
| EOF while tile (`keep-open=yes`) | Last frame stays; centre button shows `play.svg`, and pressing it restarts from 0 (`seek 0 absolute` + play). |
| `✕` at EOF | Collapses to the pill as normal. `▶` on the pill restores the tile and restarts from 0, same as the tile's play button. |
| Playback error / idle while tile or pill | Tile or pill fades out (`160ms`), `mini-state = Hidden`. "Idle" means mpv's `idle-active` (§7.3). |
| Playlist advance or `loop-playlist` wrap while tile or pill | Mini mode is unchanged: the `Idle` reported by `END_FILE` between entries does not close it. If playback returns while a close fade is running (e.g. a file loaded over IPC), the fade is cancelled. |
| External resume while pill (media key, mpv IPC) | Tile restored automatically (§3.5 invariant, §7.4 `on_state_changed`). |
| Minimize during the shrink morph | Allowed. `mini-t` continues toward 1 while `collapse-t` animates toward 1; the combined interpolation goes straight to the pill rect. The settle timer still switches the render target to `Mini` (paused, so no frames are produced). |
| Picker navigation, search or sort while tile or pill | No effect on the mini player (§3.7). |
| 30 fps cap enabled, then minimize | Cap is disabled while in the pill; re-applied on resume (§7.6). |
| 30 fps cap enabled, then expand | Cap disabled at the start of expand; the full player always runs at full rate. |
| Buffering / network stream | Spinner at `28px` inside the tile. |
| Window resized during the morph | Target rects are reactive, so the animation tracks the new geometry. Render target changes are deferred until settle (§5.3). |
| Window resized while mini | `mini-geometry-changed` updates the target size (no animation running). |
| Fullscreen → Library button | Same morph inside the fullscreen window. Esc in the picker expands, and a second Esc exits fullscreen (existing order). |
| Very wide or tall video | `video-aspect` clamped to `[0.5, 3.0]` for tile geometry; the image is letterboxed inside by `contain`. |
| Aspect override changed while mini | Not reachable (popovers are player-only). Applied on expand. |
| A/B loop or loop mode active | Continues unchanged in mini mode. |
| Scrubbing state when leaving the player | `controls-bar.is-scrubbing` cannot be true (controls hidden); no special handling. |
| Popover open when leaving the player | Existing Esc order closes popovers first; the Library button closes `active-popover` before shrinking. |

---

## 9. Performance Budget & Acceptance Criteria

Measured on the reference machine (§2.1) with a 1080p60 source.

| Metric | Budget | How to measure |
| :--- | :--- | :--- |
| GUI frame time during shrink, expand, collapse and restore | **0 frames > 16.7 ms** over 20 consecutive transitions of each kind | `SLINT_DEBUG_PERFORMANCE=console` |
| Process CPU, pill state | Within 5 points of the paused full player | `pidstat -p <pid> 1 30` |
| GUI frames delivered, tile with 30 fps cap, 60 fps source | 30 ± 1 per second | Renderer hand-off counter |
| Render-thread time per frame in mini mode | ≤ 5 ms mean, ≤ 7 ms p95 | Renderer timing log (debug build flag) |
| Process CPU, mini mode, 1080p60 | ≤ 60% of the same file in full-window mode, same session | `pidstat -p <pid> 1 30` |
| Pixel upload in mini mode | ≤ 40 MB/s at scale factor 1 | `w × h × 4 × fps` from the renderer log |
| Steady-state allocations per frame | 0 (ring reuse) | Ring-hit counter in the renderer log |
| Letterbox jump at settle | None visible | Screen recording, frame-by-frame review at 60 fps |
| Expand: time to first full-size frame | ≤ 1 frame period after request | Renderer log timestamp vs. request timestamp |

**Not yet measured, verify in Phase 2:**
1. **Picker repaint cost while the tile plays.** The Slint GPU renderer probably repaints the whole window on each new video frame, which means every visible card at 60 fps. The opt-in 30 fps cap (§3.6, §7.6) halves this cost for users who enable it. It stays opt-in regardless of the result. If the default (full rate) misses the GUI frame budget, the fix must come from the rendering side (for example a static snapshot of the picker behind the tile), not from turning the cap on by default.
2. **Render cost at HiDPI.** At scale factor 2 the tile target is up to `960×540` physical, between the measured `480×270` and `1280×720` rows. If it exceeds the budget, cap the mini target at `640` physical px wide and let the GPU upscale.

---

## 10. Testing & Verification

### 10.1 Unit Tests (`tests/test_player.cpp`, `tests/test_renderer.cpp` new)
- `fit_aspect`: 16:9 into 16:10 → height-limited; 4:3 into 16:9 → width-limited; aspect ≤ 0 → passthrough; rounding never exceeds the destination.
- Renderer force-redraw: with a paused libmpv instance (`vo=libmpv`, `--pause`, `av://lavfi:testsrc2`), `set_target_size()` produces exactly one frame callback at the new size.
- Ring reuse: 100 frames at constant size deliver buffers from at most 3 distinct allocations when the consumer drops each frame promptly.
- Mini profile: `set_mini_profile(true)` then `(false)` restores a user-set `sub-visibility=no`.
- Idleness: a two-entry playlist reports `Idle` with `is_idle_active() == false` on the advance, and a final `Idle` with `is_idle_active() == true` after `stop()` (with `keep-open=yes`, the last entry pauses at EOF instead of going idle).
- Frame cap: with a 60 fps `av://lavfi:testsrc2=r=60` source and `set_frame_cap(true)`, the hand-off rate over 3 s is 30 ± 1 fps; with a 30 fps source it is 30 ± 1 fps (no frames lost); with `set_frame_cap(false)` the 60 fps source hands off 60 ± 1 fps.

### 10.2 Headless Slint UI Tests (`tests/test_mini_player_ui.cpp` new, same harness as [tests/test_picker_ui.cpp](../tests/test_picker_ui.cpp))
- `return-to-picker` with `has-video` → `mini-state == Tile`, `view-mode == Picker`.
- Tile `✕` → `mini-minimize-clicked` invoked; after C++ sets `mini-state = Pill`, `bottom-inset == 72px` and the pill's accessible label contains the media title.
- Pill body → `mini-restore-clicked(false)`; pill `▶` → `mini-restore-clicked(true)`; pill `⤢` → `mini-expand-clicked`.
- `Space` with `mini-state == Pill` and search unfocused → `mini-restore-clicked(true)`; with search focused → not handled.
- Navigation persistence: with `mini-state == Tile`, invoking `picker-nav-selected`, `picker-breadcrumb-clicked`, `picker-search-changed` and `picker-sort-changed`, and replacing every picker model, leaves `mini-state == Tile`. Same for `Pill`.
- `Esc` with a non-empty search clears the search and leaves `mini-state` unchanged; a second `Esc` invokes `return-to-player-clicked`.
- Settings popover: selecting `30 fps` sets `mini-fps-cap == true` and invokes `mini-fps-cap-changed(true)`; default is `false`.
- Tile geometry at `1280×720` window, aspect 16/9: `mini-w == 332.8px`, `mini-h == 187.2px` (within 0.5 px).
- Clamp bounds: window `640×360` → `mini-w == 280px`; window `3840×2160` → `mini-w == 480px`.
- Portrait aspect 9/16 → height-capped formula.
- Picker `bottom-inset` equals `mini-h + 32px` in `Tile`, `72px` in `Pill` and `0` in `Hidden`.
- `Esc` in picker with `mini-active` → `return-to-player-clicked` invoked; `Space` toggles play only when search is not focused.
- Invalid state guard: setting `view-mode = Player` through the expand path always clears `mini-active`.
- Pill → player: right after the expand (`collapse-t > 0.5`) the pill is still visible; once settled it is not.
- Idle close from the pill: after `mini-closing` and then `mini-state = Hidden`, the pill does not reappear while `collapse-t` runs back to 0.
- Double-click on the tile body (real pointer events): expands once; the second click, landing on the full player, invokes neither `play-pause-clicked` nor `fullscreen-toggled`. After the guard interval, a click toggles pause again.
- Harness note: the headless backend does not render, and Slint starts an animation only when the property is read after its binding changes, so the tests read `mini-t` and `collapse-t` (`read_morph()`) after each state change, the way a rendered frame would.

### 10.3 Manual Motion QA
- Record shrink, expand, collapse to pill, restore from pill, pill → full player, minimize during shrink, and mid-flight reversal at 60 fps; review frame-by-frame for jumps, bar flashes, chrome popping during the morph and shadow artefacts.
- Repeat with 16:9, 2.39:1, 4:3 and 9:16 sources; windowed, maximized and fullscreen; scale factors 1 and 2.

---

## 11. Phased Implementation Plan

### Phase 0: Renderer prerequisites (also improve full-window playback)
1. `set_target_size` in physical px + `force_redraw_` path (§7.1).
2. Video-aspect render target via `dwidth`/`dheight` observation (§7.2, §7.3).
3. `SharedPixelBuffer` ring.
4. Unit tests §10.1.

**Exit:** full-window CPU at 1080p not worse than §2.2; paused resize re-renders; HiDPI is crisp.

### Phase 1: Mini player
1. Theme tokens, `MainWindow` properties and callbacks, tree reorder (§6).
2. `MiniOverlay` component, `VideoView` mini mode, picker `bottom-inset`.
3. `MiniPill` component and tile ↔ pill collapse (§3.5, §4.4).
4. `main.cpp` state machine, settle timer, generation guard, mini profile, pill ⇒ paused invariant (§5, §7.4).
5. Opt-in 30 fps cap: renderer hand-off cap and Settings popover section (§3.6, §6.7, §7.6).
6. Headless UI tests §10.2.

**Exit:** all §9 budgets met except the two unmeasured items.

### Phase 2: Verification & tuning
1. Measure picker repaint cost and HiDPI target cost (§9). If the HiDPI budget is missed, apply the target cap. The 30 fps cap stays opt-in in every case.
2. Run the `vf fps=30` decimation experiment (§7.6) and adopt it only if it is hitch-free and measurably cheaper.
3. Manual motion QA §10.3.
4. Commit the benchmark (§2.4); update [Video Picker UI Spec §5.2](video-picker-ui-spec.md) to reference this document.

### 11.3 Benchmark Artefacts
`bench/sw_render_bench.cpp` (from `bench.cpp`) and `bench/alpha_bench.cpp` (from `alpha.cpp`), built only with `-DMPVX_BUILD_BENCH=ON`.

---

## 12. v2 Specification: Drag & Four-Corner Snapping (Plus Future Work)

v2 adds tactile drag-to-reposition and spring-animated snapping to any of the four viewport corners while keeping v1's single-element `VideoView` architecture, constant render-target invariant (§2.3 rule 4), and §4.5 motion rules intact.

### 12.1 Corner Model & Safe Dock Geometry

```slint
// ui/theme.slint
export enum MiniCorner { BottomRight, BottomLeft, TopRight, TopLeft }
```

`BottomRight` remains the default on launch. The active `mini-corner` is **session-only** (not persisted across launches, matching §1.2) and survives `Player ↔ Picker`, `Tile ↔ Pill`, and all picker navigation (§3.7).

Because `TopBar` (`height: 64px`, `top-bar-content-height: 56px`) sits at layer 5 *above* `VideoView` (layer 2) and `MiniPill` (layer 3) with a full-width `TouchArea` (`bar-touch`), top corners must clear `TopBar` completely so the top bar never intercepts pointer events or obscures the tile's `[⤢]` / `[✕]` buttons:

```
┌─────────────────────────────── window-shell ───────────────────────────────┐
│  TopBar (56px content / 64px gradient — never overlapped by mini player)   │
├────────────────────────────────────────────────────────────────────────────┤
│  16px ╭──────────────╮                              ╭──────────────╮ 16px  │
│  ───▶ │  TopLeft     │              │               │    TopRight  │ ◀───  │
│       │  (16, 72)    │              │               │ (W-w-16, 72) │       │
│       ╰──────────────╯              │               ╰──────────────╯       │
│  ───────────────────────────────────┼────────────────────────────────────  │
│                                     │  safe-area midpoint:                 │
│                                     │  x = W / 2                           │
│       ╭──────────────╮              │  y = (72 + H - 16) / 2               │
│       │  BottomLeft  │              │               ╭──────────────╮       │
│       │ (16, H-h-16) │              │               │ BottomRight  │       │
│  16px ╰──────────────╯                              ╰──────────────╯ 16px  │
└────────────────────────────────────────────────────────────────────────────┘
```

| Corner | Tile `(x, y)` | Pill `(x, y)` | Picker `bottom-inset` | Picker `top-clearance` |
| :--- | :--- | :--- | :--- | :--- |
| `TopLeft` | `(16px, 72px)` | `(16px, 72px)` | `0px` | `mini-h + 88px` (Tile) / `128px` (Pill) |
| `TopRight` | `(W - mini-w - 16px, 72px)` | `(W - pill-w - 16px, 72px)` | `0px` | `mini-h + 88px` (Tile) / `128px` (Pill) |
| `BottomLeft` | `(16px, H - mini-h - 16px)` | `(16px, H - 40px - 16px)` | `mini-h + 32px` (Tile) / `72px` (Pill) | `0px` |
| `BottomRight` | `(W - mini-w - 16px, H - mini-h - 16px)` | `(W - pill-w - 16px, H - 40px - 16px)` | `mini-h + 32px` (Tile) / `72px` (Pill) | `0px` |

- **Top margin:** `Theme.mini-top-margin` (`72px` = `top-bar-content-height + mini-margin`), matching the `16px` margin on left, right, and bottom.
- **Drag clamping bounds:** while dragging, `(drag-x, drag-y)` is clamped to `[8px, max(8px, W - mini-w - 8px)]` horizontally (`Theme.mini-drag-edge-pad`) and `[64px, max(64px, H - mini-h - 8px)]` vertically (`Theme.mini-top-drag-edge-pad`). The `8px` edge pad gives a slight tactile over-drag cushion past the `16px` resting margin without entering `TopBar` (`64px`) or clipping outside `window-shell`.
- **Picker `bottom-inset`:** applies only when `mini-corner` is at the bottom (`BottomLeft` or `BottomRight`), avoiding an unnecessary blank band at the bottom of the picker when the mini player is docked at the top.
- **Picker `top-clearance`:** at a top corner, the whole picker (sidebar, breadcrumb bar and grid) starts below the tile or pill: `VideoPicker`'s top padding becomes `max(60px, top-clearance)`, where `top-clearance = 72px + (mini-h | 40px) + 16px` (`MainWindow.picker-top-clearance`). A scrollable inset like `bottom-inset` is not enough here, because the breadcrumb bar, the sidebar nav and the first row at scroll offset 0 cannot be scrolled out from under the tile. Like the bottom inset, it is not animated: it switches when `mini-corner` or `mini-state` changes (at drop, not during the drag), costing the picker about `mini-h + 28px` of height while docked at the top.

### 12.2 Visual Design: Tile Chrome, Drag Lift & Corner Drop-Zone Guides

#### Idle & Hover Tile Chrome (`MiniOverlay`)
- **Subtle drag grip:** a `18×3px` capsule (`border-radius: 1.5px`, `#ffffff66`) at top-left (`x: 10px, y: 14px`) fades in alongside the hover scrim. Even on the narrowest clamped `0.5` portrait tile (`110px` wide at `220px` height), it sits `8px` clear of `[⤢]` (`x: 34px`).
- **Unified hover fade:** `play-btn`, `expand-btn`, `close-btn`, the drag grip, and the `#00000066` scrim all fade together over `150ms ease-out` (`opacity` + `visible: self.opacity > 0.01`) instead of popping instantaneously.
- **Cursors:** `MouseCursor.grab` over the draggable tile body, `MouseCursor.grabbing` while actively dragging, and `MouseCursor.pointer` over `[▶]`, `[⤢]`, `[✕]`.

#### Active Drag Elevation ("Lift")
When the user drags the tile past the threshold:
1. **Unobstructed video:** the hover scrim, grip pill, and `[▶]` / `[⤢]` / `[✕]` buttons fade out (`150ms ease-out`) and disable hit-testing (`IconButton.interactive: false`, which blocks input without switching to the disabled icon colour mid-fade) so the playing video and the corner target behind it stay crystal clear. The `3px` progress bar remains visible.
2. **Shadow lift (no re-blurring):** `drop-shadow-blur` stays constant at `24px` (so Slint never re-computes a varying blur radius per frame), while `drop-shadow-offset-y` animates `8px → 14px` and `drop-shadow-color` deepens `#00000099 → #000000cc` over `140ms ease-out`.
3. **Accent ring:** the tile border transitions (`140ms ease-out`) from `1px Theme.card-border-hover` to `1.5px #818cf8d9` (matching `Theme.icon-accent`), easing through `#818cf880` during the snap spring back to `1px Theme.card-border` / `card-border-hover` on settle.

#### Corner Drop-Zone Guides (`MiniCornerGuides`)
While dragging (`mini-dragging && mini-state == Tile && !mini-closing`), a lightweight overlay drawn **between `VideoPicker` and `VideoView`** reveals the four snap slots (`mini-w × mini-h`, `Theme.card-radius`):

```
  ╭╌╌╌╌╌╌╌╌╌╌╌╌╌╌╮                      ╭──────────────╮
  ┆      (•)     ┆   inactive slot      │  ┏━━━━━━━━┓  │  active target slot
  ┆              ┆   #090a0f59 fill     │  ┃ ( ● )  ┃  │  #818cf822 wash
  ╰╌╌╌╌╌╌╌╌╌╌╌╌╌╌╯   1px #ffffff24      │  ┗━━━━━━━━┛  │  1.5px #818cf8d9
                                        ╰──────────────╯
                    ┌─────────────────┐
                    │   Dragged Tile  │  ← floats above guides (14px shadow, 1.5px #818cf8d9)
                    │▓▓▓▓▓▓▓░░░░░░░░░░│
                    └─────────────────┘
```

- **Visibility & fade:** fades in over `120ms ease-out` on drag start and fades out over `160ms ease-out` on release (`visible: self.opacity > 0.01`, so zero render/hit-test cost when idle).
- **Inactive slots (3 corners):** `#090a0f59` translucent backdrop, `1px #ffffff24` border, centered `24×24px` ring (`#ffffff10` fill, `1px #ffffff24` border) with a `6×6px` `#ffffff40` center dot.
- **Active target slot (nearest corner):** as the dragged tile crosses the safe-area midpoint, the target slot smoothly transitions (`140ms ease-out`) to `#818cf822` fill and `1.5px #818cf8d9` glowing border, while its centered reticle expands (`24px → 34px` ring `#818cf82e` / `#818cf899`, `6px → 10px` dot `Theme.icon-accent`) with `160ms ease-out-back`.

### 12.3 Gesture & Click-vs-Drag Discrimination

1. **Button isolation:** `[▶]`, `[⤢]`, and `[✕]` `IconButton`s sit as siblings *above* `body-touch` in `MiniOverlay`, so pressing any button never starts a tile drag.
2. **Threshold:** drag begins only on left-button press (`PointerEventButton.left`) once pointer displacement reaches `Theme.mini-drag-threshold` (`6px` Euclidean: `dx² + dy² ≥ 36px²`). Sub-threshold clicks expand to the full player as in v1. Pressing another button mid-drag is ignored. Releasing another button ends the drag (snap to `nearest-corner`, no expand), because Slint ends the pointer grab on any button release and the tile could no longer follow the pointer once it left the tile.
3. **Zero-drift `TouchArea` tracking:** because `body-touch` moves with `VideoView`, `tile-x + (mouse-x - pressed-x)` simplifies algebraically to `pointer_window_x - pressed_x`, keeping the grabbed pixel locked to the cursor with zero drift even when clamping at viewport edges. This holds only while the tile moves solely with the pointer, which is why a press freezes any snap in flight (item 5).
4. **Release click suppression:** `did-drag` is set when threshold is crossed and cleared on `PointerEventKind.down` (and after suppressing `clicked`). Regardless of whether Slint dispatches `pointer-event(up)` or `clicked` first, releasing after a drag **never** triggers `expand-clicked()`.
5. **Mid-snap re-grab:** a left press while a snap spring is still in flight (`press-started`) immediately cancels `snap-settle-timer`, captures the live interpolated `(tile-x, tile-y)` into `(mini-drag-x, mini-drag-y)`, clears `mini-snapping` and pins the tile there (`mini-held`). Freezing at the press, not at the threshold, matters: otherwise the spring's own motion between press and move counts toward the `6px` threshold and is added to the drag, making the tile jump. Dragging past `6px` then resumes 1:1 tracking from the pinned position; releasing without dragging (`press-released`) is a click and restarts the snap from where the tile was frozen.
6. **Drag waits for the shrink:** `body-touch` only arms a drag once `mini-t == 1.0`. `MiniOverlay` appears at `mini-t >= 0.95`, but until the morph ends the drawn rect is `m-x * mini-t` and still resizing, so a drag seeded from the resting position would jump away from the cursor. Clicks still expand during that window.

### 12.4 Transition Choreography & Curves

#### A. Drag Start (`0–150 ms`)

| t | Event |
| :--- | :--- |
| 0 ms | Threshold (`6px`) crossed. `snap-settle-timer` stopped; live `(tile-x, tile-y)` copied to `(mini-drag-x, mini-drag-y)`; `mini-snapping = false`, `mini-dragging = true`. Tile tracks pointer 1:1 (`0ms` lag). |
| 0–120 ms | `MiniCornerGuides` fades in (`opacity: 0 → 1`, `ease-out`); current nearest corner lights up. |
| 0–140 ms | Tile lifts: `drop-shadow-offset-y` `8px → 14px`, shadow deepens `#00000099 → #000000cc`, border shifts to `1.5px #818cf8d9`. |
| 0–150 ms | Hover scrim, grip pill, and overlay buttons fade out (`opacity → 0`). |

#### B. Release → Corner Snap (`0–240 ms`)

| t | Event |
| :--- | :--- |
| 0 ms | Pointer released (`up` / `cancel`). `mini-corner` updates to `nearest-corner(mini-drag-x, mini-drag-y)` (comparing tile center against `(W/2, (72 + H - 16)/2)`). `mini-snapping = true`, `mini-dragging = false`; `snap-settle-timer` (`240ms + 20ms`) armed. |
| 0–140 ms | Shadow settles (`14px → 8px`, `#000000cc → #00000099`); border eases toward `#818cf880` and back to hover/idle color. |
| 0–160 ms | `MiniCornerGuides` fades out (`opacity: 1 → 0`, `ease-out`). |
| 0–240 ms | `tile-x` and `tile-y` animate from `(mini-drag-x, mini-drag-y)` to `(target-tile-x, target-tile-y)` with a tuned spring curve (`cubic-bezier(0.22, 1.25, 0.36, 1)`). |
| 260 ms | `snap-settle-timer` fires and sets `mini-snapping = false`, restoring `0ms` position binding so subsequent window resizes stick immediately to the corner. |

> **Why `cubic-bezier(0.22, 1.25, 0.36, 1)` instead of raw CSS `ease-out-back` (`cubic-bezier(0.34, 1.56, 0.64, 1)`):** Standard `ease-out-back` overshoots by ~10.5%. On a `900px` cross-window throw, a 10.5% overshoot pushes the tile ~`95px` past the `16px` margin, clipping half the video off `window-shell`. Tuning the curve to `cubic-bezier(0.22, 1.25, 0.36, 1)` yields a crisp ~2% overshoot (`~18px` max on a full-width throw, `~3–6px` on nearby snaps) that stays clear of the window frame on typical windows. Because the overshoot scales with travel, while `mini-snapping` the drawn position (`shown-tile-x/y`, also exposed as `mini-tile-x/y`) is clamped to the drag pads (`8px` sides, `64px` top), so a ~`1000px` throw in a `2560px` window flattens against the pad instead of clipping the tile at the window edge or slipping under `TopBar`.

#### C. Corner-Aware `Player ↔ Tile`, `Tile ↔ Pill`, and Interruptions
- **`Player ↔ Tile` (`mini-t`):** shrinks to and expands from `(tile-x, tile-y)` of `mini-corner` (`300ms` / `260ms ease-out-expo`).
- **`Tile ↔ Pill` (`collapse-t`):** collapses to and restores from `(pill-x, pill-y)` in the **same** `mini-corner` (`220ms ease-in-out-cubic`), pinning the outer corner during collapse.
- **`Pill → Player` (`⤢`, `Esc`):** expands smoothly from `mini-corner`'s pill rect to full screen (`260ms ease-out-expo`).
- **Minimize mid-drag / mid-snap (`✕`, `Ctrl+W`):** commits `nearest-corner` (if dragging), clears `mini-dragging`, sets `mini-snapping = true` (`snap-settle-timer` armed), and collapses cleanly into `MiniPill` at that corner.
- **Expand mid-drag / mid-snap (`Esc`, Return to Player, card click):** `changed mini-state` pins the tile at its current position (`mini-held`), clears `mini-dragging` and `mini-snapping`, and commits no corner (`mini-corner` is unchanged). The drag shadow drops immediately (`drop-shadow-blur: 0px`, §4.5 rule 3) and `VideoView` morphs straight from its current position to full screen. `mini-held` clears the next time `mini-state` becomes `Tile`.
- **Idle close mid-drag (`mini-closing`):** clears `mini-dragging` and fades out in place (`160ms`).
- **Normative §4.5 compliance:** the mpv render target never changes during drag or snap (all 4 corners share `mini-w × mini-h`); `VideoView` is never recreated or reparented; `MiniOverlay` stays mounted across drag and snap because `mini-chrome-visible` remains true; `drop-shadow-blur` never animates its radius. The `24px` shadow stays on through drag and snap (`Tile`, not closing) so the lift eases out over the snap even when the drag began before the settled shadow (`mini-shadow == 0`); this is the translation-only exception in §4.5 rule 3.

### 12.5 Slint Additions, Edge Cases & Verification Plan

#### Theme Tokens & `MainWindow` Interface
- **New `Theme` tokens (`ui/theme.slint`):** `mini-snap-duration: 240ms`, `mini-top-margin: 72px`, `mini-drag-threshold: 6px`, `mini-drag-edge-pad: 8px`, `mini-top-drag-edge-pad: 64px`.
- **`MainWindow` (`ui/app-window.slint`):** `in-out property <MiniCorner> mini-corner: BottomRight`, `out property <bool> mini-dragging`, `out property <bool> mini-snapping`, `out property <length> mini-tile-x/y`, `out property <length> mini-pill-x/y`, plus `corner-x`, `corner-y`, `nearest-corner` helpers and `snap-settle-timer`.
- **C++ backend (`src/main.cpp`):** **zero changes required** to `MiniPlayerController`, because render-target sizing (`mini-w`, `mini-h`) is invariant across all four corners.

#### v2 Edge Cases

| Case | Behaviour |
| :--- | :--- |
| Drag while paused or at EOF | Identical drag, corner guides, and spring snap; last paused frame stays sharp with no re-render. |
| Drag while buffering | `28px` spinner remains centered inside the moving tile. |
| Right- or middle-button drag | Ignored; only `PointerEventButton.left` initiates drag. Pressing one mid-drag is ignored; releasing one ends the drag with a snap and the later left release does not expand (§12.3.2). |
| Press during the shrink morph | Click expands; drag is not armed until `mini-t == 1.0` (§12.3.6). |
| Pointer `cancel` or window leave while pressed | Treated identically to `up`: snaps cleanly to `nearest-corner` and clears `mini-dragging`. |
| Window resized while docked in any corner | `0ms` binding keeps the tile pinned to its corner (`16px` / `72px` margins). |
| Portrait (`9:16`) or extreme (`0.5`–`3.0`) aspect | Guides and corners use `mini-w × mini-h` directly; `18px` top-left grip clears `[⤢]` on `110px` min width. |
| Smallest window (`640×360`) | `mini-w = 280px`, `mini-h = 157.5px`; horizontal gap between left/right slots is `48px`, vertical gap is `114.5px` — all 4 slots remain distinct and non-overlapping. |

#### v2 Test & Motion QA Plan (`tests/test_mini_player_ui.cpp`)
1. Sub-threshold pointer movement (`< 6px`) invokes `mini-expand-clicked` and does not set `mini-dragging`.
2. Dragging past `6px` into each quadrant sets `mini-dragging == true`, suppresses `mini-expand-clicked` on release, sets `mini-corner` to the nearest of `{TopLeft, TopRight, BottomLeft, BottomRight}`, clamps within `[8px, 64px]` drag pads, and settles to exact corner coordinates after `240ms`.
3. Re-grabbing the tile mid-snap (`~90ms` after release) cancels `mini-snapping`, enters `mini-dragging` without position jump, and snaps to the newly targeted corner. Pressing mid-snap and holding still keeps the tile frozen with time advancing; sub-threshold jitter does not start a drag; a drag after a pause moves the tile by exactly the pointer delta.
3a. Right/middle buttons mid-drag never leave the tile stuck or expand; the tile cannot be dragged while `mini-t < 1`; the snap overshoot stays within the drag pads on a `2560px` window; expanding mid-drag keeps `mini-corner` and starts from the dragged position.
4. `MiniPill` position, `picker-bottom-inset` (`0px` at top corners, `mini-h + 32px` / `72px` at bottom corners), and window resize anchoring all follow `mini-corner`.
5. Interrupting an active drag with expand (`Hidden` / `Player`) or minimize (`Pill`) clears `mini-dragging` and transitions cleanly.
6. Manual 60 fps motion QA: verify corner guide fade, lift shadow, spring overshoot clearance against `window-shell` edges, and `Tile ↔ Pill` collapse in all 4 corners.

### 12.6 Remaining Future Work (v2+)
- Compact audio-only pill (reusing `MiniPill` for audio-only playback).
- Live Continue Watching progress for the playing item.
- A persistent settings store (`$XDG_CONFIG_HOME/mpvx/`) so the 30 fps cap and preferred `mini-corner`, along with hwdec, speed and aspect, survive restarts.
- mpv OpenGL render API with Slint's rendering notifier and borrowed GL textures: zero-copy frames, removing the copy-back and upload costs behind the §2.3 floor.

## 13. Open Questions
All questions from the first draft are resolved (§1.3). Remaining:
1. Should the pill show a tiny paused-frame thumbnail (`28×28`, cropped) instead of the play icon alone? It would need no extra rendering, because the last frame is already in `video-frame`.
2. Should the 30 fps setting also be reachable from the picker (for example from the top-bar app menu), given the Settings popover is player-only today?

## 14. Implementation Notes (2026-11-20)
Where the implementation departs from, or fills gaps in, the draft above:

1. **Video size source.** `Player` observes `video-out-params` (`dw`/`dh`, swapped for 90°/270° rotation) instead of `dwidth`/`dheight`, and reports it through `PlayerCallbacks::on_video_size(VideoDisplaySize)`. Aspect overrides and rotation are therefore included.
2. **Visibility with the new tree order.** Because `VideoView` now draws above the picker, the plain (no mini player) crossfade fades `VideoView` in or out over the picker instead of fading the picker over the video: `opacity: close-fade * (morphing ? 1 : 1 - picker-opacity)`, where `morphing = mini-active || mini-t > 0.001`. During a morph both the picker and the video stay at opacity 1 (§4.5 rule 2).
3. **Idle dismissal (§8).** Clearing `mini-state` directly would animate `mini-t` back to 0 and grow the video over the picker. Instead C++ sets `MainWindow.mini-closing`, which fades the tile or pill out in place (`Theme.mini-close-fade-duration`, `160ms`). A generation-guarded timer then sets `mini-state = Hidden` and restores the full render target.
4. **`mini-geometry-changed`** takes no arguments; `MiniPlayerController` reads `mini-w`/`mini-h` and multiplies by `window().scale_factor()`.
5. **Picker inset (§3.4)** is added to the scroll *content* (the Flickable's `content-height`, and the last `ListView` row's height), not as viewport padding, so the last row scrolls clear of the tile without a blank band above it. It is exposed as `MainWindow.picker-bottom-inset`.
6. **Generation counters.** `MiniPlayerController` (in `src/main.cpp`) keeps two: `target_gen_` guards the render-target settle and the idle close, and `shadow_gen_` guards the shadow fade-in. Minimizing during the shrink bumps only `shadow_gen_`, so the render target still switches to `Mini` at settle (§8).
7. **Loading from the picker.** Open File and Open URL leave mini mode through the expand path too, not only card clicks, so `Player` + `Tile`/`Pill` can never occur.
8. **Frame cap.** Forced redraws (resize, resume) always reach the GUI. `VideoRenderer::Stats::frames_delivered` counts hand-offs, for the §9 budget.
9. **Not done.** The §2.4/§11.3 benchmark sources were not in the repository, so `bench/` is not committed. The Phase 2 items (§9 unmeasured items, §10.3 motion QA, the `vf fps=30` experiment) are still open.

### 14.1 Review Fixes (2026-11-20)
A code review against this spec found six defects. The fixes are specified in the sections above; this records why.

10. **Idle means `idle-active`** (§5.1, §7.3, §7.4, §8). Closing on any `PlaybackState::Idle` faded the tile out on every playlist advance while playback continued, which broke §3.7 and the loop-mode row of §8.
11. **Pill ⇒ paused enforcement checks `is_paused()`** (§7.4). `minimize()`'s `pause()` is asynchronous, so a `Playing` notification queued just before the minimize (e.g. Space then Ctrl+W) restored the tile straight away. A sub-millisecond window remains: mpv emitting the earlier resume after `minimize()` has already run. The worst outcome is a paused tile instead of the pill.
12. **Expand click guard** (§3.3, §6.4). Replaces the latching of `mini-mode` that the draft implied.
13. **Pill visibility on expand** (§6.3). The implementation had added a `mini-active` guard to stop the pill reappearing after the idle close (note 3). That guard also hid it instantly on expand, leaving about one frame with neither pill nor video. It is now scoped to the picker.
14. **Render target fixed during the shrink** (§7.4 `on_viewport_resized`). The draft's "`mode_ != Mini`" let window resizes re-render at full size while `FullToMini`.
15. **`ResumeFrom::NextFrame` on `FILE_LOADED`** (§7.1). `has_frame_` was set by frames dropped while output was suspended, so the clear frame was re-rendered and delivered before the next file's first frame.

**Known gaps:**
- Between playlist entries, `Idle` still clears `has-video`, so the tile (like the full player) shows `Theme.canvas-bg` until the next file loads. This is more noticeable now that the tile stays up.
- The §10.1 frame-cap test measures real time and occasionally reads about 28 fps for a 30 fps source (about 2 in 13 runs): a late frame followed by an early one falls under the 29 ms limit. The `4 ms` slack in §7.6 does not fully absorb jitter in a loaded debug build.
