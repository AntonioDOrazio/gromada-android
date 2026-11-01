package io.github.antoniodorazio.gromada.extract

import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.charset.Charset
import java.security.MessageDigest
import java.util.zip.DataFormatException
import java.util.zip.Inflater

/**
 * Reader for InstallShield 5+ cabinets (DATA1.HDR + DATA1.CAB), the format of the
 * Gromada installer. Format knowledge from unshield (MIT, github.com/twogood/unshield):
 *  - common header: signature "ISc(", version, volume info, cab descriptor offset/size;
 *  - cab descriptor: file table (directory name offsets, then file descriptor offsets),
 *    file groups, components;
 *  - compressed file data: chunks of [u16 length][raw deflate stream], each inflated
 *    independently.
 * Only single-volume cabinets are supported (files split across DATA2.CAB... are reported).
 */
class InstallShieldCab(header: ByteArray, private val cab: ByteSource) {
    val majorVersion: Int
    val files: List<CabFile>
    val directories: List<String>
    val groups: List<CabGroup>

    private val hdr = header
    private val descOffset: Int
    private val fileTableOffset: Int
    private val volumeFirstFile: Long
    private val volumeLastFile: Long

    init {
        if (hdr.size < 20 || hdr.i32le(0) != SIGNATURE) throw IOException("Not an InstallShield cabinet header")
        val version = hdr.u32le(4)
        majorVersion = when ((version ushr 24).toInt()) {
            1 -> ((version ushr 12) and 0xF).toInt()
            2, 4 -> (version and 0xFFFF).toInt().let { if (it != 0) it / 100 else 0 }
            else -> throw IOException("Unknown InstallShield version 0x%08x".format(version))
        }
        if (majorVersion in 1..4 || majorVersion > 16) throw IOException("Unsupported InstallShield version $majorVersion")
        descOffset = hdr.i32le(12)
        if (hdr.i32le(16) == 0) throw IOException("Cabinet header has no descriptor")
        val d = descOffset
        fileTableOffset = hdr.i32le(d + 0x0c)
        val dirCount = hdr.i32le(d + 0x1c)
        val fileCount = hdr.i32le(d + 0x28)
        val fileTableOffset2 = hdr.i32le(d + 0x2c)
        val tableBase = d + fileTableOffset
        if (dirCount < 0 || fileCount <= 0 || tableBase + 4L * (dirCount + fileCount) > hdr.size)
            throw IOException("Corrupt cabinet file table")
        val table = IntArray(dirCount + fileCount) { hdr.i32le(tableBase + 4 * it) }

        directories = (0 until dirCount).map { cleanPath(string(tableBase + table[it])) }

        files = (0 until fileCount).map { i ->
            if (majorVersion <= 5) {
                val p = tableBase + table[dirCount + i]
                CabFile(
                    index = i,
                    nameOffset = hdr.i32le(p),
                    directory = hdr.u16le(p + 4),
                    flags = hdr.u16le(p + 8),
                    expandedSize = hdr.u32le(p + 10),
                    compressedSize = hdr.u32le(p + 14),
                    dataOffset = hdr.u32le(p + 38),
                    md5 = hdr.copyOfRange(p + 42, p + 58),
                    linkFlags = 0, linkPrevious = 0, volume = 1,
                )
            } else {
                val p = tableBase + fileTableOffset2 + i * 0x57
                CabFile(
                    index = i,
                    flags = hdr.u16le(p),
                    expandedSize = hdr.u64le(p + 2),
                    compressedSize = hdr.u64le(p + 10),
                    dataOffset = hdr.u64le(p + 18),
                    md5 = hdr.copyOfRange(p + 26, p + 42),
                    nameOffset = hdr.i32le(p + 58),
                    directory = hdr.u16le(p + 62),
                    linkPrevious = hdr.i32le(p + 76),
                    linkFlags = hdr.u8(p + 84),
                    volume = hdr.u16le(p + 85),
                )
            }.also { f ->
                f.name = if (f.nameOffset != 0) string(tableBase + f.nameOffset) else ""
                f.directoryName = directories.getOrElse(f.directory) { "" }
            }
        }

        groups = buildList {
            for (i in 0 until MAX_GROUPS) {
                var next = hdr.i32le(d + 0x3e + 4 * i)
                var guard = 0
                while (next != 0 && guard++ < 1000) {
                    val p = d + next
                    val descriptor = hdr.i32le(p + 4)
                    next = hdr.i32le(p + 8)
                    val g = d + descriptor
                    val name = string(d + hdr.i32le(g))
                    val q = g + 4 + if (majorVersion <= 5) 0x48 else 0x12
                    add(CabGroup(name, hdr.i32le(q), hdr.i32le(q + 4)))
                }
            }
        }

        // Volume header of this .cab: which file indices it holds.
        val vol = cab.readBytes(0, 20 + 64)
        if (vol.i32le(0) != SIGNATURE) throw IOException("Not an InstallShield cabinet (.cab)")
        // Same offsets in the v5 (40-byte) and v6+ (64-byte) volume headers.
        volumeFirstFile = vol.u32le(20 + 8)
        volumeLastFile = vol.u32le(20 + 12)
    }

    private fun string(offset: Int): String {
        var end = offset
        while (end < hdr.size && hdr[end] != 0.toByte()) end++
        return String(hdr, offset, end - offset, NAME_CHARSET)
    }

