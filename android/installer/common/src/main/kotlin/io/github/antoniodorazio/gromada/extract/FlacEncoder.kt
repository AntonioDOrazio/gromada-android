package io.github.antoniodorazio.gromada.extract

import java.io.InputStream
import java.io.RandomAccessFile
import java.security.MessageDigest

/**
 * Lossless FLAC encoder for 16-bit stereo PCM (CD audio), pure Kotlin so the same code runs
 * on Android and on the desktop. It uses the FLAC "fixed" predictors (orders 0-4), constant
 * and verbatim subframes, the four stereo decorrelation modes and partitioned Rice coding:
 * roughly the compression of `flac -2`, with no native code.
 *
 * Output follows the FLAC format specification (RFC 9639): "fLaC", a STREAMINFO block with
 * the total sample count, frame size bounds and the MD5 of the PCM, then fixed-blocksize
 * frames with CRC-8 headers and CRC-16 footers.
 */
class FlacEncoder(
    private val sampleRate: Int = 44100,
    private val blockSize: Int = 4096,
) {
    private val channels = 2
    private val bps = 16

    init {
        require(blockSize in 16..65535)
        require(sampleRate in 1..655350)
    }

    /**
     * Encodes [totalFrames] stereo frames of 16-bit PCM read from [pcm] (little-endian unless
     * [bigEndian]) into [outFile]. Returns the MD5 of the PCM (as stored in STREAMINFO).
     */
    fun encode(
        pcm: InputStream,
        totalFrames: Long,
        outFile: RandomAccessFile,
        bigEndian: Boolean = false,
        onFrames: (Long) -> Unit = {},
    ): ByteArray {
        outFile.setLength(0)
        val md5 = MessageDigest.getInstance("MD5")
        outFile.write(byteArrayOf('f'.code.toByte(), 'L'.code.toByte(), 'a'.code.toByte(), 'C'.code.toByte()))
        outFile.write(streamInfo(totalFrames, 0, 0, ByteArray(16)))

        val raw = ByteArray(blockSize * 4)
        val left = IntArray(blockSize)
        val right = IntArray(blockSize)
        val mid = IntArray(blockSize)
        val side = IntArray(blockSize)
        val writer = BitWriter(blockSize * 4 * 2 + 64)
        var minFrame = Int.MAX_VALUE
        var maxFrame = 0
        var frameNo = 0L
        var done = 0L
        while (done < totalFrames) {
            val n = minOf(blockSize.toLong(), totalFrames - done).toInt()
            pcm.readFullyOrThrow(raw, 0, n * 4)
            if (bigEndian) for (i in 0 until n * 2) {
                val t = raw[2 * i]; raw[2 * i] = raw[2 * i + 1]; raw[2 * i + 1] = t
            }
            md5.update(raw, 0, n * 4)
            for (i in 0 until n) {
                val l = (raw[4 * i].toInt() and 0xFF) or (raw[4 * i + 1].toInt() shl 8)
                val r = (raw[4 * i + 2].toInt() and 0xFF) or (raw[4 * i + 3].toInt() shl 8)
                left[i] = l; right[i] = r
                mid[i] = (l + r) shr 1
                side[i] = l - r
            }
            writer.reset()
            encodeFrame(writer, frameNo, n, left, right, mid, side)
            val size = writer.length
            outFile.write(writer.buffer, 0, size)
            minFrame = minOf(minFrame, size)
            maxFrame = maxOf(maxFrame, size)
            frameNo++
            done += n
            onFrames(n.toLong())
        }
        val digest = md5.digest()
        outFile.seek(4)
        outFile.write(streamInfo(totalFrames, if (minFrame == Int.MAX_VALUE) 0 else minFrame, maxFrame, digest))
        return digest
    }

    private fun streamInfo(totalFrames: Long, minFrame: Int, maxFrame: Int, md5: ByteArray): ByteArray {
        val w = BitWriter(64)
        w.write(1, 1)       // last metadata block
        w.write(0, 7)       // STREAMINFO
        w.write(34, 24)
        w.write(blockSize.toLong(), 16)
        w.write(blockSize.toLong(), 16)
        w.write(minFrame.toLong(), 24)
        w.write(maxFrame.toLong(), 24)
        w.write(sampleRate.toLong(), 20)
        w.write((channels - 1).toLong(), 3)
        w.write((bps - 1).toLong(), 5)
        w.write(totalFrames, 36)
        for (b in md5) w.write((b.toInt() and 0xFF).toLong(), 8)
        return w.buffer.copyOf(w.length)
    }

    // --- frames ---

    private fun encodeFrame(w: BitWriter, frameNo: Long, n: Int, left: IntArray, right: IntArray, mid: IntArray, side: IntArray) {
        val l = analyze(left, n, bps)
        val r = analyze(right, n, bps)
        val m = analyze(mid, n, bps)
        val s = analyze(side, n, bps + 1)
        // channel assignment: 1 = independent, 8 = left/side, 9 = side/right, 10 = mid/side
        val options = listOf(
            Triple(1, l, r), Triple(8, l, s), Triple(9, s, r), Triple(10, m, s),
        )
        val (assignment, a, b) = options.minBy { it.second.bits + it.third.bits }

        // header
        w.write(0x3FFE, 14)          // sync
        w.write(0, 1)                // reserved
        w.write(0, 1)                // fixed blocksize
        val bsCode = blockSizeCode(n)
        w.write(bsCode.toLong(), 4)
        w.write(sampleRateCode().toLong(), 4)
        w.write(assignment.toLong(), 4)
        w.write(4, 3)                // 16 bits per sample
        w.write(0, 1)
        writeUtf8(w, frameNo)
        if (bsCode == 6) w.write((n - 1).toLong(), 8)
        else if (bsCode == 7) w.write((n - 1).toLong(), 16)
        if (sampleRateCode() == 12) w.write((sampleRate / 1000).toLong(), 8)
        else if (sampleRateCode() == 13) w.write(sampleRate.toLong(), 16)
        w.write(crc8(w.buffer, w.length).toLong(), 8)

        writeSubframe(w, a, n)
        writeSubframe(w, b, n)
        w.alignToByte()
        w.write(crc16(w.buffer, w.length).toLong(), 16)
    }

    private fun blockSizeCode(n: Int): Int = when (n) {
        192 -> 1
        576, 1152, 2304, 4608 -> 2 + Integer.numberOfTrailingZeros(n / 576)
        256, 512, 1024, 2048, 4096, 8192, 16384, 32768 -> 8 + Integer.numberOfTrailingZeros(n / 256)
        else -> if (n <= 256) 6 else 7
    }

    private fun sampleRateCode(): Int = when (sampleRate) {
        88200 -> 1; 176400 -> 2; 192000 -> 3; 8000 -> 4; 16000 -> 5; 22050 -> 6; 24000 -> 7
        32000 -> 8; 44100 -> 9; 48000 -> 10; 96000 -> 11
        else -> if (sampleRate % 1000 == 0 && sampleRate / 1000 < 256) 12 else 13
    }

    private fun writeUtf8(w: BitWriter, v: Long) {
        if (v < 0x80) { w.write(v, 8); return }
        var bytes = 2
        while (v >= (1L shl (5 * bytes + 1))) bytes++
        w.write(((0xFF00 shr bytes) and 0xFF).toLong() or (v ushr (6 * (bytes - 1))), 8)
        for (i in bytes - 2 downTo 0) w.write(0x80L or ((v ushr (6 * i)) and 0x3F), 8)
    }

    // --- subframes ---

    /** Chosen encoding of one channel of one block. */
    private class Plan(
        val samples: IntArray,
        val sampleBits: Int,
        val type: Int,            // 0 constant, 1 verbatim, 2 fixed
        val order: Int,
        val residual: IntArray?,
        val partitionOrder: Int,
        val riceParams: IntArray?,
        val bits: Long,
    )

    private fun analyze(x: IntArray, n: Int, sampleBits: Int): Plan {
        var constant = true
        for (i in 1 until n) if (x[i] != x[0]) { constant = false; break }
        if (constant) return Plan(x, sampleBits, 0, 0, null, 0, null, 8L + sampleBits)

        // Sum of |residual| for each fixed order (successive differences), as reference flac does.
        val sums = LongArray(5)
        val maxOrder = minOf(4, n - 1)
        var p1 = 0; var p2 = 0; var p3 = 0; var p4 = 0
        for (i in 0 until n) {
            val e0 = x[i]
            val e1 = e0 - p1
            val e2 = e1 - p2
            val e3 = e2 - p3
            val e4 = e3 - p4
            if (i >= 0) sums[0] += kotlin.math.abs(e0).toLong()
            if (i >= 1) sums[1] += kotlin.math.abs(e1).toLong()
            if (i >= 2) sums[2] += kotlin.math.abs(e2).toLong()
            if (i >= 3) sums[3] += kotlin.math.abs(e3).toLong()
            if (i >= 4) sums[4] += kotlin.math.abs(e4).toLong()
            p1 = e0; p2 = e1; p3 = e2; p4 = e3
        }
        var order = 0
        for (o in 1..maxOrder) if (sums[o] < sums[order]) order = o

        val residual = IntArray(n - order)
        fixedResidual(x, n, order, residual)
        val (partitionOrder, params, residualBits) = bestRice(residual, n, order)
        val fixedBits = 8L + order.toLong() * sampleBits + residualBits
        val verbatimBits = 8L + n.toLong() * sampleBits
        return if (fixedBits < verbatimBits) Plan(x, sampleBits, 2, order, residual, partitionOrder, params, fixedBits)
        else Plan(x, sampleBits, 1, 0, null, 0, null, verbatimBits)
    }

    private fun fixedResidual(x: IntArray, n: Int, order: Int, out: IntArray) {
        for (i in order until n) {
            out[i - order] = when (order) {
                0 -> x[i]
                1 -> x[i] - x[i - 1]
                2 -> x[i] - 2 * x[i - 1] + x[i - 2]
                3 -> x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3]
                else -> x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4]
            }
        }
    }

    /** Chooses partition order and Rice parameters; returns (order, params, bits incl. headers). */
    private fun bestRice(res: IntArray, n: Int, predOrder: Int): Triple<Int, IntArray, Long> {
        // zigzag-mapped residuals
        val u = LongArray(res.size) { val r = res[it]; ((r.toLong() shl 1) xor (r.toLong() shr 63)) }
        var maxPo = 0
        while (maxPo < 8 && (n shr (maxPo + 1)) shl (maxPo + 1) == n && (n shr (maxPo + 1)) > predOrder) maxPo++
        var best: Triple<Int, IntArray, Long>? = null
        for (po in 0..maxPo) {
            val parts = 1 shl po
            val params = IntArray(parts)
            var bits = 2L + 4L // method + partition order
            var idx = 0
            var useRice2 = false
            for (p in 0 until parts) {
                val count = (n shr po) - if (p == 0) predOrder else 0
                var sum = 0L
                for (i in idx until idx + count) sum += u[i]
                val (k, cost) = riceParam(u, idx, count, sum)
                params[p] = k
                if (k > 14) useRice2 = true
                bits += cost
                idx += count
            }
            bits += parts.toLong() * if (useRice2) 5 else 4
            if (best == null || bits < best.third) best = Triple(po, params, bits)
        }
        return best!!
    }

    /** Best Rice parameter for a partition: exact cost around the estimate log2(mean). */
    private fun riceParam(u: LongArray, from: Int, count: Int, sum: Long): Pair<Int, Long> {
        if (count == 0) return 0 to 0L
        val mean = sum / count
        val guess = if (mean <= 0) 0 else 63 - java.lang.Long.numberOfLeadingZeros(mean)
        var bestK = 0
        var bestCost = Long.MAX_VALUE
        for (k in maxOf(0, guess - 1)..minOf(MAX_RICE, guess + 1)) {
            var cost = count.toLong() * (k + 1)
            for (i in from until from + count) cost += u[i] ushr k
            if (cost < bestCost) { bestCost = cost; bestK = k }
        }
        return bestK to bestCost
    }

    private fun writeSubframe(w: BitWriter, p: Plan, n: Int) {
        when (p.type) {
            0 -> {
                w.write(0, 1); w.write(0, 6); w.write(0, 1)
                w.writeSigned(p.samples[0], p.sampleBits)
            }
            1 -> {
                w.write(0, 1); w.write(1, 6); w.write(0, 1)
                for (i in 0 until n) w.writeSigned(p.samples[i], p.sampleBits)
            }
            else -> {
                w.write(0, 1); w.write((8 or p.order).toLong(), 6); w.write(0, 1)
                for (i in 0 until p.order) w.writeSigned(p.samples[i], p.sampleBits)
                val params = p.riceParams!!
                val rice2 = params.any { it > 14 }
                w.write(if (rice2) 1 else 0, 2)
                w.write(p.partitionOrder.toLong(), 4)
                val res = p.residual!!
                var idx = 0
                for ((part, k) in params.withIndex()) {
                    val count = (n shr p.partitionOrder) - if (part == 0) p.order else 0
                    w.write(k.toLong(), if (rice2) 5 else 4)
                    for (i in idx until idx + count) w.writeRice(res[i], k)
                    idx += count
                }
            }
        }
    }

    companion object {
        private const val MAX_RICE = 30

        private val CRC8 = IntArray(256) { i ->
            var c = i
            repeat(8) { c = if (c and 0x80 != 0) (c shl 1) xor 0x07 else c shl 1 }
            c and 0xFF
        }
        private val CRC16 = IntArray(256) { i ->
            var c = i shl 8
            repeat(8) { c = if (c and 0x8000 != 0) (c shl 1) xor 0x8005 else c shl 1 }
            c and 0xFFFF
        }

        fun crc8(b: ByteArray, len: Int): Int {
            var c = 0
            for (i in 0 until len) c = CRC8[c xor (b[i].toInt() and 0xFF)]
            return c
        }

        fun crc16(b: ByteArray, len: Int): Int {
            var c = 0
            for (i in 0 until len) c = ((c shl 8) xor CRC16[(c ushr 8) xor (b[i].toInt() and 0xFF)]) and 0xFFFF
            return c
        }
    }
}

