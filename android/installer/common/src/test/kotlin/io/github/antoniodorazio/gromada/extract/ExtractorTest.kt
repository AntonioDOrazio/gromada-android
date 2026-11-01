package io.github.antoniodorazio.gromada.extract

import org.junit.jupiter.api.Assumptions.assumeTrue
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.io.TempDir
import java.io.ByteArrayInputStream
import java.io.File
import java.io.RandomAccessFile
import java.security.MessageDigest
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

class CueSheetTest {
    private val gromadaLike = """
        FILE "GROMADA.bin" BINARY
          TRACK 01 MODE1/2352
            INDEX 01 00:00:00
          TRACK 02 AUDIO
            INDEX 01 11:43:00
          TRACK 03 AUDIO
            INDEX 01 16:41:44
    """.trimIndent()

    @Test
    fun msf() {
        assertEquals(0, CueSheet.parseMsf("00:00:00"))
        assertEquals((11 * 60 + 43) * 75L, CueSheet.parseMsf("11:43:00"))
        assertEquals(75L * 60 * 16 + 41 * 75 + 44, CueSheet.parseMsf("16:41:44"))
        assertFailsWith<CueException> { CueSheet.parseMsf("00:60:00") }
        assertFailsWith<CueException> { CueSheet.parseMsf("00:00:75") }
    }

    @Test
    fun singleFileExtentsCutAtIndex01() {
        val cue = CueSheet.parse(gromadaLike)
        assertEquals(listOf(1, 2, 3), cue.tracks.map { it.number })
        val size = 2352L * (CueSheet.parseMsf("16:41:44") + 1000)
        val ext = cue.extents { size }
        assertEquals(0L, ext[0].byteOffset)
        assertEquals(52725L * 2352, ext[0].byteLength)
        assertEquals(52725L * 2352, ext[1].byteOffset)
        assertEquals((CueSheet.parseMsf("16:41:44") - 52725) * 2352, ext[1].byteLength)
        assertEquals(1000L * 2352, ext[2].byteLength)
    }

    @Test
    fun multiFileWithPregaps() {
        val cue = CueSheet.parse(
            """
            REM comment
            FILE "Game (Track 1).bin" BINARY
              TRACK 01 MODE2/2352
                INDEX 01 00:00:00
            FILE "Game (Track 2).bin" BINARY
              TRACK 02 AUDIO
                INDEX 00 00:00:00
                INDEX 01 00:02:00
              TRACK 03 AUDIO
                INDEX 00 00:10:00
                INDEX 01 00:12:00
            """.trimIndent()
        )
        assertEquals(listOf("Game (Track 1).bin", "Game (Track 2).bin"), cue.files.map { it.name })
        val ext = cue.extents { if (it.name.contains("1")) 2352L * 100 else 2352L * 75 * 20 }
        assertEquals(2352L * 100, ext[0].byteLength)
        assertEquals(2352L * 150, ext[1].byteOffset)
        assertEquals(2352L * 750, ext[1].byteLength) // up to the next INDEX 01
        assertEquals(2352L * 900, ext[2].byteOffset)
        assertEquals(2352L * 600, ext[2].byteLength)
    }

    @Test
    fun rejectsBrokenSheets() {
        assertFailsWith<CueException> { CueSheet.parse("TRACK 01 AUDIO") }
        assertFailsWith<CueException> { CueSheet.parse("FILE \"a.bin\" BINARY\n TRACK 01 AUDIO\n INDEX 00 00:00:00") }
        assertFailsWith<CueException> { CueSheet.parse("") }
    }
}