    /** Valid, extractable files (named, with data, not marked invalid). */
    val validFiles: List<CabFile>
        get() = files.filter { it.flags and FLAG_INVALID == 0 && it.name.isNotEmpty() && (it.dataOffset != 0L || it.linkFlags and LINK_PREV != 0) }

    /**
     * Writes the expanded content of [file] to [out]; returns the MD5 of the content.
     * Fails on size mismatch and, for version 6+, on MD5 mismatch (as unshield does).
     */
    fun extract(file: CabFile, out: OutputStream, onBytes: (Long) -> Unit = {}): ByteArray {
        if (file.linkFlags and LINK_PREV != 0) return extract(files[file.linkPrevious], out, onBytes)
        if (file.flags and FLAG_SPLIT != 0 || file.index < volumeFirstFile || file.index > volumeLastFile)
            throw IOException("${file.path} is stored in another cabinet volume (multi-volume cabinets are not supported)")
        val md5 = MessageDigest.getInstance("MD5")
        val input = ObfuscatedStream(cab.openStream(file.dataOffset), file.flags and FLAG_OBFUSCATED != 0)
        var written = 0L
        if (file.flags and FLAG_COMPRESSED != 0) {
            var left = file.compressedSize
            val chunk = ByteArray(0x10000 + 1)
            val outBuf = ByteArray(0x20000)
            val inflater = Inflater(true)
            try {
                while (left > 0) {
                    input.readFullyOrThrow(chunk, 0, 2)
                    val n = chunk.u16le(0)
                    if (n == 0) throw IOException("${file.path}: empty compressed chunk")
                    input.readFullyOrThrow(chunk, 0, n)
                    chunk[n] = 0 // the extra byte zlib wants in raw (nowrap) mode
                    inflater.reset()
                    inflater.setInput(chunk, 0, n + 1)
                    try {
                        while (!inflater.finished()) {
                            val got = inflater.inflate(outBuf)
                            if (got == 0 && (inflater.needsInput() || inflater.needsDictionary()))
                                throw IOException("${file.path}: truncated deflate chunk")
                            out.write(outBuf, 0, got)
                            md5.update(outBuf, 0, got)
                            written += got
                            onBytes(got.toLong())
                        }
                    } catch (e: DataFormatException) {
                        throw IOException("${file.path}: corrupt compressed data (${e.message})", e)
                    }
                    left -= 2 + n
                }
            } finally {
                inflater.end()
            }
        } else {
            val buf = ByteArray(0x10000)
            var left = file.expandedSize
            while (left > 0) {
                val n = minOf(left, buf.size.toLong()).toInt()
                input.readFullyOrThrow(buf, 0, n)
                out.write(buf, 0, n)
                md5.update(buf, 0, n)
                written += n
                left -= n
                onBytes(n.toLong())
            }
        }
        if (written != file.expandedSize)
            throw IOException("${file.path}: expanded size ${file.expandedSize} expected, got $written")
        val digest = md5.digest()
        // unshield checks MD5 only for v6+, but v5 descriptors carry it too (all 200 files of
        // the Italian Gromada CD match): check it whenever present, it catches bad image reads.
        val hasMd5 = majorVersion >= 6 || file.md5.any { it != 0.toByte() }
        if (hasMd5 && !digest.contentEquals(file.md5))
            throw IOException("${file.path}: MD5 mismatch (immagine danneggiata?)")
        return digest
    }

    companion object {
        const val SIGNATURE = 0x28635349 // "ISc("
        const val FLAG_SPLIT = 1
        const val FLAG_OBFUSCATED = 2
        const val FLAG_COMPRESSED = 4
        const val FLAG_INVALID = 8
        const val LINK_PREV = 1
        private const val MAX_GROUPS = 71
        val NAME_CHARSET: Charset = Charset.forName("windows-1252")

        /** Backslash-separated cabinet path to a safe relative path ("" for the root). */
        fun cleanPath(p: String): String =
            p.replace('\\', '/').split('/').filter { it.isNotEmpty() && it != "." && it != ".." }.joinToString("/")
    }
}

data class CabGroup(val name: String, val firstFile: Int, val lastFile: Int)

class CabFile(
    val index: Int,
    val nameOffset: Int,
    val directory: Int,
    val flags: Int,
    val expandedSize: Long,
    val compressedSize: Long,
    val dataOffset: Long,
    val md5: ByteArray,
    val linkFlags: Int,
    val linkPrevious: Int,
    val volume: Int,
) {
    var name: String = ""
        internal set
    var directoryName: String = ""
        internal set

    /** Relative output path, '/'-separated. */
    val path: String
        get() = listOf(directoryName, InstallShieldCab.cleanPath(name)).filter { it.isNotEmpty() }.joinToString("/")

    override fun toString() = "$path ($expandedSize)"
}

/** Applies InstallShield's byte obfuscation (counted over every byte read for the file). */
private class ObfuscatedStream(private val inner: InputStream, private val enabled: Boolean) : InputStream() {
    private var seed = 0

    private fun deobfuscate(b: Int): Int {
        val x = (b xor 0xD5) and 0xFF
        val ror = ((x ushr 2) or (x shl 6)) and 0xFF
        return (ror - (seed++ % 0x47)) and 0xFF
    }

    override fun read(): Int {
        val b = inner.read()
        return if (b < 0 || !enabled) b else deobfuscate(b)
    }

    override fun read(b: ByteArray, off: Int, len: Int): Int {
        val n = inner.read(b, off, len)
        if (enabled && n > 0) for (i in off until off + n) b[i] = deobfuscate(b[i].toInt() and 0xFF).toByte()
        return n
    }
}
