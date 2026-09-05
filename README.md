# World Clock Wallpaper for Omarchy

An animated Wayland wallpaper with a rotating Earth, real-time daylight,
city flares, and local clocks around the world. The native OpenGL ES renderer
pauses when the desktop is covered, so it does not keep rendering behind an
opaque maximized or tiled workspace.

Plugin ID: `spacexrace.worldclock`

## Install

```sh
omarchy plugin add https://github.com/spaceXrace/omarchy-world-clock.git --enable
```

The first enable builds the native renderer into
`~/.cache/spacexrace.worldclock`; the git-managed plugin checkout stays clean
and can be updated normally. A stock Omarchy installation usually has the
required build stack. If the first build reports missing dependencies, install
them once:

```sh
omarchy pkg add gcc make pkgconf wayland wayland-protocols libglvnd libpng glib2
omarchy plugin disable spacexrace.worldclock
omarchy plugin enable spacexrace.worldclock
```

## Toggle and startup

Open the Omarchy menu and choose:

`Trigger` → `Toggle` → `World Clock Wallpaper`

The entry is added to the user extension file, so Omarchy updates do not
overwrite it. The check mark means the wallpaper is enabled. That enabled state
is stored by Omarchy itself: enabled starts automatically with the desktop;
disabled stays off across restarts.

The terminal equivalents are:

```sh
omarchy plugin disable spacexrace.worldclock
omarchy plugin enable spacexrace.worldclock
```

## Update

```sh
omarchy plugin update spacexrace.worldclock
```

The renderer rebuilds in the cache when its source changes.

## Remove

Remove the optional menu row first, then remove the plugin:

```sh
~/.config/omarchy/plugins/spacexrace.worldclock/bin/world-clock uninstall-menu
omarchy plugin remove spacexrace.worldclock
rm -rf -- "$HOME/.cache/spacexrace.worldclock"
```

The cache deletion is optional. Omarchy's plugin remover stops the service;
the existing static wallpaper is revealed immediately.

## Behavior

- Earth rotation period: 7.5 minutes
- Background star drift period: 7.5 minutes
- Default cap: 30 fps
- Coverage pause threshold: 95% of the target output
- Default output: the focused display at launch

The wallpaper never changes the current Omarchy theme, does not inhibit idle
or locking, and does not use the network at runtime. It currently targets one
display.

## Development

```sh
make
./build/cities-earth --assets assets --preview
omarchy plugin validate .
```

## License

Code is MIT licensed. Third-party data and artwork retain their own terms; see
[ATTRIBUTION.md](ATTRIBUTION.md). In particular, the extracted bitmap fonts do
not have a confirmed redistribution license.
