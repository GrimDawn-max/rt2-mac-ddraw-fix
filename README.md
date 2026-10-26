# Railroad Tycoon II on Mac (CrossOver): DirectDraw campaign fix

A small `ddraw.dll` that makes **campaigns** in Railroad Tycoon II Platinum (GOG) work under CrossOver on a Mac.

## The problem

Each campaign scenario plays its intro video in 640x480 at 256 colours. CrossOver refuses that display mode on Mac displays, and the game stops with:

> DirectDraw Error #-141934591

New Scenario and the menus aren't affected, only campaigns.

## What the file does

It sits beside the game and passes every DirectDraw call to CrossOver's own DirectDraw. It changes only one thing: if a display mode is refused, it asks again at the size already in use, with the same colour depth. The intro video then plays in the top-left of the screen and the scenario loads.

It also writes a short log, `ddrawlog.txt`, in the game folder, listing the display modes the game asked for and the answers. It doesn't connect to anything or change any other files.

## Getting the game running

1. **Bottle:** a plain Windows 10 (64-bit) bottle with the GOG version installed. Don't add cnc-ddraw or dgVoodoo2; both crash this game under CrossOver.
2. **Main display:** the game uses the main display (the one with the menu bar). Before starting it, set that display to **800x600** (or 1024x768). In System Settings › Displays › Advanced…, turn on *Show resolutions as list*, then *Show all resolutions*. An **external display** is the most reliable; on a MacBook's built-in screen the game sometimes never finishes starting (sound, no picture). While you play, your other displays go black; that's normal. Set the resolution back when you've finished.
3. **Start option:** if your display has no 1024x768 mode, start the game with `/800600`, for example using GOG's "Rt2 (force 800 x 600 mode)" shortcut.
4. **This fix:**
   - copy `ddraw.dll` into `C:\GOG Games\Railroad Tycoon 2\`;
   - in CrossOver: bottle › Wine Configuration › **Libraries**, add `ddraw` and set it to **native, then builtin**.
5. **In the game:** leave Options › Full screen **on**, and Color mouse cursor **off**.

## To remove

Delete `ddraw.dll` from the game folder and remove the `ddraw` override.

## Tested with

CrossOver 26.3, macOS 26.4, Apple Silicon (M1 Max), GOG Railroad Tycoon II Platinum 2.0.0.5.