class FlacEncoderTest {
    private fun roundTrip(pcm: ByteArray, bigEndian: Boolean = false, blockSize: Int = 4096): FlacTestDecoder {
        val frames = pcm.size / 4L
        val tmp = File.createTempFile("enc", ".flac")
        try {
            val md5 = RandomAccessFile(tmp, "rw").use { FlacEncoder(blockSize = blockSize).encode(ByteArrayInputStream(pcm), frames, it, bigEndian) }
            val dec = FlacTestDecoder(tmp.readBytes())
            val samples = dec.decode()
            val le = if (bigEndian) ByteArray(pcm.size) { pcm[it xor 1] } else pcm
            val expected = ShortArray(le.size / 2) { ((le[2 * it].toInt() and 0xFF) or (le[2 * it + 1].toInt() shl 8)).toShort() }
            assertContentEquals(expected, samples)
            assertContentEquals(MessageDigest.getInstance("MD5").digest(le), md5)
            assertEquals(frames, dec.info.total)
            assertEquals(blockSize, dec.info.minBlock)
            assertEquals(blockSize, dec.info.maxBlock)
            assertTrue(dec.info.minFrame in 1..dec.info.maxFrame)
            externalFlacTest(tmp)
            return dec
        } finally {
            tmp.delete()
        }
    }

    /** Cross-check with the reference decoder when the `flac` tool is installed. */
    private fun externalFlacTest(file: File) {
        val ok = try {
            ProcessBuilder("flac", "-t", "-s", file.path).redirectErrorStream(true).start().let { p ->
                p.inputStream.readBytes(); p.waitFor()
            }
        } catch (e: java.io.IOException) {
            return // not installed
        }
        assertEquals(0, ok, "flac -t failed")
    }

    private fun stereo(frames: Int, f: (Int) -> Pair<Int, Int>): ByteArray {
        val out = ByteArray(frames * 4)
        for (i in 0 until frames) {
            val (l, r) = f(i)
            out[4 * i] = l.toByte(); out[4 * i + 1] = (l shr 8).toByte()
            out[4 * i + 2] = r.toByte(); out[4 * i + 3] = (r shr 8).toByte()
        }
        return out
    }

    @Test fun silence() { roundTrip(stereo(10000) { 0 to 0 }) }
    @Test fun constantNonZero() { roundTrip(stereo(4096) { 1234 to -77 }) }
    @Test fun extremes() { roundTrip(stereo(9000) { i -> if (i % 2 == 0) 32767 to -32768 else -32768 to 32767 }) }
    @Test fun fullScaleNoise() {
        val rnd = java.util.Random(1)
        roundTrip(stereo(8192 + 5) { rnd.nextInt(65536) - 32768 to rnd.nextInt(65536) - 32768 })
    }
    @Test fun music() { roundTrip(Synthetic.pcm(44100 * 2 + 333, 7)) }
    @Test fun identicalChannels() { roundTrip(stereo(20000) { i -> val v = (8000 * Math.sin(i / 30.0)).toInt(); v to v }) }
    @Test fun tinyStreams() { for (n in listOf(1, 2, 3, 5, 17, 255, 256, 257)) roundTrip(Synthetic.pcm(n, n)) }
    @Test fun bigEndianInput() { roundTrip(Synthetic.pcm(5000, 3), bigEndian = true) }
    @Test fun otherBlockSizes() {
        roundTrip(Synthetic.pcm(10000, 4), blockSize = 1152)
        roundTrip(Synthetic.pcm(10000, 5), blockSize = 1000)
    }
    @Test fun manyFramesUseMultiByteFrameNumbers() {
        val dec = roundTrip(Synthetic.pcm(200 * 64 + 1, 9), blockSize = 64)
        assertEquals(201, dec.frames)
    }
}

class ImageAndCabinetTest {
    private val files = listOf(
        Synthetic.CabEntry("", "gromada.exe", ByteArray(70000) { (it * 31 % 251).toByte() }),
        Synthetic.CabEntry("", "fw.res", ByteArray(200_000) { (it / 1000).toByte() }),
        Synthetic.CabEntry("", "fw.ini", "[fw]\r\n".toByteArray(), compressed = false),
        Synthetic.CabEntry("maps", "MAPS.INI", "maps".toByteArray()),
        Synthetic.CabEntry("maps/levels", "01.map", ByteArray(5000) { 7 }),
        Synthetic.CabEntry("cursores", "ARROW.CUR", ByteArray(326) { it.toByte() }, compressed = false),
        Synthetic.CabEntry("", "save1", ByteArray(74)),
    )

