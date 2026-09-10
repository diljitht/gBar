# gBar

A fork of [scorpion-26/gBar](https://github.com/scorpion-26/gBar). Thanks to scorpion-26 for the original project.

This README documents only changes from the original. For shared features, general build and usage instructions, configuration, styling, plugins, and troubleshooting, see the [original README](https://github.com/scorpion-26/gBar#readme). Apply the fork-specific dependency and path changes below when following those instructions.

## Changes from upstream

Compared with upstream [`03bedc7`](https://github.com/scorpion-26/gBar/commit/03bedc7471add061fb15e0ca1c9d2f729b8c5d7b), checked on September 6, 2026. The implementation described here is this fork at [`e5d5af6`](https://github.com/diljitht/gBar/commit/e5d5af6).

### Features and behavior

- **Automatic reload:** gBar watches the active `config` and `style.scss`/`style.css` files. Saving either file triggers a debounced process reload with the original command-line arguments, so configuration and style changes take effect without a manual restart.
- **Background transparency:** The bar background uses 85% opacity without dimming text or icons. Adjust `$bar-opacity` in `style.scss` (`0` = transparent, `1` = opaque), or the alpha in the `.bar` RGBA color in `style.css`. This also affects audio fly-ins, which share the `.bar` class; tooltips and menus remain opaque. Existing custom themes need the same background rule to enable transparency.
- **Clock hover:** An optional animated additional date/time label appears on hover while the normal clock stays visible. `TimeFullOnHover` defaults to `false`; `DateTimeStyleFull` controls the additional label.
- **Hyprland scratchpads:** Special workspaces appear as named buttons in a separate group. Clicking toggles the scratchpad, and long names do not stretch regular workspace buttons. Requires `UseHyprlandIPC: true`.
- **Workspace commands:** Lua-style `hl.dsp` commands are attempted first, with legacy command fallback on failure, for compatibility with newer Hyprland versions.
- **Audio backend:** Direct PipeWire/SPA monitoring replaces `libpulse`; asynchronous `wpctl` commands replace `pamixer` for volume and mute changes.
- **GPU monitoring:** NVIDIA support and the `WithNvidia` Meson option are removed. AMD support is retained.
- **Tray styling:** Menu backgrounds, text, selection colors, and separators are adjusted, and a fixed tray-container offset is removed.
- **Popup locking:** Process-owned locks in `XDG_RUNTIME_DIR` replace `/tmp` sentinel files. Locks are released when the process exits, including after a crash. Audio and microphone popups share a lock.

Example configuration for the new clock-hover behavior:

```text
DateTimeStyle: %H:%M
TimeFullOnHover: true
DateTimeStyleFull: %a, %d/%m/%y
```

Scratchpad buttons display their actual Hyprland names. The `ws-special-active` and `ws-special-inactive` CSS classes are separate, but both use bold purple text in the shipped theme.

### Reliability fixes

- Package results are delivered on the GTK main loop with widget-lifetime checks; malformed output and failed commands do not terminate the bar.
- Bluetooth operations retain stable device data and ignore obsolete completions; tray callbacks check item lifetime and validate received pixmap data.
- Widget teardown cleans up timers and image references; scratchpad button references are reset when the bar is recreated.
- Hyprland IPC handles partial writes, closes sockets on errors, validates socket-path length, avoids SIGPIPE, and limits each exchange to two seconds.
- CPU sampling establishes an initial baseline and avoids double-counting guest time; disk/network failures and counter resets are handled more safely.
- Numeric configuration parsing validates complete values and ranges, supports CRLF files, and fixes inline-comment truncation.
- Popup shutdown handles SIGINT/SIGTERM through the main loop; monitor-change shutdown races and bottom-margin storage are corrected.

### Build and packaging changes

- Use this repository, `https://github.com/diljitht/gBar`, instead of the upstream clone URL.
- Replace `libpulse`/`pamixer` dependencies with `libpipewire-0.3`, `libspa-0.2`, and WirePlumber's `wpctl`. Audio control still launches an external command; it is not entirely native PipeWire control.
- An Arch [PKGBUILD](PKGBUILD) targeting this fork is included; install with `makepkg -si`. Ensure WirePlumber is installed for `wpctl`, since the package dependency list does not explicitly include it.
- The upstream Nix flake, Home Manager module, and Nix CI integration are removed; no replacement Nix setup is shipped.
- The plugin example moves from `example/` to [examples/](examples/), and the package-update helper moves from `data/update.sh` to [scripts/update.sh](scripts/update.sh).

### Fork-specific caveats

- The direct PipeWire backend does not yet fully handle default-device changes or device removal; monitoring/control can remain attached to an earlier device.
- Special-workspace names containing quotes or shell metacharacters are not safely escaped in toggle commands and should be avoided.
- Workspace polling still uses synchronous IPC: the timeout bounds UI stalls rather than eliminating them.
- Audio/microphone and Bluetooth popups require an absolute, private, user-owned `XDG_RUNTIME_DIR`. Their `gBar__audio.lock` and `gBar__bluetooth.lock` files intentionally remain on disk after exit. Do not delete them while a widget is running. Upstream advice to remove `/tmp/gBar__audio` or `/tmp/gBar__bluetooth` does not apply to this fork.
