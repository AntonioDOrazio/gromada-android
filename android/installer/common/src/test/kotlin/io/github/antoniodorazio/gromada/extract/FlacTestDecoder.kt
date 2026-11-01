package io.github.antoniodorazio.gromada.extract

import java.security.MessageDigest

/**
 * Independent, strict FLAC decoder for tests (fixed-blocksize streams, all subframe types
 * except LPC, which our encoder never writes). Verifies frame CRCs and the STREAMINFO MD5.
 */
class FlacTestDecoder(private val data: ByteArray) {
    class StreamInfo(val minBlock: Int, val maxBlock: Int, val minFrame: Int, val maxFrame: Int,
                     val rate: Int, val channels: Int, val bps: Int, val total: Long, val md5: ByteArray)

    lateinit var info: StreamInfo
        private set
    var frames = 0
        private set

    private var pos = 0L // bit position

    private fun bits(n: Int): Long {
        var v = 0L
        var left = n
        while (left > 0) {
            val avail = 8 - (pos and 7).toInt()
            val take = minOf(avail, left)
            val b = data[(pos ushr 3).toInt()].toInt() and 0xFF
            v = (v shl take) or ((b ushr (avail - take)) and ((1 shl take) - 1)).toLong()
            pos += take
            left -= take
        }
        return v
    }

    private fun signed(n: Int): Int {
        val v = bits(n)
        return ((v shl (64 - n)) shr (64 - n)).toInt()
    }

    private fun unary(): Long {
        var q = 0L
        while (bits(1) == 0L) q++
        return q
    }

    /** Decodes everything; returns interleaved 16-bit samples. */
    fun decode(): ShortArray {
        check(String(data, 0, 4, Charsets.US_ASCII) == "fLaC") { "no fLaC marker" }
        pos = 32
        var last = false
        while (!last) {
            last = bits(1) == 1L
            val type = bits(7).toInt()
            val len = bits(24).toInt()
            val start = pos
            if (type == 0) {
                info = StreamInfo(bits(16).toInt(), bits(16).toInt(), bits(24).toInt(), bits(24).toInt(),
                    bits(20).toInt(), bits(3).toInt() + 1, bits(5).toInt() + 1, bits(36),
                    ByteArray(16) { bits(8).toByte() })
            }
            pos = start + len * 8L
        }
        check(info.channels == 2 && info.bps == 16)
        val out = ShortArray((info.total * 2).toInt())
        var written = 0
        while ((pos ushr 3) < data.size) {
            val frameStart = (pos ushr 3).toInt()
            check(bits(14) == 0x3FFEL) { "bad sync at $frameStart" }
            check(bits(1) == 0L)
            check(bits(1) == 0L) { "variable blocksize not expected" }
            val bsCode = bits(4).toInt()
            val rateCode = bits(4).toInt()
            val assignment = bits(4).toInt()
            check(bits(3) == 4L) { "sample size must be 16" }
            check(bits(1) == 0L)
            // UTF-8 coded frame number
            val first = bits(8).toInt()
            var extra = 0
            var number = first.toLong()
            if (first and 0x80 != 0) {
                extra = Integer.numberOfLeadingZeros(first.inv() shl 24) - 1
                number = (first and (0x3F shr extra)).toLong()
                repeat(extra) { number = (number shl 6) or (bits(8) and 0x3F) }
            }
            check(number == frames.toLong()) { "frame number $number != $frames" }
            val n = when (bsCode) {
                1 -> 192
                in 2..5 -> 576 shl (bsCode - 2)
                6 -> bits(8).toInt() + 1
                7 -> bits(16).toInt() + 1
                in 8..15 -> 256 shl (bsCode - 8)
                else -> error("reserved blocksize")
            }
            when (rateCode) { 12 -> bits(8); 13, 14 -> bits(16) }
            val headerEnd = (pos ushr 3).toInt()
            val crc8 = bits(8).toInt()
            check(crc8 == FlacEncoder.crc8(data.copyOfRange(frameStart, headerEnd), headerEnd - frameStart)) { "header CRC" }

            // side channel has one extra bit: second subframe for left/side and mid/side, first for side/right
            val a = subframe(n, if (assignment == 9) 17 else 16)
            val b = subframe(n, if (assignment == 8 || assignment == 10) 17 else 16)
            if (pos and 7 != 0L) pos += 8 - (pos and 7)
            val frameEnd = (pos ushr 3).toInt()
            val crc16 = bits(16).toInt()
            check(crc16 == FlacEncoder.crc16(data.copyOfRange(frameStart, frameEnd), frameEnd - frameStart)) { "frame CRC" }
            for (i in 0 until n) {
                val (l, r) = when (assignment) {
                    1 -> a[i] to b[i]
                    8 -> a[i] to a[i] - b[i]
                    9 -> (a[i] + b[i]) to b[i]
                    10 -> { val m = (a[i] shl 1) or (b[i] and 1); ((m + b[i]) shr 1) to ((m - b[i]) shr 1) }
                    else -> error("assignment $assignment")
                }
                out[written++] = l.toShort().also { check(it.toInt() == l) }
                out[written++] = r.toShort().also { check(it.toInt() == r) }
            }
            frames++
        }
        check(written == out.size) { "decoded $written of ${out.size}" }
        val md5 = MessageDigest.getInstance("MD5")
        val bytes = ByteArray(out.size * 2)
        for (i in out.indices) { bytes[2 * i] = out[i].toByte(); bytes[2 * i + 1] = (out[i].toInt() shr 8).toByte() }
        check(md5.digest(bytes).contentEquals(info.md5)) { "MD5 mismatch" }
        return out
    }

    private fun subframe(n: Int, sampleBits: Int): IntArray {
        check(bits(1) == 0L)
        val type = bits(6).toInt()
        check(bits(1) == 0L) { "wasted bits not expected" }
        val x = IntArray(n)
        when {
            type == 0 -> { val v = signed(sampleBits); x.fill(v) }
            type == 1 -> for (i in 0 until n) x[i] = signed(sampleBits)
            type in 8..12 -> {
                val order = type - 8
                for (i in 0 until order) x[i] = signed(sampleBits)
                val method = bits(2).toInt()
                check(method <= 1)
                val po = bits(4).toInt()
                var i = order
                for (p in 0 until (1 shl po)) {
                    val k = bits(if (method == 1) 5 else 4).toInt()
                    check(k != if (method == 1) 31 else 15) { "escape not expected" }
                    val count = (n shr po) - if (p == 0) order else 0
                    repeat(count) {
                        val q = unary()
                        val u = (q shl k) or bits(k)
                        val r = ((u ushr 1) xor -(u and 1)).toInt()
                        x[i] = r + when (order) {
                            0 -> 0
                            1 -> x[i - 1]
                            2 -> 2 * x[i - 1] - x[i - 2]
                            3 -> 3 * x[i - 1] - 3 * x[i - 2] + x[i - 3]
                            else -> 4 * x[i - 1] - 6 * x[i - 2] + 4 * x[i - 3] - x[i - 4]
                        }
                        i++
                    }
                }
            }
            else -> error("subframe type $type")
        }
        return x
    }
}
