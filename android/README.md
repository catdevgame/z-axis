# Android (dual-screen handhelds)

The collection's runner cross-built for arm64 (`libps2EntryRunner.so`, the
three games as the Mac app has them), started by SDL's `SDLActivity` through
`GameActivity`, with the second screen's panel (`SecondScreenPanel`). No Gradle: the SDK's build tools alone. The full notes
are in `docs/BRINGUP.md`, "Android".

| File | What it is |
|---|---|
| `AndroidManifest.xml` | the app's manifest (package `io.github.catdevgame.zaxiscollection`, label Z-Axis Collection) |
| `src/` | the Java: `GameActivity`, `SecondScreenPanel` and `Documents` (the system's documents UI for the setup's discs and the saves' file) |
| `build-ffmpeg.sh` | the minimal static FFmpeg of the Mac and Windows builds, built once with the NDK |
| `build-apk.sh` | the APK from a cross-built `libps2EntryRunner.so`, SDL's Java and ours |
| `push-game.sh` | the games' files onto the device (`files/games/<id>/`), and each game's panel `.pmap` files |

## Requirements

The SDK in brew's `android-commandlinetools` (NDK 28.2, build-tools,
platforms) as `ANDROID_HOME`, and a JDK 17 as `JAVA_HOME`.

## Building

```sh
android/build-ffmpeg.sh <ffmpeg-9.0.2.tar.xz> <work dir> <prefix>
cmake -S . -B <build dir> -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/<version>/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DANDROID_STL=c++_static \
    -DPS2X_ENABLE_FFMPEG=ON -DPS2X_FFMPEG_PREFIX=<prefix> -DPS2X_ENABLE_DEBUG_UI=OFF
ninja -C <build dir> ps2EntryRunner
android/build-apk.sh <build dir> <SDL3 source dir> <out.apk> [keystore] [icon]
```

`APP_ID=<id>` gives the APK another application id (another app, with its own files folder; the classes keep
their package), `APP_LABEL` another name under its icon, `EXTRA_LIBS` more files for its native library folder
(`lib<name>.so` each: the one app folder Android runs programs from). The app's `PS2X_APP_*` options are as in `tooling/ai/tools/make-app.sh`. An Android build is the app's flavour
(`ZAXIS_DEV_CHECKS` OFF by default there: no checkers' machinery, docs/DEVTOOLS.md "Two flavours").
`[icon]` is a square PNG or the Mac app's `AppIcon.icns` (game art: never committed).

## Setting the games up

On the device: Settings > Games, Set Up From Image (an ISO, a CHD, CSO or ZSO, or a BIN/CUE dump's BIN, or both its files) or From
Folder (a folder of the disc's files, or of disc images: every game in it is set up). The system's documents UI
gives the app the files it may read; the app keeps that permission, so Repair From reads the same disc later.

## Moving the saves

Settings > Games, Saves and Settings: Export to a File writes every game's saves and settings (not the disc files,
logs or caches) into one zip where the player puts it; Import From a File takes such a zip into this app. The app of
the earlier id (`rest.src.cat.aggressiveinline`) is another app to Android: an update of it built with
`APP_ID=rest.src.cat.aggressiveinline` and its own key exports, the new app imports.

## Running a game from a computer

Install the APK and start it once (Android makes its files folder), then

```sh
ANDROID_SERIAL=<serial> android/push-game.sh [--panel-only] <Resources folder> <work folder>
```

sends the folders of the Mac app's `Contents/Resources` to
`/sdcard/Android/data/<id>/files/` (`APP_ID`, the default id's otherwise): a collection's
`games/<id>/{disc,...}` as they are, with each game's panel files made here
(`games/<id>/panel`: Aggressive Inline's and Dave Mirra BMX 2's). The app opens
on its menu; `PS2X_APP_GAME=<id>` in `files/ps2x-env.txt` boots one game.
Settings, saves and logs are in `files/support` (`support/games/<id>/`).
