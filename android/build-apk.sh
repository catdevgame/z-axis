#!/bin/sh
# Makes the Android APK from a cross-built libps2EntryRunner.so, with the SDK's
# build tools alone: no Gradle, and nothing here is game data.
#
#   ANDROID_HOME=<sdk> JAVA_HOME=<jdk 17> [APP_ID=<id>] [APP_LABEL=<name>] [EXTRA_LIBS="<file>..."] \
#       android/build-apk.sh <build dir> <SDL3 source dir> <out.apk> [keystore] [icon]
#
# APP_ID is the application id (the manifest's io.github.catdevgame.zaxiscollection
# by default; the classes keep their package): another id is another app, with
# its own files folder. EXTRA_LIBS are more files for the native library
# folder (lib<name>.so each), the one app folder Android lets a program run from.
#
# The keystore is made on first use (a debug key: the APK is for your own
# device). The APK is not committed, nor is the icon: [icon] is a square PNG,
# the Mac app's baseline/icon/square.png (game art, in no commit), resized here;
# without it the APK has the system's plain icon.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=$1
SDL=$2
OUT=$3
KEY=${4:-$BUILD/android-debug.keystore}
ICON=${5:-}
# (runtime/ since tools17; ps2xRuntime/ in a build dir from before)
SO=$BUILD/runtime/libps2EntryRunner.so
[ -f "$SO" ] || SO=$BUILD/ps2xRuntime/libps2EntryRunner.so
TOOLS=$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -1)
JAR=$(ls -d "$ANDROID_HOME"/platforms/android-* | sort -V | tail -1)/android.jar
STAGE=$BUILD/apk
JAVAC=$JAVA_HOME/bin/javac

[ -f "$SO" ] || { echo "no $SO" >&2; exit 1; }
rm -rf "$STAGE"
mkdir -p "$STAGE/classes" "$STAGE/lib/arm64-v8a"

"$JAVAC" -nowarn -Xlint:none -source 8 -target 8 -cp "$JAR" -d "$STAGE/classes" \
    "$SDL"/android-project/app/src/main/java/org/libsdl/app/*.java \
    $(find "$HERE/src" -name '*.java')
"$TOOLS/d8" --release --min-api 33 --lib "$JAR" --output "$STAGE" $(find "$STAGE/classes" -name '*.class')

cp "$HERE/AndroidManifest.xml" "$STAGE/AndroidManifest.xml"
COMPILED=
if [ -n "$ICON" ]; then
    # The Mac app's icon (Resources/AppIcon.icns) does too: its largest picture.
    case $ICON in
    *.icns)
        sips -s format png "$ICON" --out "$STAGE/icon-source.png" >/dev/null
        ICON=$STAGE/icon-source.png
        ;;
    esac
    # An adaptive icon: launchers show the middle 72 of its 108 dp and round
    # it off, so the picture goes in the middle 66 dp over its own corner colour.
    sips -c 4 4 --cropOffset 2 2 -s format bmp "$ICON" --out "$STAGE/corner.bmp" >/dev/null
    BACK=$(tail -c 3 "$STAGE/corner.bmp" | od -An -tx1 | awk '{print toupper($3 $2 $1)}')
    RES=$STAGE/res
    mkdir -p "$RES/mipmap-xxxhdpi" "$RES/mipmap-anydpi-v26" "$RES/values"
    sips -s format png -z 264 264 "$ICON" --out "$STAGE/icon.png" >/dev/null
    sips -p 432 432 --padColor "$BACK" "$STAGE/icon.png" --out "$RES/mipmap-xxxhdpi/icon_picture.png" >/dev/null 2>&1
    echo "<resources><color name=\"icon_back\">#$BACK</color></resources>" >"$RES/values/icon.xml"
    cat >"$RES/mipmap-anydpi-v26/icon.xml" <<'ADAPTIVE'
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/icon_back" />
    <foreground android:drawable="@mipmap/icon_picture" />
</adaptive-icon>
ADAPTIVE
    "$TOOLS/aapt2" compile -o "$STAGE/res.zip" --dir "$RES"
    COMPILED=$STAGE/res.zip
    sed -i '' 's/<application$/& android:icon="@mipmap\/icon"/' "$STAGE/AndroidManifest.xml"
fi
if [ -n "${APP_LABEL:-}" ]; then
    sed -i '' "s|android:label=\"Z-Axis Collection\"|android:label=\"$APP_LABEL\"|g" "$STAGE/AndroidManifest.xml"
fi
RENAME=
[ -n "${APP_ID:-}" ] && RENAME="--rename-manifest-package $APP_ID"
"$TOOLS/aapt2" link -o "$STAGE/base.apk" -I "$JAR" --manifest "$STAGE/AndroidManifest.xml" \
    --min-sdk-version 33 --target-sdk-version 33 $RENAME $COMPILED
cp "$SO" "$STAGE/lib/arm64-v8a/"
EXTRA=
for LIB in ${EXTRA_LIBS:-}; do
    case $(basename "$LIB") in
    lib*.so) ;;
    *) echo "$LIB: the native library folder takes lib<name>.so files only" >&2; exit 1 ;;
    esac
    cp "$LIB" "$STAGE/lib/arm64-v8a/"
    EXTRA="$EXTRA lib/arm64-v8a/$(basename "$LIB")"
done
"$ANDROID_HOME/ndk/$(ls "$ANDROID_HOME/ndk" | sort -V | tail -1)/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" \
    --strip-debug "$STAGE/lib/arm64-v8a/libps2EntryRunner.so"
# The app's own font (runtime/resources/fonts: Michroma, OFL 1.1, the TTF unchanged with its licence) as assets: the
# menu reads assets/fonts/Michroma-Regular.ttf through SDL before Aggressive Inline's disc is set up.
mkdir -p "$STAGE/assets/fonts"
cp "$HERE/../runtime/resources/fonts/Michroma-Regular.ttf" "$HERE/../runtime/resources/fonts/Michroma-OFL.txt" "$STAGE/assets/fonts/"
(cd "$STAGE" && zip -q -1 base.apk classes.dex lib/arm64-v8a/libps2EntryRunner.so assets/fonts/Michroma-Regular.ttf \
    assets/fonts/Michroma-OFL.txt $EXTRA)
"$TOOLS/zipalign" -f 4 "$STAGE/base.apk" "$STAGE/aligned.apk"

if [ ! -f "$KEY" ]; then
    "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$KEY" -storepass android -keypass android \
        -alias debug -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Aggressive Inline Debug"
fi
"$TOOLS/apksigner" sign --ks "$KEY" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT" "$STAGE/aligned.apk"
ls -l "$OUT"
