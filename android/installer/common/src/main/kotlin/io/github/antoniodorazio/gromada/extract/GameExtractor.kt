package io.github.antoniodorazio.gromada.extract

import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.util.concurrent.CancellationException
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.atomic.AtomicLong

/** Where the game files are on the CD. */
sealed class GameSource {
    abstract val description: String

    /** InstallShield 5+ installer, e.g. INSTALL_ITA/DATA1.HDR + DATA1.CAB (Italian CD). */
    class Cabinet(val dir: String, val cab: InstallShieldCab) : GameSource() {
        override val description get() = "InstallShield $dir (v${cab.majorVersion}, ${cab.validFiles.size} file)"
    }

    /** Game files stored as plain files on the CD (directory containing fw.res). */
    class PlainFiles(val dir: String, val files: List<Pair<String, IsoEntry>>) : GameSource() {
        override val description get() = "file sul CD in /${dir}"
    }
}

enum class Stage { ANALYZE, GAME_FILES, MUSIC, DONE }

fun interface ProgressListener {
    fun onProgress(stage: Stage, done: Long, total: Long, detail: String)
}

data class ExtractedFile(val path: String, val size: Long, val md5: String)

data class ExtractionResult(
    val source: String,
    val files: List<ExtractedFile>,
    val musicTracks: List<Int>,
)

/**
 * Turns a Gromada CD image into the game folder the libretro core expects:
 * gromada.exe, fw.res, fw.ini, maps/, avi/, cursores/, save1..5 (everything the
 * installer would copy) plus music/trackNN.flac for the CD audio tracks 2-12.
 *
 * Pure JVM code: used by the Android installer and by the desktop CLI ([Cli]).
 */
class GameExtractor(private val image: DiscImage, private val log: (String) -> Unit = {}) {
    val iso: Iso9660 by lazy {
        val track = image.dataTrack ?: throw IOException("L'immagine non contiene una traccia dati")
        Iso9660(DataTrack(track.data, track.sectorSize))
    }

    /** Finds the game on the CD: an InstallShield cabinet holding fw.res, or plain files. */
    fun locateGame(): GameSource {
        val entries = iso.walk(maxDepth = 4).toList()
        val byDir = entries.groupBy({ it.first.substringBeforeLast('/', "") }, { it })
        val candidates = mutableListOf<GameSource.Cabinet>()
        for ((dir, list) in byDir.toSortedMap()) {
            val hdr = list.firstOrNull { it.first.substringAfterLast('/').equals("DATA1.HDR", true) } ?: continue
            val cab = list.firstOrNull { it.first.substringAfterLast('/').equals("DATA1.CAB", true) } ?: continue
            try {
                val header = iso.open(hdr.second).readBytes(0, hdr.second.size.toInt())
                val c = InstallShieldCab(header, iso.open(cab.second))
                val hasRes = c.validFiles.any { it.name.equals("fw.res", true) }
                log("Trovato installer $dir: ${c.validFiles.size} file, gruppi ${c.groups.joinToString { it.name }}" +
                    if (hasRes) "" else " (senza fw.res, ignorato)")
                if (hasRes) candidates += GameSource.Cabinet(dir, c)
            } catch (e: IOException) {
                log("Ignoro $dir: ${e.message}")
            }
        }
        if (candidates.isNotEmpty()) return candidates.first()

        val res = entries.firstOrNull { !it.second.isDirectory && it.first.substringAfterLast('/').equals("fw.res", true) }
        if (res != null) {
            val dir = res.first.substringBeforeLast('/', "")
            val prefix = if (dir.isEmpty()) "" else "$dir/"
            val files = entries.filter { !it.second.isDirectory && it.first.startsWith(prefix) }
                .map { it.first.removePrefix(prefix) to it.second }
            return GameSource.PlainFiles(dir, files)
        }
        throw IOException("Gromada non trovato nell'immagine: nessun installer con fw.res né file del gioco")
    }

