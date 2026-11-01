#!/usr/bin/env bash
# Emulator end-to-end test of the RetroArch integration (see installer-e2e.yml).
# Needs a running emulator (adb), RA_APK = RetroArch website build (aarch64 APK, which also
# carries x86_64 libraries) and CORE_SO = the Gromada core built for the emulator ABI.
# Run from android/installer.
set -euo pipefail
RA=com.retroarch.aarch64
APP=io.github.antoniodorazio.gromada.installer
OUT=${OUT:-e2e-out}
mkdir -p "$OUT"
fail() { echo "E2E FAILED: $*"; exit 1; }

adb wait-for-device
adb shell getprop ro.build.version.release
adb install -r -g "$RA_APK"
adb shell dumpsys package "$RA" | grep -E "versionName|targetSdk" | head -3

# RetroArch's first start (its own launcher) creates retroarch.cfg and /sdcard/RetroArch.
adb shell pm grant "$RA" android.permission.READ_EXTERNAL_STORAGE || true
adb shell pm grant "$RA" android.permission.WRITE_EXTERNAL_STORAGE || true
adb shell monkey -p "$RA" -c android.intent.category.LAUNCHER 1
sleep 30
adb shell am force-stop "$RA"

# Fake game folder (no game data in CI: the core must fail to load fw.res, cleanly).
adb shell mkdir -p /sdcard/GromadaE2E/core
adb push "$CORE_SO" /sdcard/GromadaE2E/core/gromada_libretro_android.so
adb shell "echo fake > /sdcard/GromadaE2E/gromada.exe"

# Install the app under test first so that all-files access can be granted.
./gradlew --no-daemon :app:installDebug :app:installDebugAndroidTest
adb shell appops set "$APP" MANAGE_EXTERNAL_STORAGE allow
T="$APP.RetroArchE2ETest"
RUNNER="$APP.test/androidx.test.runner.AndroidJUnitRunner"
# run one test method; its logcat goes to $OUT/<method>.log
step() {
  adb logcat -c
  adb shell am instrument -w -r -e class "$T#$1" "$RUNNER" > "$OUT/$1.txt"
  sleep 2
  adb logcat -d > "$OUT/$1.log"
  grep -q "OK (1 test)" "$OUT/$1.txt" || { cat "$OUT/$1.txt"; fail "$1"; }
  echo "--- $1: RetroArch/ActivityManager log"
  grep -E "ActivityTaskManager: START|Start proc .*retroarch|Process .*retroarch.*(died|has died)|sideload|RetroArch: \[ENV\] (Config|Libretro|Auto-start)|RetroArch.*(\[Core\]|\[Content\]|Gromada|fw.res|rror)|AndroidRuntime|FATAL" "$OUT/$1.log" | cut -c1-260 | head -60 || true
}
step a_detectsWebsiteBuild
step b_writesPlaylistWithAllFilesAccess
adb shell am force-stop "$RA"
step c_sideloadsCoreAndStartsGame
adb shell am force-stop "$RA"
step d_launchesGame
step e_launchesGameWhileRetroArchRuns

echo "--- core copied into RetroArch's private folder?"
adb root >/dev/null; sleep 3
adb shell ls -l "/data/data/$RA/cores/" | tee "$OUT/cores.txt"
want=$(sha256sum "$CORE_SO" | cut -d' ' -f1)
got=$(adb shell sha256sum "/data/data/$RA/cores/gromada_libretro_android.so" | cut -d' ' -f1)
[ "$want" = "$got" ] || fail "core not sideloaded (sha256 $got != $want)"
echo "core sideloaded: sha256 $got"

echo "--- playlist written by the installer"
adb shell cat /sdcard/RetroArch/playlists/Gromada.lpl | tee "$OUT/Gromada.lpl"

echo "--- launch extras received by RetroArch (sideload+start, then direct launch)"
for t in c_sideloadsCoreAndStartsGame d_launchesGame; do
  grep -F "[ENV] Libretro path: \"/data/user/0/$RA/cores/gromada_libretro_android.so\"" "$OUT/$t.log" \
    || fail "$t: RetroArch did not get LIBRETRO"
  grep -F "[ENV] Auto-start game \"/sdcard/GromadaE2E/gromada.exe\"" "$OUT/$t.log" \
    || fail "$t: RetroArch did not get ROM"
  grep -F "[ENV] Config file: \"/storage/emulated/0/Android/data/$RA/files/retroarch.cfg\"" "$OUT/$t.log" \
    || fail "$t: RetroArch did not get CONFIGFILE"
done
echo "--- launch while RetroArch is running (informational)"
grep -cF "[ENV] Auto-start game" "$OUT/e_launchesGameWhileRetroArchRuns.log" || true
echo "E2E OK"
