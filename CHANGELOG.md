# Changelog

## 0.3.1 beta

- **Red flag restarts:** the 5 s Pre-Start no longer runs at a red flag restart. The game gives you no throttle there until its red lights begin, so the Pre-Start was 5 s in which you could not spool the turbo. The red lights now begin as in the standard game, and you spool the turbo under them.

## 0.3.0 beta

- **Blue lamps in the game's start lights:** during Pre-Start the five lamps of the game's own start lights on your HUD flash blue, then the red lights run as usual. They are part of the game's race screen now, so they follow its HUD layout and scale.
- **Turbo Spool in HUD customisation:** the TURBO SPOOL gauge is now one of the game's HUD elements. Move it, resize it, change its opacity or turn it off in the game's HUD customisation, where it is called Turbo Spool. It starts just under the start lights.
- **DLSS Frame Generation:** the blue lamps and the gauge now show with Frame Generation on too, because the game draws them. Spool Up draws them itself only when the game's HUD refuses them.
- **Log:** the two cases in which the start lights skipped a start without a log line now write one.
- **Swofty Launcher 1.8.38 or newer** is now needed.

## 0.2.2 beta

- **Race weekends:** Spool Up now also runs in a race that follows practice or qualifying in the same weekend. Before, it ran only when the race or sprint was the first session you entered that weekend, because the session name it checked stays on the weekend's first session. It now reads the game's own session type at the start lights.
- **Pre-Start:** the light panels along the track now flash blue in the game world too, not only on your screen. The start lights on the gantry stay the game's own: they have no blue lamps.
- **HUD in the game's style:** five blue start lights flash on your screen where the game's red lights come on, and the game's own lap counter says PRE-START. The blue panels at the screen edges are gone.
- **Turbo spool gauge:** styled like the game's timing panel and placed just above the dash. It turns green and says FULL when the turbo is spooled.
- **Warnings:** LOW-OUTPUT START and SLOW CAR AHEAD now show once for 5 s in the game's own message box, the one that shows DRS ENABLED. They no longer flash, and the amber panels at the screen edges are gone.
- **Steam only:** Spool Up is now listed for the Steam version of F1 25 only, and needs Swofty Launcher 1.8.36 or newer.

## 0.2.1 beta

- **Source code link:** the addon now names its GitHub repository in its Swofty details, as Swofty now asks of addons that run their own code in the game. Nothing changes in the game.

## 0.2.0 beta

First public release.

- **Pre-Start:** 5 s of flashing blue panels and PRE-START before the first red light, also after a formation lap and at a standing restart.
- **Turbo spool:** hold 10,000 rpm or more to fill the TURBO SPOOL bar. With an empty bar the engine has about a third of its power at lights out.
- **Low-output safety net:** a very slow car gets just enough MGU-K to hold a minimum acceleration until 50 km/h, and a flashing warning.
- **AI starts:** most clean, some poor, a few fumbled. A fumbled car bogs for 1.5 s before it recovers.
- **Settings:** every part can be switched off, and the spool time and revs can be changed. Custom settings are untested so far.