    /**
     * Extracts into [outDir]. Files are written as "name.part" and renamed when complete,
     * so an interrupted run never leaves a truncated file under its final name.
     */
    fun extract(
        outDir: File,
        music: Boolean = true,
        threads: Int = Runtime.getRuntime().availableProcessors().coerceIn(1, 4),
        progress: ProgressListener = ProgressListener { _, _, _, _ -> },
        cancelled: () -> Boolean = { false },
        /** Existing files for which this returns true are left untouched (save slots by default). */
        keepExisting: (String) -> Boolean = { isSaveSlot(it) },
    ): ExtractionResult {
        progress.onProgress(Stage.ANALYZE, 0, 1, "Analisi dell'immagine")
        val source = locateGame()
        log("Sorgente: ${source.description}")
        if (!outDir.isDirectory && !outDir.mkdirs()) throw IOException("Impossibile creare $outDir")

        val files = mutableListOf<ExtractedFile>()
        when (source) {
            is GameSource.Cabinet -> {
                val list = source.cab.validFiles
                val total = list.sumOf { it.expandedSize }
                var done = 0L
                for (f in list) {
                    checkCancel(cancelled)
                    progress.onProgress(Stage.GAME_FILES, done, total, f.path)
                    if (keepExisting(f.path) && File(outDir, f.path).exists()) {
                        log("Mantengo ${f.path} (già presente)")
                        done += f.expandedSize
                        continue
                    }
                    val md5 = writeAtomically(File(outDir, f.path)) { out ->
                        source.cab.extract(f, out) { n ->
                            done += n
                            progress.onProgress(Stage.GAME_FILES, done, total, f.path)
                            checkCancel(cancelled)
                        }
                    }
                    files += ExtractedFile(f.path, f.expandedSize, md5.toHex())
                }
            }
            is GameSource.PlainFiles -> {
                val total = source.files.sumOf { it.second.size }
                var done = 0L
                for ((path, entry) in source.files) {
                    checkCancel(cancelled)
                    if (keepExisting(path) && File(outDir, path).exists()) {
                        done += entry.size
                        continue
                    }
                    val md5 = writeAtomically(File(outDir, path)) { out ->
                        val digest = MessageDigest.getInstance("MD5")
                        iso.open(entry).openStream().use { input ->
                            val buf = ByteArray(65536)
                            while (true) {
                                val n = input.read(buf)
                                if (n < 0) break
                                out.write(buf, 0, n); digest.update(buf, 0, n)
                                done += n
                                progress.onProgress(Stage.GAME_FILES, done, total, path)
                            }
                        }
                        digest.digest()
                    }
                    files += ExtractedFile(path, entry.size, md5.toHex())
                }
            }
        }

        val tracks = mutableListOf<Int>()
        if (music) {
            val audio = image.audioTracks
            if (audio.isEmpty()) log("Nessuna traccia audio nell'immagine: niente musica")
            else {
                encodeMusic(audio, File(outDir, "music"), threads, progress, cancelled)
                tracks += audio.map { it.number }
            }
        }
        writeManifest(outDir, source, files, tracks)
        progress.onProgress(Stage.DONE, 1, 1, "Completato")
        return ExtractionResult(source.description, files, tracks)
    }

    private fun encodeMusic(audio: List<ImageTrack>, dir: File, threads: Int, progress: ProgressListener, cancelled: () -> Boolean) {
        if (!dir.isDirectory && !dir.mkdirs()) throw IOException("Impossibile creare $dir")
        val total = audio.sumOf { it.audioFrames }
        val done = AtomicLong()
        val pool = Executors.newFixedThreadPool(threads.coerceAtLeast(1))
        try {
            val jobs: List<Future<*>> = audio.map { track ->
                pool.submit {
                    val name = "track%02d.flac".format(track.number)
                    val target = File(dir, name)
                    val part = File(dir, "$name.part")
                    try {
                        RandomAccessFile(part, "rw").use { raf ->
                            track.data.openStream().use { pcm ->
                                FlacEncoder().encode(pcm, track.audioFrames, raf, track.bigEndian) { n ->
                                    val d = done.addAndGet(n)
                                    checkCancel(cancelled)
                                    progress.onProgress(Stage.MUSIC, d, total, name)
                                }
                            }
                        }
                    } catch (e: Throwable) {
                        part.delete()
                        throw e
                    }
                    rename(part, target)
                    log("Musica: $name (%.1f s)".format(track.seconds))
                }
            }
            for (j in jobs) {
                try {
                    j.get()
                } catch (e: java.util.concurrent.ExecutionException) {
                    jobs.forEach { it.cancel(true) }
                    throw e.cause ?: e
                }
            }
        } finally {
            pool.shutdownNow()
        }
    }

    private fun writeManifest(outDir: File, source: GameSource, files: List<ExtractedFile>, tracks: List<Int>) {
        val text = buildString {
            appendLine("# Gromada installer manifest")
            appendLine("format=1")
            appendLine("source=${source.description}")
            appendLine("volume=${iso.volumeId}")
            appendLine("music=${tracks.joinToString(",")}")
            for (f in files) appendLine("${f.md5} ${f.size} ${f.path}")
        }
        writeAtomically(File(outDir, MANIFEST)) { it.write(text.toByteArray(Charsets.UTF_8)) }
    }

    companion object {
        const val MANIFEST = ".gromada-installer"

        /** Files the core needs; a folder missing any of them is not a usable game folder. */
        val REQUIRED = listOf("fw.res", "maps")

        /** The game's save slots (save1..save5): never overwritten by a new extraction. */
        fun isSaveSlot(path: String) = path.matches(Regex("(?i)save\\d+"))

        fun isGameFolder(dir: File) = REQUIRED.all { name -> dir.listFiles()?.any { it.name.equals(name, true) } == true }

        internal fun checkCancel(cancelled: () -> Boolean) {
            if (cancelled() || Thread.currentThread().isInterrupted) throw CancellationException("Annullato")
        }

        private fun <T> writeAtomically(target: File, body: (OutputStream) -> T): T {
            target.parentFile?.let { if (!it.isDirectory && !it.mkdirs()) throw IOException("Impossibile creare $it") }
            val part = File(target.parentFile, target.name + ".part")
            val result = try {
                FileOutputStream(part).buffered(256 * 1024).use { body(it) }
            } catch (e: Throwable) {
                part.delete()
                throw e
            }
            rename(part, target)
            return result
        }

        private fun rename(from: File, to: File) {
            if (to.exists() && !to.delete()) throw IOException("Impossibile sostituire $to")
            if (!from.renameTo(to)) throw IOException("Impossibile rinominare $from in $to")
        }
    }
}

internal fun ByteArray.toHex() = joinToString("") { "%02x".format(it) }