    private fun discTree(): Synthetic.DirNode {
        val (hdr, cab) = Synthetic.installShield5(files)
        return Synthetic.DirNode("", listOf(
            Synthetic.FileNode("README.TXT", "readme".toByteArray()),
            Synthetic.DirNode("INSTALL_ITA", listOf(
                Synthetic.FileNode("DATA1.CAB", cab),
                Synthetic.FileNode("DATA1.HDR", hdr),
                Synthetic.FileNode("Setup.exe", ByteArray(10)),
            )),
        ))
    }

    @Test
    fun cabinetRoundTrip() {
        val (hdr, cab) = Synthetic.installShield5(files)
        val c = InstallShieldCab(hdr, ArrayByteSource(cab))
        assertEquals(5, c.majorVersion)
        assertEquals(listOf("Resources"), c.groups.map { it.name })
        assertEquals(files.map { listOf(it.dir, it.name).filter(String::isNotEmpty).joinToString("/") }, c.validFiles.map { it.path })
        for ((f, e) in c.validFiles.zip(files)) {
            val out = java.io.ByteArrayOutputStream()
            c.extract(f, out)
            assertContentEquals(e.content, out.toByteArray(), f.path)
        }
    }

    @Test
    fun corruptDataFailsMd5() {
        val (hdr, cab) = Synthetic.installShield5(files)
        val c = InstallShieldCab(hdr, ArrayByteSource(cab))
        val ini = c.validFiles.first { it.name == "fw.ini" } // stored, not compressed
        cab[ini.dataOffset.toInt()] = (cab[ini.dataOffset.toInt()] + 1).toByte()
        val e = assertFailsWith<java.io.IOException> { c.extract(ini, java.io.ByteArrayOutputStream()) }
        assertTrue(e.message!!.contains("MD5"))
    }

    @Test
    fun isoPrimaryAndJoliet() {
        for (joliet in listOf(false, true)) {
            val iso = Iso9660(ArrayByteSource(Synthetic.iso(discTree(), joliet)))
            assertEquals(joliet, iso.joliet)
            assertEquals("TEST", iso.volumeId)
            val setup = iso.find("install_ita/setup.EXE")!!
            assertEquals(10, setup.size)
            assertEquals(setOf("README.TXT", "INSTALL_ITA", "INSTALL_ITA/DATA1.CAB", "INSTALL_ITA/DATA1.HDR", "INSTALL_ITA/SETUP.EXE"),
                iso.walk().map { it.first.uppercase() }.toSet())
        }
    }

    @Test
    fun fullImageExtraction(@TempDir tmp: File) {
        val data = Synthetic.mode1Raw(Synthetic.iso(discTree(), joliet = true))
        val dataSectors = data.size / 2352
        val a2 = Synthetic.pcm(588 * 300, 2) // 300 sectors
        val a3 = Synthetic.pcm(588 * 200 + 0, 3)
        val bin = data + a2 + a3
        val msf = { s: Int -> "%02d:%02d:%02d".format(s / 75 / 60, s / 75 % 60, s % 75) }
        val cue = """
            FILE "disc.bin" BINARY
              TRACK 01 MODE1/2352
                INDEX 01 00:00:00
              TRACK 02 AUDIO
                INDEX 01 ${msf(dataSectors)}
              TRACK 03 AUDIO
                INDEX 01 ${msf(dataSectors + 300)}
        """.trimIndent()
        DiscImage.fromCue(cue) { name, _, _ -> assertEquals("disc.bin", name); ArrayByteSource(bin) }.use { image ->
            val out = File(tmp, "game")
            val result = GameExtractor(image).extract(out, threads = 2)
            assertEquals(listOf(2, 3), result.musicTracks)
            for (e in files) {
                val f = File(out, listOf(e.dir, e.name).filter(String::isNotEmpty).joinToString("/"))
                assertContentEquals(e.content, f.readBytes(), f.path)
            }
            assertTrue(GameExtractor.isGameFolder(out))
            for ((n, pcm) in listOf(2 to a2, 3 to a3)) {
                val samples = FlacTestDecoder(File(out, "music/track%02d.flac".format(n)).readBytes()).decode()
                assertEquals(pcm.size / 2, samples.size)
                for (i in samples.indices)
                    if (samples[i] != ((pcm[2 * i].toInt() and 0xFF) or (pcm[2 * i + 1].toInt() shl 8)).toShort())
                        error("track $n sample $i differs")
            }
            assertTrue(File(out, GameExtractor.MANIFEST).readText().contains("fw.res"))
            assertTrue(out.walk().none { it.name.endsWith(".part") })

            // A second extraction (e.g. after a broken one) refreshes the game but keeps save slots.
            File(out, "save1").writeBytes(byteArrayOf(1, 2, 3))
            File(out, "fw.res").writeBytes(byteArrayOf(9))
            GameExtractor(image).extract(out, music = false)
            assertContentEquals(byteArrayOf(1, 2, 3), File(out, "save1").readBytes())
            assertContentEquals(files.first { it.name == "fw.res" }.content, File(out, "fw.res").readBytes())
        }
    }

