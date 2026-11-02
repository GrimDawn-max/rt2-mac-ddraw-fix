# Railroad Tycoon II on Mac (CrossOver): DirectDraw campaign fix

A small `ddraw.dll` that makes **campaigns** in Railroad Tycoon II Platinum (GOG) work under CrossOver on a Mac.

## The problem

Each campaign scenario plays its intro video in 640x480 at 256 colours. CrossOver refuses that display mode on Mac displays, and the game stops with:

> DirectDraw Error #-141934591

New Scenario and the menus aren't affected, only campaigns.

## What the file does

It sits beside the game and passes every DirectDraw call to CrossOver's own DirectDraw, with two changes:

- **Campaign videos:** if a display mode is refused, it asks again at the size already in use, with the same colour depth. The intro video then plays and the scenario loads. Usually the video fills the screen; on some setups it may play in a small area at the top-left. Either way the game itself runs full screen.
- **Faster start on MacBook screens:** when the game asks which display modes exist, it answers with the classic modes the game uses (640x480, 800x600, 1024x768), each checked against your display. Otherwise a MacBook's built-in screen reports over a thousand modes, and the game never finishes starting.

It also writes a short log, `ddrawlog.txt`, in the game folder, listing the display modes the game asked for and the answers. It doesn't connect to anything or change any other files.

## Getting the game running

1. **Bottle:** a plain Windows 10 (64-bit) bottle with the GOG version installed. Don't add cnc-ddraw or dgVoodoo2; both crash this game under CrossOver.
2. **Main display:** the game uses the main display (the one with the menu bar), and that display must already be at a size the game uses before you start it: **800x600** or **1024x768**. On most Mac displays, if the game has to switch the display itself, the screen goes black with only sound. In System Settings › Displays › Advanced…, turn on *Show resolutions as list*, then *Show all resolutions*, and pick 800 × 600 (or 1024 × 768). **Keep the display's normal refresh rate** (on a MacBook that's the default, ProMotion), so your other screens stay usable while you play. Set the resolution back when you've finished.
   - A MacBook's built-in screen lists 800 × 600. Some external monitors list 1024 × 768.
   - Some monitors list neither; the Apple Studio Display is one. Play on the MacBook screen instead (see below), or use a BetterDisplay virtual screen.
3. **Starting the game:**
   - **Display at 800x600:** start it with `/800600`. GOG puts a shortcut for this, "Rt2 (force 800 x 600 mode)", in the game folder, but CrossOver doesn't show it as an icon. In CrossOver, select the bottle, click **Run Command**, **Browse** to the game folder, choose `Rt2 (force 800 x 600 mode).bat` and click **Run**. To keep it as an icon, use **Save Command as Launcher** in the same window.
   - **Display at 1024x768:** start the game normally.
4. **This fix:**
   - copy `ddraw.dll` into the game folder, next to `RT2_PLAT.EXE` (GOG's default: `C:\GOG Games\Railroad Tycoon II\`);
   - in CrossOver: bottle › Wine Configuration › **Libraries**, add `ddraw` and set it to **native, then builtin**.
5. **In the game:** Options › General: Color Depth **16 bit** (if the screen stays black, try **8 bit**), Full screen **on**, Color mouse cursor **off** (it causes crashes). When a campaign scenario starts, other screens may flash black for under a second while the intro video switches to 256 colours; that's expected.
6. **While playing, don't switch away** with Command-Tab: the game's screen goes black. Quit from the game's menu instead.

## With two or more monitors

The game always appears on the **main display** (the one with the menu bar).

1. In System Settings › Displays, select the screen you want to play on and set **Use as: Main display**. (Skip this if it's already the main display.) If your usual monitor doesn't list 800 × 600 or 1024 × 768, choose the MacBook's built-in screen.
2. Set that screen to **800 × 600** or **1024 × 768** as in step 2 above. Leave **Refresh rate** at its default.
3. Start the game as in step 3 above. Your other monitors stay usable.
4. When you've finished, set the resolution back and make your usual screen the main display again.

## Optional: play in a window with a BetterDisplay virtual screen

If you'd rather keep all your real screens at their normal resolution, the game also runs on a virtual screen made with BetterDisplay, which you watch in a window.

1. In BetterDisplay, create a virtual screen and set it to **800 × 600**.
2. In System Settings › Displays, set that virtual screen to **Use as: Main display**.
3. In BetterDisplay, open the virtual screen's **Picture in Picture** window (a Pro feature). Drag its corner to make it bigger.
4. Start the game with `/800600`, as in step 3 of *Getting the game running*.
5. To play, move the mouse pointer onto the virtual screen (past the edge of your real screen, where the virtual screen sits in the Displays arrangement); you'll see the pointer in the window. If clicks don't reach the game, click its icon in the Dock once.
6. When you've finished, make your usual screen the main display again.

## Steam version

The setup is the same, with two differences (not yet tested; please report back if you try it):

- Put `ddraw.dll` in the game's Steam folder, next to `RT2_PLAT.EXE` (in Steam: right-click the game › Manage › Browse local files).
- Instead of GOG's shortcut, add `/800600` in Steam: right-click the game › Properties › **Launch Options**.

## To remove

Delete `ddraw.dll` from the game folder and remove the `ddraw` override.

## Tested with

CrossOver 26.3, Apple Silicon, GOG Railroad Tycoon II Platinum 2.0.0.5:

- macOS 26.4 and 26.7, M1 Max: MacBook screen at 800x600; a BetterDisplay virtual screen at 800x600.
- macOS 27.2, M3 Ultra: an MSI monitor at its normal resolution, which the game switched to 1024x768 itself (New Scenario, 8 bit).
