# Wave Progress Bar (Geode mod for Geometry Dash)

Turns the in-level progress bar into the wave gamemode: your equipped wave icon
travels along the bar, leaving a 45 degree zigzag trail.

## Project layout

```
wave-progress-bar/
├── mod.json                     Mod metadata and settings
├── CMakeLists.txt               Build script
├── logo.png                     Mod icon
├── about.md                     Description shown in Geode's mod list
├── changelog.md                 Changelog shown in Geode's mod list
├── src/main.cpp                 All mod code
└── .github/workflows/build.yml  Builds Windows / macOS / Android on GitHub
```

## Before you build

1. Open `mod.json` and change `id` (`yourname.wave-progress-bar`), `developer`
   and `repository` to your own.
2. Make sure Geometry Dash is on **2.2081** (the version in `mod.json`).

## Option A: build locally

1. Install the [Geode CLI](https://docs.geode-sdk.org/getting-started/geode-cli) and
   set up the SDK (`geode sdk install`, then `geode sdk install-binaries`).
   You also need a C++ toolchain (Visual Studio on Windows, Xcode on macOS), CMake and Ninja.
2. In this folder run:
   ```
   geode build
   ```
3. The result is `build/yourname.wave-progress-bar.geode`. Copy it into
   `Geometry Dash/geode/mods/` (or run `geode package install <file>`),
   then start the game.

## Option B: build with GitHub (no local setup)

1. Create a GitHub repository and push this folder to it.
2. Open the **Actions** tab, run **Build Binary**, and download the
   **Build Output** artifact. It contains the `.geode` file for each platform.

## Using it

- Turn on **Show Progress Bar** in GD's settings.
- Enter a level. Mod options are in Geode > Wave Progress Bar and apply the next time you enter a level.

| Setting | What it does |
|---|---|
| Hide Original Fill | Hides the green fill so only the wave shows |
| Wave Icon Scale | Size of the wave icon |
| Zigzag Height | Height of the zigzag relative to the bar |
| Zigzag Style | Classic (even) or Chaotic (uneven, same pattern per level) |
| Trail Thickness | Line thickness |
| Trail Length (%) | Full trail, or a short tail behind the wave |
| Trail Color | Use your primary or secondary icon color |

## How it works

- `PlayLayer::setupHasCompleted` creates a `WaveBar` node as a child of the progress bar
  (`m_progressBar`), so it moves, scales and hides together with the bar.
- `PlayLayer::updateProgressbar` runs every frame and moves the wave to the current percent.
- The zigzag is a list of turning points with every segment at a slope of exactly 1, so the
  wave always moves at 45 degrees. The icon is rotated to face up or down along it.
- The icon is a `SimplePlayer` set to the wave icon from `GameManager`, with your colors and glow.

## If something looks off

This code was written against Geode's documented API but has not been compiled or run in-game.
If the wave sits in the wrong spot or has the wrong size, check the constants at the top of
`src/main.cpp`:

- `BAR_PADDING_X`: shrink or grow the horizontal margin of the wave.
- `BAR_USABLE_HEIGHT` or the "Zigzag Height" setting: change how tall the zigzag is.
- `BAR_FALLBACK_WIDTH` / `BAR_FALLBACK_HEIGHT`: used if the bar texture can't be measured.

If the build fails with an unknown member or function name, check the current Geode bindings for
`GameManager` (icon and color getters) and `SimplePlayer`, since names can change between versions.
