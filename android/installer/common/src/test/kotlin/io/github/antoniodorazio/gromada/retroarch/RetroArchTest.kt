package io.github.antoniodorazio.gromada.retroarch

import org.junit.jupiter.api.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class RetroArchTest {
    private val website = RetroArch.Install(
        "com.retroarch.aarch64", "1.22.2", 28, false,
        "/data/user/0/com.retroarch.aarch64", "/data/app/~~x/com.retroarch.aarch64-y/base.apk", null,
    )

    @Test
    fun storageAccessByBuild() {
        assertEquals(RetroArch.StorageAccess.LEGACY, website.storage)
        assertEquals(RetroArch.StorageAccess.SCOPED, website.copy(targetSdk = 35, installer = "com.android.vending").storage)
        assertEquals(RetroArch.StorageAccess.ALL_FILES, website.copy(targetSdk = 36, requestsAllFilesAccess = true).storage)
    }

    @Test
    fun launchExtrasMatchRetroArchLauncher() {
        val e = RetroArch.launchExtras(website, "/storage/emulated/0", "/storage/emulated/0/Gromada/gromada.exe", "ime/x")
        assertEquals("/storage/emulated/0/Gromada/gromada.exe", e["ROM"])
        assertEquals("/data/user/0/com.retroarch.aarch64/cores/gromada_libretro_android.so", e["LIBRETRO"])
        assertEquals("/storage/emulated/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg", e["CONFIGFILE"])
        assertEquals("/storage/emulated/0", e["SDCARD"])
        assertEquals("/storage/emulated/0/Android/data/com.retroarch.aarch64/files", e["EXTERNAL"])
        assertEquals("/data/user/0/com.retroarch.aarch64", e["DATADIR"])
        assertEquals("ime/x", e["IME"])
    }

    @Test
    fun playlistIsValidJsonWithEscapes() {
        val text = RetroArch.playlist("/sd/Gro\"mada\\x/gromada.exe", "/data/c.so")
        assertTrue(text.contains("\"path\": \"/sd/Gro\\\"mada\\\\x/gromada.exe\""))
        assertTrue(text.contains("\"version\": \"1.5\""))
        assertTrue(text.contains("\"core_path\": \"/data/c.so\""))
        assertEquals("\"a\\u0001b\"", RetroArch.jsonString("a\u0001b"))
        // balanced braces/brackets: a cheap structural check (no JSON library on the JVM side)
        assertEquals(text.count { it == '{' }, text.count { it == '}' })
        assertEquals(text.count { it == '[' }, text.count { it == ']' })
    }
}
