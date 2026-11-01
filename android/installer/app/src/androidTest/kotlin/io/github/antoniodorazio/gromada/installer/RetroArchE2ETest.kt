package io.github.antoniodorazio.gromada.installer

import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import io.github.antoniodorazio.gromada.retroarch.RetroArch
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.FixMethodOrder
import org.junit.Test
import org.junit.runner.RunWith
import org.junit.runners.MethodSorters
import java.io.File

/**
 * End-to-end check against the real RetroArch website build (com.retroarch.aarch64), run on
 * an emulator by e2e/run.sh, which prepares /sdcard/GromadaE2E and afterwards verifies, as
 * root, that the core landed in RetroArch's private cores folder and that RetroArch received
 * our launch extras (from its logcat). Skipped when RetroArch is not installed.
 */
@RunWith(AndroidJUnit4::class)
@FixMethodOrder(MethodSorters.NAME_ASCENDING)
class RetroArchE2ETest {
    private val ctx = InstrumentationRegistry.getInstrumentation().targetContext
    private val dir = File("/sdcard/GromadaE2E")

    private fun retroArch(): RetroArch.Install {
        val ra = Platform.findRetroArch(ctx).firstOrNull { it.packageName == "com.retroarch.aarch64" }
        assumeTrue("RetroArch (com.retroarch.aarch64) not installed", ra != null)
        return ra!!
    }

    @Test
    fun a_detectsWebsiteBuild() {
        val ra = retroArch()
        assertTrue("targetSdk ${ra.targetSdk}", ra.targetSdk <= 29)
        assertEquals(RetroArch.StorageAccess.LEGACY, ra.storage)
        assertNull(ra.installer) // installed with adb, not from Google Play
        assertTrue(ra.dataDir.endsWith("/com.retroarch.aarch64"))
    }

    @Test
    fun b_writesPlaylistWithAllFilesAccess() {
        val ra = retroArch()
        assertTrue("all files access not granted", Platform.hasAllFilesAccess(ctx))
        val playlists = File(RetroArch.defaultPlaylistDir(Platform.storageRoot))
        playlists.mkdirs()
        val file = File(playlists, RetroArch.PLAYLIST_FILE)
        file.writeText(RetroArch.playlist(File(dir, "gromada.exe").absolutePath, ra.corePath))
        assertTrue(file.length() > 0)
    }

    // e2e/run.sh runs c, d and e one by one, stopping RetroArch before c and d.

    @Test
    fun c_sideloadsCoreAndStartsGame() {
        val ra = retroArch()
        val core = File(dir, "core/${RetroArch.CORE_FILE}")
        assertTrue("missing $core", core.isFile)
        ActivityScenario.launch(MainActivity::class.java).use { s ->
            s.onActivity { Platform.sideloadCore(it, ra, core, File(dir, "gromada.exe")) }
            Thread.sleep(15_000) // RetroArch copies the core, then starts with it
        }
    }

    @Test
    fun d_launchesGame() = launch()

    @Test
    fun e_launchesGameWhileRetroArchRuns() = launch()

    private fun launch() {
        val ra = retroArch()
        ActivityScenario.launch(MainActivity::class.java).use { s ->
            s.onActivity { Platform.launchGame(it, ra, File(dir, "gromada.exe")) }
            Thread.sleep(15_000)
        }
    }
}
