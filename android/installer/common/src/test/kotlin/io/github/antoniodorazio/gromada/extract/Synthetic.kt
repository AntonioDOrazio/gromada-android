package io.github.antoniodorazio.gromada.extract

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.Deflater

/** Builders for tiny synthetic CD images, ISO filesystems and InstallShield 5 cabinets. */
object Synthetic {
    class Le(size: Int) {
        val buf: ByteBuffer = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
        fun u8(at: Int, v: Int) = apply { buf.put(at, v.toByte()) }
        fun u16(at: Int, v: Int) = apply { buf.putShort(at, v.toShort()) }
        fun u32(at: Int, v: Int) = apply { buf.putInt(at, v) }
        fun bytes(at: Int, b: ByteArray) = apply { b.forEachIndexed { i, x -> buf.put(at + i, x) } }
        fun str(at: Int, s: String) = bytes(at, s.toByteArray(Charsets.ISO_8859_1) + 0)
        val array: ByteArray get() = buf.array()
    }

    // ---------------- InstallShield 5 ----------------

    class CabEntry(val dir: String, val name: String, val content: ByteArray, val compressed: Boolean = true)

    /** Returns (DATA1.HDR, DATA1.CAB) for a version-5 cabinet with one file group. */
    fun installShield5(entries: List<CabEntry>, groupName: String = "Resources"): Pair<ByteArray, ByteArray> {
        // --- cabinet data ---
        val cab = ByteArrayOutputStream()
        cab.write(ByteArray(0x200)) // common header + volume header, patched below
        val offsets = mutableListOf<Int>()
        val stored = mutableListOf<Int>()
        for (e in entries) {
            offsets += cab.size()
            val start = cab.size()
            if (e.compressed) {
                var p = 0
                do {
                    val n = minOf(0x8000, e.content.size - p)
                    val d = Deflater(Deflater.BEST_COMPRESSION, true)
                    d.setInput(e.content, p, n); d.finish()
                    val out = ByteArray(0x10000)
                    val len = d.deflate(out)
                    check(d.finished())
                    d.end()
                    cab.write(len and 0xFF); cab.write(len shr 8)
                    cab.write(out, 0, len)
                    p += n
                } while (p < e.content.size)
            } else cab.write(e.content)
            stored += cab.size() - start
        }
        val cabBytes = cab.toByteArray()
        Le(60).u32(0, InstallShieldCab.SIGNATURE).u32(4, 0x01005201).u32(20, 0x200)
            .u32(28, 0).u32(32, entries.size - 1).array.copyInto(cabBytes)

        // --- header ---
        val dirs = entries.map { it.dir }.distinct()
        val h = Le(0x4000)
        val d = 0x200
        h.u32(0, InstallShieldCab.SIGNATURE).u32(4, 0x01005201).u32(12, d).u32(16, 0x1000)
        val ft = 0x300 // file table offset, relative to d
        h.u32(d + 0x0c, ft).u32(d + 0x1c, dirs.size).u32(d + 0x28, entries.size)
        var heap = ft + 4 * (dirs.size + entries.size) // relative to d + ft for table entries
        val tableAbs = d + ft
        dirs.forEachIndexed { i, name ->
            h.u32(tableAbs + 4 * i, heap - ft); h.str(d + heap, name.replace('/', '\\')); heap += name.length + 1
        }
        entries.forEachIndexed { i, e ->
            val nameOff = heap - ft
            h.str(d + heap, e.name); heap += e.name.length + 1
            val desc = heap
            h.u32(tableAbs + 4 * (dirs.size + i), desc - ft)
            val p = d + desc
            h.u32(p, nameOff).u16(p + 4, dirs.indexOf(e.dir))
                .u16(p + 8, if (e.compressed) InstallShieldCab.FLAG_COMPRESSED else 0)
                .u32(p + 10, e.content.size).u32(p + 14, stored[i]).u32(p + 38, offsets[i])
                .bytes(p + 42, java.security.MessageDigest.getInstance("MD5").digest(e.content))
            heap += 0x3a
        }
        // file group: OffsetList at d+heap, descriptor after it
        val nameOff = heap; h.str(d + heap, groupName); heap += groupName.length + 1
        val list = heap; heap += 12
        val gd = heap; heap += 4 + 0x48 + 8
        h.u32(d + list, nameOff).u32(d + list + 4, gd).u32(d + list + 8, 0)
        h.u32(d + gd, nameOff).u32(d + gd + 4 + 0x48, 0).u32(d + gd + 8 + 0x48, entries.size - 1)
        h.u32(d + 0x3e, list)
        return h.array.copyOf(d + heap + 16) to cabBytes
    }

    // ---------------- ISO 9660 ----------------

    sealed class Node(val name: String)
    class FileNode(name: String, val data: ByteArray) : Node(name)
    class DirNode(name: String, val children: List<Node>) : Node(name)

