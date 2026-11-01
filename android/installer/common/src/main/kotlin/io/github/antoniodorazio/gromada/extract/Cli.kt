package io.github.antoniodorazio.gromada.extract

import java.io.File
import kotlin.system.exitProcess

/**
 * Desktop CLI, same code as the Android installer:
 *
 *     gromada-extract <image.cue|image.iso|image.bin> <output dir> [--bin FILE]... [--no-music] [--threads N]
 *     gromada-extract --list <image>
 *
 * With a .cue, each FILE is looked up next to the cue (exact name, then ignoring case);
 * `--bin` overrides the lookup, in FILE order (useful when the .bin was renamed).
 */
object Cli {
    @JvmStatic
    fun main(args: Array<String>) {
        val positional = mutableListOf<String>()
        val bins = mutableListOf<File>()
        var music = true
        var list = false
        var threads = Runtime.getRuntime().availableProcessors().coerceIn(1, 4)
        var i = 0
        while (i < args.size) {
            when (val a = args[i]) {
                "--bin" -> bins += File(args.getOrNull(++i) ?: usage())
                "--no-music" -> music = false
                "--list" -> list = true
                "--threads" -> threads = args.getOrNull(++i)?.toIntOrNull() ?: usage()
                "-h", "--help" -> usage()
                else -> if (a.startsWith("--")) usage() else positional += a
            }
            i++
        }
        if (positional.size != (if (list) 1 else 2)) usage()
        val image = openImage(File(positional[0]), bins)
        image.use {
            for (t in image.tracks) println(t)
            val ex = GameExtractor(image) { System.err.println(it) }
            if (list) {
                for ((path, e) in ex.iso.walk()) println("  /$path${if (e.isDirectory) "/" else " (${e.size})"}")
                when (val src = ex.locateGame()) {
                    is GameSource.Cabinet -> {
                        for (g in src.cab.groups) println("group ${g.name}: ${g.firstFile}..${g.lastFile}")
                        for (f in src.cab.validFiles) println("  ${f.path} ${f.expandedSize} flags=${f.flags}")
                    }
                    is GameSource.PlainFiles -> src.files.forEach { println("  ${it.first}") }
                }
                return
            }
            var last = ""
            val start = System.nanoTime()
            val result = ex.extract(File(positional[1]), music, threads, progress = { stage, done, total, _ ->
                val pct = if (total > 0) done * 100 / total else 0
                val line = "$stage $pct%"
                if (line != last) { last = line; System.err.println(line) }
            })
            println("Sorgente: ${result.source}")
            println("File del gioco: ${result.files.size}, tracce musicali: ${result.musicTracks}")
            println("Tempo: %.1f s".format((System.nanoTime() - start) / 1e9))
        }
    }

    fun openImage(path: File, bins: List<File> = emptyList()): DiscImage {
        if (!path.name.endsWith(".cue", ignoreCase = true)) return DiscImage.fromDataImage(ChannelByteSource.open(path))
        val dir = path.absoluteFile.parentFile
        return DiscImage.fromCue(path.readText(Charsets.ISO_8859_1)) { name, index, _ ->
            val file = bins.getOrNull(index)
                ?: File(dir, name).takeIf { it.isFile }
                ?: dir.listFiles()?.firstOrNull { it.name.equals(name, ignoreCase = true) }
                ?: throw CueException("File '$name' citato nel cue non trovato in $dir (usa --bin)")
            ChannelByteSource.open(file)
        }
    }

    private fun usage(): Nothing {
        System.err.println(
            "uso: gromada-extract <immagine.cue|.iso|.bin> <cartella> [--bin FILE]... [--no-music] [--threads N]\n" +
                "     gromada-extract --list <immagine>"
        )
        exitProcess(2)
    }
}
