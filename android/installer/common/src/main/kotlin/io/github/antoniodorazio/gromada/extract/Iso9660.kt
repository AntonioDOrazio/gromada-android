package io.github.antoniodorazio.gromada.extract

import java.io.IOException

/**
 * The user data of a CD data track as a flat sequence of 2048-byte logical sectors
 * (what an .iso file contains). Raw sectors may be:
 *  - 2048 bytes (plain ISO / MODE1/2048): user data only;
 *  - 2352 bytes, Mode 1: 12 sync + 4 header bytes, then 2048 user bytes (+ EDC/ECC);
 *  - 2352 bytes, Mode 2 Form 1 (XA): 12 sync + 4 header + 8 subheader, then 2048;
 *  - 2336 bytes (Mode 2 without sync/header): 8 subheader bytes, then 2048.
 * For 2352-byte sectors the mode is read from the sector header (the CUE sheet label
 * is often wrong), using the first volume descriptor sector (16).
 */
class DataTrack(private val raw: ByteSource, private val rawSectorSize: Int) : ByteSource {
    private val userOffset: Int
    private val sectorCount: Long = raw.size / rawSectorSize

    init {
        userOffset = when (rawSectorSize) {
            2048 -> 0
            2336 -> 8
            2352 -> {
                val probe = if (sectorCount > 16) 16L else 0L
                val hdr = raw.readBytes(probe * 2352, 16)
                if (!hasSync(hdr)) throw IOException("Data track has no CD sync pattern at sector $probe")
                when (hdr.u8(15)) {
                    1 -> 16
                    2 -> 24
                    else -> throw IOException("Unsupported CD sector mode ${hdr.u8(15)}")
                }
            }
            else -> throw IOException("Unsupported data sector size $rawSectorSize")
        }
    }

    override val size: Long = sectorCount * SECTOR

    // Small read-ahead cache: CAB extraction reads sequentially in small pieces.
    private val cacheSectors = 64
    private val rawBuf = ByteArray(cacheSectors * rawSectorSize)
    private val cache = ByteArray(cacheSectors * SECTOR)
    private var cacheFirst = -1L
    private var cacheCount = 0

    @Synchronized
    override fun read(pos: Long, dst: ByteArray, off: Int, len: Int): Int {
        if (pos >= size || len == 0) return 0
        val sector = pos / SECTOR
        if (sector < cacheFirst || sector >= cacheFirst + cacheCount) load(sector)
        val inCache = pos - cacheFirst * SECTOR
        val n = minOf(len.toLong(), cacheCount * SECTOR.toLong() - inCache).toInt()
        System.arraycopy(cache, inCache.toInt(), dst, off, n)
        return n
    }

    private fun load(first: Long) {
        val count = minOf(cacheSectors.toLong(), sectorCount - first).toInt()
        raw.readFully(first * rawSectorSize, rawBuf, 0, count * rawSectorSize)
        for (i in 0 until count)
            System.arraycopy(rawBuf, i * rawSectorSize + userOffset, cache, i * SECTOR, SECTOR)
        cacheFirst = first
        cacheCount = count
    }

    companion object {
        const val SECTOR = 2048
        private val SYNC = byteArrayOf(0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0)
        fun hasSync(b: ByteArray) = (0 until 12).all { b[it] == SYNC[it] }
    }
}

class IsoEntry(val name: String, val isDirectory: Boolean, val extent: Long, val size: Long) {
    override fun toString() = if (isDirectory) "$name/" else "$name ($size)"
}

/**
 * Read-only ISO 9660 filesystem with Joliet (UCS-2 long names) when present, as on the
 * Gromada CD. Names compare case-insensitively; version suffixes (";1") are dropped.
 */
class Iso9660(private val data: ByteSource) {
    val root: IsoEntry
    val joliet: Boolean
    val volumeId: String

