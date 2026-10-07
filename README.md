# Husk

<p align="center">
  <a href="https://trendshift.io/repositories/233057?utm_source=trendshift-badge&amp;utm_medium=badge&amp;utm_campaign=badge-trendshift-233057" target="_blank" rel="noopener noreferrer">
    <img src="https://trendshift.io/api/badge/trendshift/repositories/233057/daily?language=Swift" alt="Leviidev/Husk | Trendshift" width="250" height="55"/>
  </a>
</p>

[![Husk Downloads](https://img.shields.io/github/downloads/leviidev/husk/total?style=for-the-badge&color=5865F2&labelColor=111111)](https://github.com/leviidev/husk/releases)

Android app launcher for iOS.

Drop in an APK, tap it, and the Android app opens full-screen.

## Screenshots

### Games on the Translation Layer

These run straight on the iPhone through Husk's translation layer: the game's
own Android code, with no Android system booted underneath it.

<table>
  <tr>
    <td align="center"><img src="Screenshots/IMG_1576.jpeg" alt="GTA San Andreas: CJ riding a BMX through Ganton" width="100%"><br><sub><b>GTA: San Andreas</b> — riding through Ganton</sub></td>
    <td align="center"><img src="Screenshots/IMG_1572.jpeg" alt="Minecraft Dungeons: the opening cutscene over a burning castle bridge" width="100%"><br><sub><b>Minecraft Dungeons</b> — the opening cutscene</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="Screenshots/IMG_1582.jpeg" alt="Beach Buggy Racing 2: leading a race in first place" width="100%"><br><sub><b>Beach Buggy Racing 2</b> — leading a race</sub></td>
    <td align="center"><img src="Screenshots/IMG_1580.jpeg" alt="Geometry Dash: flying the ship through a lava level" width="100%"><br><sub><b>Geometry Dash</b> — ship section of a level</sub></td>
  </tr>
</table>

### The app

<table>
  <tr>
    <td align="center" width="33%"><img src="Screenshots/IMG_1573.png" alt="The Library tab on the Translation Layer side, with a grid of games" width="100%"><br><sub><b>Library</b> — your games, with Translation Layer and Emulation a swipe apart</sub></td>
    <td align="center" width="33%"><img src="Screenshots/IMG_1575.png" alt="Per-game settings for GTA: SA" width="100%"><br><sub><b>Per-game settings</b> — orientation, resolution, a clean screenshot mode and the on-screen controller</sub></td>
    <td align="center" width="33%"><img src="Screenshots/IMG_1574.png" alt="The Settings tab" width="100%"><br><sub><b>Settings</b> — JIT, Discover, performance and appearance</sub></td>
  </tr>
</table>

## JIT

Husk needs JIT, which on iOS takes an attached debugger. Use StikDebug, or
Husk's built-in StikJIT helper (iOS 26+), which on iOS 27 can pair with your
iPhone from Settings with no computer. The app walks you through it; see
[docs/06-built-in-jit.md](docs/06-built-in-jit.md).

## Builds

Every push builds an unsigned `Husk.ipa` in GitHub Actions
([build-ipa.yml](.github/workflows/build-ipa.yml)). It is attached to the run
as an artifact, ready for AltStore, SideStore or TrollStore to sign and
install. The first run builds QEMU and its dependencies from scratch, which
takes a couple of hours; after that they are cached.

## Licence

GPL-2.0-or-later. Husk links QEMU, which is GPLv2, so the shipped binary is a
combined GPLv2 work and the full source is public. It cannot go on the App
Store — both because of that and because it needs `get-task-allow` plus a
debugger attaching at runtime. See [docs/01-licensing.md](docs/01-licensing.md).