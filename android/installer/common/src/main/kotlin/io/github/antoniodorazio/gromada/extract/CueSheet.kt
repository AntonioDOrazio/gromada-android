package io.github.antoniodorazio.gromada.extract

/**
 * Minimal CUE sheet model: FILE / TRACK / INDEX, which is all a CD image needs to be cut.
 * Times are MSF (minutes:seconds:frames, 75 frames per second), relative to the start of
 * their FILE. Other commands (REM, TITLE, FLAGS, PREGAP, ...) are ignored: PREGAP/POSTGAP
 * describe silence that is not stored in the image.
 */
data class CueSheet(val files: List<CueFile>) {
    val tracks: List<CueTrack> get() = files.flatMap { it.tracks }

    companion object {
        fun parse(text: String): CueSheet {
            val files = mutableListOf<CueFile>()
            var currentFile: CueFile? = null
            var currentTrack: CueTrack? = null
            for ((lineNo, raw) in text.lines().withIndex()) {
                val line = raw.trim().trimStart('﻿')
                if (line.isEmpty()) continue
                val words = splitCueLine(line)
                when (words[0].uppercase()) {
                    "FILE" -> {
                        if (words.size < 2) throw CueException("Line ${lineNo + 1}: FILE without a name")
                        val type = words.getOrNull(2)?.uppercase() ?: "BINARY"
                        currentFile = CueFile(words[1], type).also { files += it }
                        currentTrack = null
                    }
                    "TRACK" -> {
                        val file = currentFile ?: throw CueException("Line ${lineNo + 1}: TRACK before FILE")
                        val number = words.getOrNull(1)?.toIntOrNull()
                            ?: throw CueException("Line ${lineNo + 1}: bad TRACK number")
                        val mode = words.getOrNull(2)?.uppercase() ?: throw CueException("Line ${lineNo + 1}: TRACK without type")
                        currentTrack = CueTrack(number, mode).also { file.tracks += it }
                    }
                    "INDEX" -> {
                        val track = currentTrack ?: throw CueException("Line ${lineNo + 1}: INDEX before TRACK")
                        val idx = words.getOrNull(1)?.toIntOrNull() ?: throw CueException("Line ${lineNo + 1}: bad INDEX")
                        val msf = words.getOrNull(2) ?: throw CueException("Line ${lineNo + 1}: INDEX without time")
                        track.indexes[idx] = parseMsf(msf)
                    }
                    else -> Unit
                }
            }
            if (files.isEmpty() || files.all { it.tracks.isEmpty() }) throw CueException("No tracks in the CUE sheet")
            for (t in files.flatMap { it.tracks })
                if (1 !in t.indexes) throw CueException("Track ${t.number} has no INDEX 01")
            return CueSheet(files)
        }

        fun parseMsf(msf: String): Long {
            val p = msf.split(':')
            if (p.size != 3) throw CueException("Bad MSF time '$msf'")
            val (m, s, f) = p.map { it.toLongOrNull() ?: throw CueException("Bad MSF time '$msf'") }
            if (s >= 60 || f >= 75) throw CueException("Bad MSF time '$msf'")
            return (m * 60 + s) * 75 + f
        }

        private fun splitCueLine(line: String): List<String> {
            val out = mutableListOf<String>()
            var i = 0
            while (i < line.length) {
                while (i < line.length && line[i].isWhitespace()) i++
                if (i >= line.length) break
                if (line[i] == '"') {
                    val end = line.indexOf('"', i + 1).let { if (it < 0) line.length else it }
                    out += line.substring(i + 1, end)
                    i = end + 1
                } else {
                    val start = i
                    while (i < line.length && !line[i].isWhitespace()) i++
                    out += line.substring(start, i)
                }
            }
            return out
        }
    }
}

class CueException(message: String) : Exception(message)

class CueFile(val name: String, val type: String) {
    val tracks = mutableListOf<CueTrack>()
}

class CueTrack(val number: Int, val mode: String) {
    /** INDEX number -> sector (frame) offset from the start of the FILE. */
    val indexes = sortedMapOf<Int, Long>()

    val isAudio get() = mode == "AUDIO"

    /** Bytes per sector in the image file. */
    val sectorSize: Int
        get() = when (mode) {
            "AUDIO", "MODE1/2352", "MODE2/2352", "CDI/2352" -> 2352
            "MODE1/2048", "MODE2/2048" -> 2048
            "MODE2/2336", "CDI/2336" -> 2336
            "MODE2/2324" -> 2324
            "CDG" -> 2448
            else -> throw CueException("Unsupported track mode $mode")
        }

    override fun toString() = "TRACK %02d %s".format(number, mode)
}

/** A track located in its image file: [byteOffset, byteOffset + byteLength) of [file]. */
data class TrackExtent(val track: CueTrack, val file: CueFile, val byteOffset: Long, val byteLength: Long) {
    val sectors: Long get() = byteLength / track.sectorSize
}

/**
 * Cuts every track at INDEX 01 boundaries: a track spans from its INDEX 01 to the next
 * track's INDEX 01 in the same file (so a following pregap stays with the previous track,
 * as most rippers do), the last track of a file to the end of that file.
 */
fun CueSheet.extents(fileSize: (CueFile) -> Long): List<TrackExtent> {
    val out = mutableListOf<TrackExtent>()
    for (file in files) {
        val size = fileSize(file)
        // For WAVE files the caller passes the size of the PCM "data" chunk.
        var prevSector = 0L
        var prevByte = 0L
        var prevSize = file.tracks.firstOrNull()?.sectorSize ?: 2352
        val starts = file.tracks.map { t ->
            val s = t.indexes.getValue(1)
            if (s < prevSector) throw CueException("$t: INDEX 01 goes backwards")
            val b = prevByte + (s - prevSector) * prevSize
            prevSector = s; prevByte = b; prevSize = t.sectorSize
            b
        }
        for ((i, t) in file.tracks.withIndex()) {
            val start = starts[i]
            val end = if (i + 1 < file.tracks.size) starts[i + 1] else size
            if (start > size) throw CueException("$t starts past the end of ${file.name} ($start > $size)")
            out += TrackExtent(t, file, start, minOf(end, size) - start)
        }
    }
    return out
}
