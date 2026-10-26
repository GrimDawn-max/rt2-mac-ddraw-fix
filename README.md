# Railroad Tycoon II on Mac (CrossOver): DirectDraw campaign fix

A small `ddraw.dll` that makes **campaigns** in Railroad Tycoon II Platinum (GOG) work under CrossOver on a Mac.

## The problem

Each campaign scenario plays its intro video in 640x480 at 256 colours. CrossOver refuses that display mode on Mac displays, and the game stops with:

> DirectDraw Error #-141934591

New Scenario and the menus aren't affected, only campaigns.

## What the file does

It sits beside the game and passes every DirectDraw call to CrossOver's own DirectDraw, with two changes:

- **Campaign videos:** if a display mode is refused, it asks again at the size already in use, with the same colour depth. The intro video then plays in the top-left of the screen and the scenario loads.
- **Faster start on MacBook screens:** when the game asks which display modes exist, it answers with the classic modes the game uses (640x480, 800x600, 1024x768), each checked against your display. Otherwise a MacBook's built-in screen reports over a thousand modes, and the game never finishes starting.

It also writes a short log, `ddrawlog.txt`, in the game folder, listing the display modes the game asked for and the answers. It doesn't connect to anything or change any other files.

## Getting the game running

1. **Bottle:** a plain Windows 10 (64-bit) bottle with the GOG version installed. Don't add cnc-ddraw or dgVoodoo2; both crash this game under CrossOver.
2. **Main display:** the game uses the main display (the one with the menu bar). Before starting it, set that display to **800x600** (or 1024x768). In System Settings › Displays › Advanced…, turn on *Show resolutions as list*, then *Show all resolutions*. **Keep the display's normal refresh rate** (on a MacBook that's the default, ProMotion). The game then never has to switch the display, so your other screens stay usable while you play, whether the game is on the MacBook screen or an external one. With a different refresh rate (for example 60 Hz on a MacBook), macOS blacks out every other screen until you quit. Set the resolution back when you've finished.
3. **Start option:** start the game with `/800600`, for example using GOG's "Rt2 (force 800 x 600 mode)" shortcut. With the display already at 800x600, the game never has to switch modes.
4. **This fix:**
   - copy `ddraw.dll` into `C:\GOG Games\Railroad Tycoon 2\`;
   - in CrossOver: bottle › Wine Configuration › **Libraries**, add `ddraw` and set it to **native, then builtin**.
5. **In the game:** Options › General: Color Depth **16 bit**, Full screen **on**, Color mouse cursor **off** (it causes crashes). When a campaign scenario starts, other screens may flash black for under a second while the intro video switches to 256 colours; that's expected.

## With two or more monitors

The game always appears on the **main display** (the one with the menu bar).

1. In System Settings › Displays, select the screen you want to play on and set **Use as: Main display**. (Skip this if it's already the main display.)
2. For that screen, set the resolution to **800 × 600**: click **Advanced…**, turn on *Show resolutions as list*, then *Show all resolutions*. Leave **Refresh rate** at its default.
3. Start the game with GOG's **"Rt2 (force 800 x 600 mode)"** shortcut. Your other monitors stay usable.
4. When you've finished, set the resolution back and make your usual screen the main display again.

## To remove

Delete `ddraw.dll` from the game folder and remove the `ddraw` override.

## Tested with

CrossOver 26.3, macOS 26.4, Apple Silicon (M1 Max), GOG Railroad Tycoon II Platinum 2.0.0.5.