    /** A 2048-byte-sector ISO with the given tree (Joliet names if [joliet]). */
    fun iso(root: DirNode, joliet: Boolean): ByteArray {
        val sectors = mutableMapOf<Node, Int>()
        val sizes = mutableMapOf<Node, Int>()
        var next = 20
        fun encName(n: String) = if (joliet) n.toByteArray(Charsets.UTF_16BE) else (n.uppercase() + if (n.contains('.')) ";1" else "").toByteArray()
        fun recLen(nameLen: Int) = (33 + nameLen).let { it + (it and 1) }
        fun dirSize(d: DirNode): Int {
            var size = 0
            var used = 0
            for (len in listOf(1, 1) + d.children.map { encName(it.name).size }) {
                val r = recLen(len)
                if (used + r > 2048) { size += 2048; used = 0 }
                used += r
            }
            return size + 2048
        }
        fun layout(n: Node) {
            when (n) {
                is DirNode -> { sizes[n] = dirSize(n); sectors[n] = next; next += sizes[n]!! / 2048; n.children.forEach(::layout) }
                is FileNode -> { sizes[n] = n.data.size; sectors[n] = next; next += (n.data.size + 2047) / 2048 }
            }
        }
        layout(root)
        val img = ByteArray(next * 2048)
        fun record(at: Int, node: Node?, rawName: ByteArray, target: Node): Int {
            val len = recLen(rawName.size)
            val b = ByteBuffer.wrap(img, at, len).order(ByteOrder.LITTLE_ENDIAN)
            img[at] = len.toByte()
            b.putInt(at + 2, sectors[target]!!); b.order(ByteOrder.BIG_ENDIAN).putInt(at + 6, sectors[target]!!)
            b.order(ByteOrder.LITTLE_ENDIAN).putInt(at + 10, sizes[target]!!); b.order(ByteOrder.BIG_ENDIAN).putInt(at + 14, sizes[target]!!)
            img[at + 25] = if (target is DirNode) 2 else 0
            img[at + 32] = rawName.size.toByte()
            rawName.copyInto(img, at + 33)
            return len
        }
        fun write(n: Node, parent: DirNode) {
            when (n) {
                is FileNode -> n.data.copyInto(img, sectors[n]!! * 2048)
                is DirNode -> {
                    var p = sectors[n]!! * 2048
                    var used = 0
                    val recs = listOf(byteArrayOf(0) to n as Node, byteArrayOf(1) to parent as Node) + n.children.map { encName(it.name) to it }
                    for ((raw, target) in recs) {
                        val len = recLen(raw.size)
                        if (used + len > 2048) { p += 2048 - used; used = 0 }
                        record(p, null, raw, target); p += len; used += len
                    }
                    n.children.forEach { write(it, n) }
                }
            }
        }
        write(root, root)
        fun descriptor(sector: Int, type: Int) {
            val at = sector * 2048
            img[at] = type.toByte()
            "CD001".toByteArray().copyInto(img, at + 1)
            img[at + 6] = 1
            if (type == 2) { img[at + 88] = 0x25; img[at + 89] = 0x2F; img[at + 90] = 0x45 }
            val vol = if (type == 2) "TEST".toByteArray(Charsets.UTF_16BE) else "TEST".toByteArray()
            vol.copyInto(img, at + 40)
            ByteBuffer.wrap(img).order(ByteOrder.LITTLE_ENDIAN).putShort(at + 128, 2048)
            record(at + 156, null, byteArrayOf(0), root)
        }
        descriptor(16, 1)
        if (joliet) { descriptor(17, 2); img[18 * 2048] = 255.toByte(); "CD001".toByteArray().copyInto(img, 18 * 2048 + 1) }
        else { img[17 * 2048] = 255.toByte(); "CD001".toByteArray().copyInto(img, 17 * 2048 + 1) }
        return img
    }

    /** Wraps 2048-byte sectors into raw 2352-byte Mode 1 sectors (sync + header; ECC left zero). */
    fun mode1Raw(iso: ByteArray): ByteArray {
        val n = iso.size / 2048
        val out = ByteArray(n * 2352)
        for (s in 0 until n) {
            val at = s * 2352
            out[at] = 0; for (i in 1..10) out[at + i] = -1; out[at + 11] = 0
            val lba = s + 150
            out[at + 12] = bcd(lba / 75 / 60); out[at + 13] = bcd(lba / 75 % 60); out[at + 14] = bcd(lba % 75)
            out[at + 15] = 1
            iso.copyInto(out, at + 16, s * 2048, s * 2048 + 2048)
        }
        return out
    }

    private fun bcd(v: Int) = (((v / 10) shl 4) or (v % 10)).toByte()

    // ---------------- audio ----------------

    /** Interleaved 16-bit little-endian stereo PCM with a few different signals. */
    fun pcm(frames: Int, seed: Int): ByteArray {
        val rnd = java.util.Random(seed.toLong())
        val out = ByteArray(frames * 4)
        for (i in 0 until frames) {
            val t = i / 44100.0
            val l = (12000 * Math.sin(2 * Math.PI * (220 + seed * 10) * t) + rnd.nextGaussian() * 200).toInt().coerceIn(-32768, 32767)
            val r = if (i % 20000 < 10000) -l else (l / 2 + rnd.nextInt(64))
            out[4 * i] = l.toByte(); out[4 * i + 1] = (l shr 8).toByte()
            out[4 * i + 2] = r.toByte(); out[4 * i + 3] = (r shr 8).toByte()
        }
        return out
    }
}