/** MSB-first bit writer into a growable byte array. */
internal class BitWriter(initial: Int) {
    var buffer = ByteArray(initial)
        private set
    private var bytes = 0
    private var acc = 0L   // pending bits, right-aligned
    private var accBits = 0

    /** Bytes written so far (call [alignToByte] first for an exact count). */
    val length: Int get() = bytes

    fun reset() { bytes = 0; acc = 0; accBits = 0 }

    fun write(value: Long, bits: Int) {
        if (bits == 0) return
        if (bits > 32) { write(value ushr 32, bits - 32); write(value and 0xFFFFFFFFL, 32); return }
        acc = (acc shl bits) or (value and ((1L shl bits) - 1))
        accBits += bits
        while (accBits >= 8) {
            accBits -= 8
            put(((acc ushr accBits) and 0xFF).toInt())
        }
    }

    fun write(value: Int, bits: Int) = write(value.toLong(), bits)

    fun writeSigned(value: Int, bits: Int) = write(value.toLong() and ((1L shl bits) - 1), bits)

    fun writeRice(r: Int, k: Int) {
        val u = (r.toLong() shl 1) xor (r.toLong() shr 63)
        var q = u ushr k
        while (q >= 32) { write(0, 32); q -= 32 }
        write(1L, q.toInt() + 1)               // q zeros then a one
        if (k > 0) write(u and ((1L shl k) - 1), k)
    }

    fun alignToByte() {
        if (accBits > 0) write(0, 8 - accBits)
    }

    private fun put(b: Int) {
        if (bytes == buffer.size) buffer = buffer.copyOf(buffer.size * 2)
        buffer[bytes++] = b.toByte()
    }
}
