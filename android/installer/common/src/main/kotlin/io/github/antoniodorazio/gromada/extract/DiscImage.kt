package io.github.antoniodorazio.gromada.extract

import java.io.IOException

/** One track of a CD image, with its bytes in the image file. */
class ImageTrack(
    val number: Int,
    val isAudio: Boolean,
    val data: ByteSource,
    val sectorSize: Int,
    /** Audio samples are big-endian (CUE "MOTOROLA" files). */
    val bigEndian: Boolean = false,
) {
    /** Stereo 16-bit frames of an audio track. */
    val audioFrames: Long get() = data.size / 4
    val seconds: Double get() = audioFrames / 44100.0
    override fun toString() = "track %02d %s (%d bytes)".format(number, if (isAudio) "audio" else "data/$sectorSize", data.size)
}

/**
 * A CD image: .cue + .bin (single or one file per track, BINARY/MOTOROLA/WAVE), a plain
 * .iso, or a raw 2352-byte .bin without cue (data track only).
 */
class DiscImage(val tracks: List<ImageTrack>, private val owned: List<AutoCloseable> = emptyList()) : AutoCloseable {
    val dataTrack: ImageTrack? get() = tracks.firstOrNull { !it.isAudio }
    val audioTracks: List<ImageTrack> get() = tracks.filter { it.isAudio }

    override fun close() = owned.forEach { it.close() }

    companion object {
        /**
         * [open] receives each FILE name of the cue sheet (and its position among the FILE
         * entries) and returns its bytes; it decides how to map names to real files
         * (e.g. a cue naming "GROMADA.bin" for a file renamed "GROMADA_1.bin").
         */
        fun fromCue(cueText: String, open: (name: String, fileIndex: Int, fileCount: Int) -> ByteSource): DiscImage {
            val cue = CueSheet.parse(cueText)
            val sources = mutableMapOf<CueFile, ByteSource>()
            val owned = mutableListOf<AutoCloseable>()
            try {
                for ((i, f) in cue.files.withIndex()) {
                    val src = open(f.name, i, cue.files.size).also { owned += it }
                    sources[f] = when (f.type) {
                        "BINARY", "MOTOROLA" -> src
                        "WAVE" -> wavData(src)
                        else -> throw CueException("Unsupported FILE type ${f.type} (${f.name})")
                    }
                }
                val tracks = cue.extents { sources.getValue(it).size }.map { e ->
                    val src = sources.getValue(e.file).slice(e.byteOffset, e.byteLength)
                    ImageTrack(e.track.number, e.track.isAudio, src, e.track.sectorSize, bigEndian = e.file.type == "MOTOROLA")
                }
                return DiscImage(tracks, owned)
            } catch (e: Exception) {
                owned.forEach { runCatching { it.close() } }
                throw e
            }
        }

        /** A plain ISO (2048-byte sectors) or a raw single-track image (2352/2336-byte sectors). */
        fun fromDataImage(src: ByteSource): DiscImage {
            val sectorSize = when {
                src.size >= 2352 * 17 && DataTrack.hasSync(src.readBytes(16L * 2352, 12)) -> 2352
                src.size >= 2352 && DataTrack.hasSync(src.readBytes(0, 12)) -> 2352
                src.size % 2336 == 0L && src.size % 2048 != 0L -> 2336
                else -> 2048
            }
            return DiscImage(listOf(ImageTrack(1, false, src, sectorSize)), listOf(src))
        }

        /** The PCM payload ("data" chunk) of a RIFF/WAVE file; must be 16-bit stereo 44.1 kHz. */
        fun wavData(src: ByteSource): ByteSource {
            val head = src.readBytes(0, 12)
            if (String(head, 0, 4, Charsets.US_ASCII) != "RIFF" || String(head, 8, 4, Charsets.US_ASCII) != "WAVE")
                throw IOException("Not a WAVE file")
            var p = 12L
            var fmtOk = false
            while (p + 8 <= src.size) {
                val ch = src.readBytes(p, 8)
                val id = String(ch, 0, 4, Charsets.US_ASCII)
                val len = ch.u32le(4)
                if (id == "fmt ") {
                    val f = src.readBytes(p + 8, 16)
                    fmtOk = f.u16le(0) == 1 && f.u16le(2) == 2 && f.i32le(4) == 44100 && f.u16le(14) == 16
                } else if (id == "data") {
                    if (!fmtOk) throw IOException("WAVE track is not 16-bit stereo 44.1 kHz PCM")
                    return src.slice(p + 8, minOf(len, src.size - p - 8))
                }
                p += 8 + len + (len and 1)
            }
            throw IOException("WAVE file without data chunk")
        }
    }
}