    @Test
    fun cancellationLeavesNoPartialFiles(@TempDir tmp: File) {
        val data = Synthetic.mode1Raw(Synthetic.iso(discTree(), joliet = false))
        DiscImage.fromDataImage(ArrayByteSource(data)).use { image ->
            var calls = 0
            assertFailsWith<java.util.concurrent.CancellationException> {
                GameExtractor(image).extract(File(tmp, "g"), cancelled = { ++calls > 3 })
            }
            assertTrue(File(tmp, "g").walk().none { it.name.endsWith(".part") })
        }
    }
}

/**
 * Optional end-to-end check on a real CD image, run when these are set:
 *   GROMADA_CUE (and GROMADA_BIN if the cue names a different .bin), GROMADA_DATA = reference
 *   game folder (as installed by the original installer). Compares every file byte for byte
 *   and every music track against the raw CD audio.
 */
class RealImageTest {
    @Test
    fun matchesReferenceInstallation(@TempDir tmp: File) {
        val cue = System.getenv("GROMADA_CUE")?.let(::File)
        val ref = System.getenv("GROMADA_DATA")?.let(::File)
        assumeTrue(cue != null && cue.isFile && ref != null && ref.isDirectory, "GROMADA_CUE/GROMADA_DATA not set")
        val bins = listOfNotNull(System.getenv("GROMADA_BIN")?.let(::File))
        val out = File(tmp, "game")
        Cli.openImage(cue!!, bins).use { image ->
            val result = GameExtractor(image).extract(out)
            assertEquals((2..12).toList(), result.musicTracks)
            val skip = setOf("music")
            val refFiles = ref!!.walk().filter { it.isFile && it.relativeTo(ref).path.substringBefore('/') !in skip }
                .associateBy { it.relativeTo(ref).path }
            val outFiles = out.walk().filter { it.isFile && it.name != GameExtractor.MANIFEST && it.relativeTo(out).path.substringBefore('/') !in skip }
                .associateBy { it.relativeTo(out).path }
            assertEquals(refFiles.keys.sorted(), outFiles.keys.sorted())
            for ((path, f) in refFiles) assertContentEquals(f.readBytes(), outFiles.getValue(path).readBytes(), path)
            for (track in image.audioTracks) {
                val samples = FlacTestDecoder(File(out, "music/track%02d.flac".format(track.number)).readBytes()).decode()
                val raw = track.data.readBytes(0, track.data.size.toInt())
                assertEquals(raw.size / 2, samples.size)
                for (i in samples.indices)
                    if (samples[i] != ((raw[2 * i].toInt() and 0xFF) or (raw[2 * i + 1].toInt() shl 8)).toShort())
                        error("track ${track.number}: sample $i differs")
            }
        }
    }
}
