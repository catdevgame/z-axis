# Android (dual-screen handhelds)

The collection for dual-screen Android handhelds: all three games, as the Mac app has them, with the game on the
top screen and a panel on the bottom one (the level's map, goals and tricks). Each release carries an APK with no game
code in it (the app makes each game's code on the device when it sets the game up); or build the APK yourself and put
it on your own device.

Under the hood it is the collection's runner cross-built for arm64 (`libzx-runner.so`), started by SDL's
`SDLActivity` through `GameActivity`; the bottom screen is `SecondScreenPanel`. The native side is
`platform/android/android_app.cpp` and `android_documents.cpp`, behind `__ANDROID__`, so the Mac and Windows
builds are unchanged. No Gradle: the SDK's build tools alone.

## Setting the games up

On the device: the first start opens Settings > Games. Find Discs picks a folder (your ROMs folder: every game's PS2
disc in it and its folders is found, and the games' Xbox and GameCube discs for the extras), and Set Up Games sets
up the games ticked. A single game: Set Up From Image (an ISO, a CHD, CSO or ZSO, or a BIN/CUE dump's BIN, or both
its files) or From Folder (a folder of the disc's files, or of disc images: every game in it is set up). The
system's documents UI gives the app the files it may read (content:// URIs, read through the descriptors the
content resolver opens: `game_setup.h` DocumentAccess); the app keeps that permission, so Repair From reads the
same disc later. It can read no other path. Setting a game up takes a while on a handheld, about 8 to 19 minutes a
game, most of it compiling the game's code.

## Moving the saves

Settings > Games, Saves and Settings: Export to a File writes every game's saves and settings (not the disc files,
logs or caches) into one zip where the player puts it; Import From a File takes such a zip into this app
(`collection/save_transfer`).

## The files

| File | What it is |
|---|---|
| `AndroidManifest.xml` | the app's manifest (package `io.github.catdevgame.zaxiscollection`, label Z-Axis Collection) |
| `src/` | the Java: `GameActivity`, `SecondScreenPanel` and `Documents` (the system's documents UI for the setup's discs and the saves' file) |
| `build-ffmpeg.sh` | the minimal static FFmpeg of the Mac and Windows builds, built once with the NDK |
| `build-apk.sh` | the APK from a cross-built `libzx-runner.so`, SDL's Java and ours |
| `push-game.sh` | the games' files onto the device (`files/games/<id>/`), and each game's panel `.pmap` files |

## The game code, made on the device

The APK carries no game code. As on Mac and Windows (`collection/game_code.h`), setting a game up
recompiles the disc's boot ELF, compiles the result and links it into the game's module, with the pinned LLVM
(`third_party/toolchain/llvm.lock`, `archive-android-arm64`: `build-llvm.sh android-arm64`, clang and lld for AArch64
alone). Android runs no program an app wrote or unpacked itself, so the compiler, the linker and the recompiler are
the APK's native libraries (`libclang.so`, `liblld.so`, `libps2_recomp.so`, run from the app's native library
folder), and the rest of the tools (the module's headers, the compiler's, each game's recompiler inputs) are its
`assets/game-code-tools`, unpacked into the internal storage at the first start of each APK. The modules are kept in
the internal storage too (`game-code/<id>`): Android loads no code from the external files folder.

## Requirements

The SDK in brew's `android-commandlinetools` (NDK 28.2, build-tools, platforms) as `ANDROID_HOME`, and a JDK 17
as `JAVA_HOME`.

## Building

```sh
platform/android/build-ffmpeg.sh <ffmpeg-9.0.2.tar.xz> <work dir> <prefix>
cmake -S . -B <build dir> -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/<version>/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DANDROID_STL=c++_static \
    -DZX_ENABLE_FFMPEG=ON -DZX_FFMPEG_PREFIX=<prefix> \
    <the collection's arguments as platform/mac/make-app.sh gives them: ZX_APP_GAMES, ZX_BIND_DIR,
     the three games' generated and VU1 folders>
ninja -C <build dir> zx-runner
platform/android/build-apk.sh <build dir> <SDL3 source dir> <out.apk> [keystore] [icon]
```

The shaders are the HLSL compiled through the NDK's glslc (`GS_SPIRV=1`). An Android build is the app's flavour
(`ZX_DEV_CHECKS` OFF by default there: no checkers' machinery, docs/DEVTOOLS.md "Two flavours").

The app players install (no game code, the game code's tools in it) adds `-DZX_GAME_MODULES=shared
-DZX_ANDROID_RECOMPILER=ON` to the configure and `ps2_recomp` to the build, then stages the tools from the unpacked
Android LLVM archive and gives them to the APK:

```sh
ninja -C <build dir> zx-runner ps2_recomp
python3 third_party/toolchain/stage_game_code_tools.py <tools dir> --llvm <unpacked 23.1.5-android-arm64 archive> \
    --recompiler <build dir>/recompiler/ps2xRecomp/ps2_recomp --sse2neon <build dir>/_deps/sse2neon-src/sse2neon.h \
    --tree-games . --android $ANDROID_HOME/ndk/<version>/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf
GAME_CODE_TOOLS=<tools dir> platform/android/build-apk.sh <build dir> <SDL3 source dir> <out.apk> ...
```

The version (the menu's, the system's versionName and versionCode) is the tree's `VERSION.txt`: the build takes it
unless configured with another `-DZX_APP_VERSION`, and `build-apk.sh` takes the native library's (`APP_VERSION`
must match it); a `RELEASE=1` APK is the tree's version or none.

A release's APK is built so from a clone of the public repository (no game files, none of the workshop's), with
`RELEASE=1 KS_PASS=file:<the key's password file> KS_ALIAS=zaxis-collection` and the collection's release keystore
(kept apart from every repository) as the keystore: no profileable element, packed at zip's highest level.

`APP_ID=<id>` gives the APK another application id (another app, with its own files folder; the classes keep
their package), `APP_LABEL` another name under its icon (`TEST=1`: both for a test build, the id
`io.github.catdevgame.zaxiscollection.test` and the name Z-Axis Test, so it installs beside the app you play with,
with its own games, saves and settings; debuggable, for `adb shell run-as`), `EXTRA_LIBS` more files for its native library folder
(`lib<name>.so` each: the one app folder Android runs programs from). `[icon]` is a square PNG or the Mac app's
`AppIcon.icns` (game art: never committed). Without `RELEASE=1`, a keystore that does not exist yet is made on
first use as a debug key (the default one is in the build dir).

## Running a game from a computer

Install the APK and start it once (Android makes its files folder), then

```sh
ANDROID_SERIAL=<serial> platform/android/push-game.sh [--panel-only] <Resources folder> <work folder>
```

sends the folders of the Mac app's `Contents/Resources` to `/sdcard/Android/data/<id>/files/` (`APP_ID`, the default
id's otherwise) as a tar stream (`adb push` cannot make subfolders there): a collection's `games/<id>/{disc,...}` as
they are, with each game's panel files made here by `panel_map.py` (`games/<id>/panel`: the three games' maps,
goals and trick lists). `--panel-only` does just the panels. The app opens on its menu; `ZX_APP_GAME=<id>` in
`files/zx-env.txt` boots one game. Settings, saves and logs are in `files/support` (`support/games/<id>/`).

`files/zx-env.txt` takes the developer switches, one `NAME=value` a line. `ZX_GS_SCALE` counts only with
`ZX_GS_BACKEND=gpu` set too (otherwise the settings win, as on Mac and Windows); here `ZX_GS_SCALE=<x>x<y>`
sets both. `ZX_CONTROL=<absolute path under files>` works (the game's working directory is its support folder,
so a relative one lands there), with `cmd` and `out` reached over `adb shell`; it serves the first game booted in
the process only, and it reads `cmd` every vsync through the sdcard's FUSE, so leave it out of frame-time
measurements. `ZX_DEV_START_LEVEL` and `ZX_DEV_START_SKATE` reach and skate a level without it.
`ZX_SECOND_SCREEN=off` leaves a second display unused (GameActivity reads it from the same file): a dual-screen
device runs as a single-screen one, with the bottom screen over the game by its shortcut (M, L3+R2).

## How it runs on the device

- The second screen: `SecondScreenPanel` (a Presentation, not focusable, so the pad stays with the game and B cannot
  cancel it), a native painter at about 20 Hz in the surface's CPU buffer, off the game's frames and GPU. The game
  thread takes one snapshot a vsync while the panel is up. The panel follows the game that runs
  (`panel::setGameFiles`, set by the app as a game boots). The pad drives the menus; a touch goes to the bottom
  screen (the collection menu's there, or the game's panel).
- In the background (from `onStop`: Home, the screen off) SDL blocks the window thread inside its event pump, so the
  app's per-frame pause never runs; an SDL event watch (`pauseInBackground`) pauses the EE scheduler before the pump
  blocks. Without it the game thread draws on with nothing presenting and GPU memory grows without limit.
- Split screen stays left and right on the top screen.
- A white home pill over the immersive game is SystemUI's `SecondaryHomeHandle`, not the navigation bar: after an
  Overview gesture Launcher3 leaves a quick switch rotation set and SystemUI shows the handle over hidden bars. The
  next swipe up to Home clears it.
- A new pipeline costs about 130 ms on the GS thread, at every start: SDL's Vulkan backend passes no pipeline
  cache.
- Profiling: the manifest is `profileable` from the shell, so `simpleperf record --app
  io.github.catdevgame.zaxiscollection -f 4000 --duration 25` samples a stock device (no root; `-p PID` is
  refused); `simpleperf report --symdir <build>/runtime` symbolizes with the unstripped .so. The perf log's
  `cpu ms/vsync` line gives the game, VIF1 and GS threads' CPU (the workers are named `vif1-stage` and `gs-stage`).