    init {
        var primary: ByteArray? = null
        var supplementary: ByteArray? = null
        var sector = 16L
        while (sector < 16 + 64) {
            val vd = data.readBytes(sector * DataTrack.SECTOR, DataTrack.SECTOR)
            if (String(vd, 1, 5, Charsets.US_ASCII) != "CD001") throw IOException("Not an ISO 9660 filesystem (sector $sector)")
            when (vd.u8(0)) {
                1 -> if (primary == null) primary = vd
                2 -> if (vd.u8(88) == 0x25 && vd.u8(89) == 0x2F && vd.u8(90) in setOf(0x40, 0x43, 0x45)) supplementary = vd
                255 -> break
            }
            sector++
        }
        val vd = supplementary ?: primary ?: throw IOException("No ISO 9660 primary volume descriptor")
        joliet = supplementary != null
        val blockSize = vd.u16le(128)
        if (blockSize != DataTrack.SECTOR) throw IOException("Unsupported ISO block size $blockSize")
        root = parseRecord(vd, 156)?.let { IsoEntry("", true, it.extent, it.size) }
            ?: throw IOException("Bad ISO root directory record")
        volumeId = decodeName(vd.copyOfRange(40, 72)).trim { it <= ' ' }
    }

    fun list(dir: IsoEntry): List<IsoEntry> {
        require(dir.isDirectory)
        val bytes = data.readBytes(dir.extent * DataTrack.SECTOR, dir.size.toInt())
        val out = mutableListOf<IsoEntry>()
        var p = 0
        while (p < bytes.size) {
            val len = bytes.u8(p)
            if (len == 0) { // records never cross sectors: skip to the next one
                p = (p / DataTrack.SECTOR + 1) * DataTrack.SECTOR
                continue
            }
            val rec = parseRecord(bytes, p)
            if (rec != null && rec.name != "\u0000" && rec.name != "\u0001") out += rec
            p += len
        }
        return out
    }

    private fun parseRecord(b: ByteArray, p: Int): IsoEntry? {
        val len = b.u8(p)
        if (len < 34) return null
        val extent = b.u32le(p + 2)
        val size = b.u32le(p + 10)
        val flags = b.u8(p + 25)
        if (flags and 0x80 != 0) throw IOException("Multi-extent ISO files are not supported")
        val nameLen = b.u8(p + 32)
        val rawName = b.copyOfRange(p + 33, p + 33 + nameLen)
        val name = if (nameLen == 1 && (rawName[0].toInt() == 0 || rawName[0].toInt() == 1)) rawName[0].toInt().toChar().toString()
        else decodeName(rawName)
        return IsoEntry(name, flags and 0x02 != 0, extent, size)
    }

    private fun decodeName(raw: ByteArray): String {
        var s = if (joliet) String(raw, Charsets.UTF_16BE) else String(raw, Charsets.ISO_8859_1)
        val semi = s.indexOf(';')
        if (semi >= 0) s = s.substring(0, semi)
        if (!joliet && s.endsWith('.')) s = s.dropLast(1)
        return s
    }

    /** Finds a path such as "INSTALL_ITA/DATA1.CAB", ignoring case. */
    fun find(path: String): IsoEntry? {
        var cur = root
        for (part in path.split('/').filter { it.isNotEmpty() }) {
            if (!cur.isDirectory) return null
            cur = list(cur).firstOrNull { it.name.equals(part, ignoreCase = true) } ?: return null
        }
        return cur
    }

    /** All entries below [dir] as (relative path, entry), depth-first. */
    fun walk(dir: IsoEntry = root, prefix: String = "", maxDepth: Int = 8): Sequence<Pair<String, IsoEntry>> = sequence {
        for (e in list(dir)) {
            val path = prefix + e.name
            yield(path to e)
            if (e.isDirectory && maxDepth > 0) yieldAll(walk(e, "$path/", maxDepth - 1))
        }
    }

    fun open(file: IsoEntry): ByteSource {
        require(!file.isDirectory)
        return data.slice(file.extent * DataTrack.SECTOR, file.size)
    }
}
