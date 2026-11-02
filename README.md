# Railroad Tycoon II on Mac (CrossOver): DirectDraw fix

A small `ddraw.dll` that makes Railroad Tycoon II Platinum (GOG) run properly under CrossOver on a Mac: full screen on any display, campaigns included, with no display settings to change.

## The problem

The game switches the display to 640x480, 800x600 or 1024x768 when it starts. Under CrossOver on a Mac, that switch goes wrong on most displays:

- On displays without that exact size (the Apple Studio Display, many modern monitors) the screen goes black with only sound, and the pointer is stuck on another screen.
- Campaign intro videos ask for 640x480 at 256 colours, which CrossOver refuses on Mac displays; the game stops with *DirectDraw Error #-141934591*.
- Switching away with Command-Tab turns the screen black.

## What the file does

The game is told its display mode has been set, but your display is never changed. The game draws its screen as usual, and the fix shows that picture on your main display, scaled up to fill it while keeping the game's 4:3 shape (black bars at the sides). Scaling is sharp: the picture is enlarged by a whole number with crisp pixels, and only the last small step is smoothed. Mouse positions are translated to match.

This means:

- it works on any Mac display at its normal resolution, including the Studio Display and MacBook screens;
- campaigns and their intro videos work, in 16-bit and 256 colours;
- Command-Tab is safe: switch away and back as you like;
- the game's highest resolution, 1024x768, works everywhere.

Every other call goes to CrossOver's own DirectDraw. The file also writes a short log, `ddrawlog.txt`, in the game folder, listing what the game asked for. It doesn't connect to anything or change any other files.

## Setup

1. **Bottle:** a plain Windows 10 (64-bit) bottle with the GOG version installed. Don't add cnc-ddraw or dgVoodoo2; both crash this game under CrossOver.
2. **This fix:**
   - copy `ddraw.dll` into the game folder, next to `RT2_PLAT.EXE` (GOG's default: `C:\GOG Games\Railroad Tycoon II\`);
   - in CrossOver: bottle › Wine Configuration › **Libraries**, add `ddraw` and set it to **native, then builtin**.
3. **Start the game** normally from its CrossOver icon. It fills your main display (the one with the menu bar).
4. **For the sharpest picture,** in the game's Options untick **800 x 600** so it runs at 1024x768.

Leave your displays at their normal resolution; nothing needs setting before you play.

## Steam version

The setup is the same (not yet tested; please report back if you try it). Put `ddraw.dll` in the game's Steam folder, next to `RT2_PLAT.EXE` (in Steam: right-click the game › Manage › Browse local files).

## To remove

Delete `ddraw.dll` from the game folder and remove the `ddraw` override.

## Settings

For troubleshooting, these can be set as environment variables in the bottle:

- `DDRAWLOG_NOSCALE=1`: turn the display layer off; the game switches the real display itself, as without the fix.
- `DDRAWLOG_GDI=1`: draw with Windows GDI instead of OpenGL.

## Building from source

The source is in `src/`. With MinGW-w64 (on a Mac: `brew install mingw-w64`):

```
i686-w64-mingw32-gcc -O2 -shared -s -o ddraw.dll src/ddrawlog.c src/ddraw.def -static-libgcc -lopengl32 -lgdi32
```

## Tested with

CrossOver 26.3, macOS 26.7, M1 Max, GOG Railroad Tycoon II Platinum 2.0.0.5:

- Apple Studio Display at its normal resolution as the main display, and the MacBook screen as the main display;
- menus, 800x600 and 1024x768, a scenario, a campaign with its intro video, the 256-colour opening video, the colour mouse cursor, and Command-Tab away and back.

## Versions

- **v1.2:** the display is never changed; the game is shown scaled on the display at its own resolution. Replaces the earlier versions, which needed the display set to 800x600 or 1024x768 before starting.
- v1.1 and v1.0: earlier versions, no longer available.
