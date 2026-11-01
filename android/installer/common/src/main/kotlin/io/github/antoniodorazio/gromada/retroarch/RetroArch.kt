package io.github.antoniodorazio.gromada.retroarch

/**
 * What an external app can do with RetroArch for Android, from its source code
 * (libretro/RetroArch pkg/android, checked on tags v1.17.0 .. v1.22.2 and master):
 *
 * - Cores are loaded only from RetroArch's private `dataDir/cores/` (shared storage is
 *   mounted noexec and outside the linker namespace), which other apps cannot write.
 * - `com.retroarch.browser.debug.CoreSideloadActivity` is exported without permission:
 *   given `LIBRETRO=<path of a .so RetroArch can read>` it copies the file into
 *   `dataDir/cores/` and then starts RetroArch with that core. This automates the
 *   manual "Load Core > Install or Restore a Core".
 * - `com.retroarch.browser.retroactivity.RetroActivityFuture` is exported and takes the
 *   same string extras as RetroArch's own launcher: ROM, LIBRETRO, CONFIGFILE, IME,
 *   DATADIR, APK, SDCARD, EXTERNAL (plus REFRESH, QUITFOCUS).
 * - The website builds (com.retroarch, com.retroarch.aarch64, com.retroarch.ra32) target
 *   SDK 28 with legacy storage, so with the storage permission they read any file on
 *   shared storage. The Google Play builds (also com.retroarch / com.retroarch.aarch64
 *   "Plus") target SDK 35 without storage permissions up to 1.22.2: they cannot read a
 *   game folder or a core in shared storage. Newer builds request MANAGE_EXTERNAL_STORAGE.
 */
object RetroArch {
    /** Package names, most suitable first for an arm64 handheld. */
    val PACKAGES = listOf("com.retroarch.aarch64", "com.retroarch", "com.retroarch.ra32")

    const val ACTIVITY = "com.retroarch.browser.retroactivity.RetroActivityFuture"
    const val SIDELOAD_ACTIVITY = "com.retroarch.browser.debug.CoreSideloadActivity"
    const val CORE_FILE = "gromada_libretro_android.so"
    const val INFO_FILE = "gromada_libretro.info"
    const val CORE_NAME = "Gromada"
    const val PLAYLIST_FILE = "Gromada.lpl"

    enum class StorageAccess {
        /** targetSdk <= 29: reads shared storage once the storage permission is granted. */
        LEGACY,
        /** Requests "All files access": works if the user granted it to RetroArch. */
        ALL_FILES,
        /** Scoped storage only (Play Store builds up to 1.22.x): cannot read our files. */
        SCOPED,
    }

    data class Install(
        val packageName: String,
        val versionName: String,
        val targetSdk: Int,
        val requestsAllFilesAccess: Boolean,
        /** ApplicationInfo.dataDir, e.g. /data/user/0/com.retroarch.aarch64 */
        val dataDir: String,
        /** ApplicationInfo.sourceDir (the APK) */
        val sourceDir: String,
        val installer: String?,
    ) {
        val storage: StorageAccess
            get() = when {
                targetSdk <= 29 -> StorageAccess.LEGACY
                requestsAllFilesAccess -> StorageAccess.ALL_FILES
                else -> StorageAccess.SCOPED
            }
        val fromPlayStore: Boolean get() = installer == "com.android.vending"
        val corePath: String get() = "$dataDir/cores/$CORE_FILE"
    }

    /** RetroArch's external files dir, where its launcher keeps retroarch.cfg. */
    fun externalDir(storageRoot: String, pkg: String) = "$storageRoot/Android/data/$pkg/files"
    fun configPath(storageRoot: String, pkg: String) = externalDir(storageRoot, pkg) + "/retroarch.cfg"

    /** Default playlist folder of the website builds when internal storage is writable. */
    fun defaultPlaylistDir(storageRoot: String) = "$storageRoot/RetroArch/playlists"

    /** Extras for CoreSideloadActivity: copy [coreFile] into RetroArch's cores folder. */
    fun sideloadExtras(coreFile: String) = mapOf("LIBRETRO" to coreFile)

    /**
     * Extras to start [content] with the Gromada core, identical to what RetroArch's own
     * launcher (MainMenuActivity.startRetroActivity) passes, so the user's retroarch.cfg
     * and default folders are used.
     */
    fun launchExtras(ra: Install, storageRoot: String, content: String, ime: String?): Map<String, String> = buildMap {
        put("ROM", content)
        put("LIBRETRO", ra.corePath)
        put("CONFIGFILE", configPath(storageRoot, ra.packageName))
        if (!ime.isNullOrEmpty()) put("IME", ime)
        put("DATADIR", ra.dataDir)
        put("APK", ra.sourceDir)
        put("SDCARD", if (ra.fromPlayStore) externalDir(storageRoot, ra.packageName) else storageRoot)
        put("EXTERNAL", externalDir(storageRoot, ra.packageName))
    }

    /** A RetroArch playlist (JSON format 1.5) with one entry starting the game with our core. */
    fun playlist(content: String, corePath: String, label: String = "Gromada"): String = buildString {
        fun s(v: String) = jsonString(v)
        append("{\n")
        append("  \"version\": \"1.5\",\n")
        append("  \"default_core_path\": ${s(corePath)},\n")
        append("  \"default_core_name\": ${s(CORE_NAME)},\n")
        append("  \"label_display_mode\": 0,\n")
        append("  \"right_thumbnail_mode\": 0,\n")
        append("  \"left_thumbnail_mode\": 0,\n")
        append("  \"thumbnail_match_mode\": 0,\n")
        append("  \"sort_mode\": 0,\n")
        append("  \"items\": [\n")
        append("    {\n")
        append("      \"path\": ${s(content)},\n")
        append("      \"label\": ${s(label)},\n")
        append("      \"core_path\": ${s(corePath)},\n")
        append("      \"core_name\": ${s(CORE_NAME)},\n")
        append("      \"crc32\": \"DETECT\",\n")
        append("      \"db_name\": ${s(PLAYLIST_FILE)}\n")
        append("    }\n")
        append("  ]\n")
        append("}\n")
    }

    fun jsonString(v: String): String = buildString {
        append('"')
        for (c in v) when {
            c == '"' -> append("\\\"")
            c == '\\' -> append("\\\\")
            c < ' ' -> append("\\u%04x".format(c.code))
            else -> append(c)
        }
        append('"')
    }
}
