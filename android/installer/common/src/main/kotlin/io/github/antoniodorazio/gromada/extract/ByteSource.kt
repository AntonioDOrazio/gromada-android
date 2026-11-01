package io.github.antoniodorazio.gromada.extract

import java.io.EOFException
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.channels.FileChannel

/**
 * Random-access, read-only bytes. Positional reads only, so one source can be shared by
 * several readers. On Android it wraps the FileChannel of a SAF ParcelFileDescriptor,
 * on the desktop a RandomAccessFile.
 */
interface ByteSource : AutoCloseable {
    val size: Long

    /** Reads up to [len] bytes at [pos]; returns the number read (0 only at/after the end). */
    fun read(pos: Long, dst: ByteArray, off: Int, len: Int): Int

    override fun close() {}

    fun readFully(pos: Long, dst: ByteArray, off: Int = 0, len: Int = dst.size) {
        var done = 0
        while (done < len) {
            val n = read(pos + done, dst, off + done, len - done)
            if (n <= 0) throw EOFException("Unexpected end of data at ${pos + done} (size $size)")
            done += n
        }
    }

    fun readBytes(pos: Long, len: Int): ByteArray = ByteArray(len).also { readFully(pos, it) }

    /** A window [offset, offset + length) of this source. */
    fun slice(offset: Long, length: Long): ByteSource {
        require(offset >= 0 && length >= 0 && offset + length <= size) {
            "Slice [$offset, ${offset + length}) out of bounds (size $size)"
        }
        val parent = this
        return object : ByteSource {
            override val size = length
            override fun read(pos: Long, dst: ByteArray, off: Int, len: Int): Int {
                if (pos >= length) return 0
                return parent.read(offset + pos, dst, off, minOf(len.toLong(), length - pos).toInt())
            }
        }
    }

    /** Sequential stream over [from, size), with a large read-ahead buffer. */
    fun openStream(from: Long = 0, bufferSize: Int = 256 * 1024): InputStream = SourceInputStream(this, from, bufferSize)
}

class ChannelByteSource(private val channel: FileChannel, private val closeable: AutoCloseable? = null) : ByteSource {
    override val size: Long = channel.size()

    override fun read(pos: Long, dst: ByteArray, off: Int, len: Int): Int {
        if (pos >= size || len == 0) return 0
        val n = channel.read(ByteBuffer.wrap(dst, off, len), pos)
        return if (n < 0) 0 else n
    }

    override fun close() {
        channel.close()
        closeable?.close()
    }

    companion object {
        fun open(file: File): ChannelByteSource {
            val raf = RandomAccessFile(file, "r")
            return ChannelByteSource(raf.channel, raf)
        }
    }
}

class ArrayByteSource(private val data: ByteArray) : ByteSource {
    override val size: Long get() = data.size.toLong()
    override fun read(pos: Long, dst: ByteArray, off: Int, len: Int): Int {
        if (pos >= data.size) return 0
        val n = minOf(len.toLong(), data.size - pos).toInt()
        System.arraycopy(data, pos.toInt(), dst, off, n)
        return n
    }
}

private class SourceInputStream(private val src: ByteSource, private var pos: Long, bufferSize: Int) : InputStream() {
    private val buf = ByteArray(bufferSize)
    private var bufPos = 0
    private var bufLen = 0

    private fun fill(): Boolean {
        if (bufPos < bufLen) return true
        val n = src.read(pos, buf, 0, buf.size)
        if (n <= 0) return false
        pos += n
        bufPos = 0
        bufLen = n
        return true
    }

    override fun read(): Int = if (fill()) buf[bufPos++].toInt() and 0xFF else -1

    override fun read(b: ByteArray, off: Int, len: Int): Int {
        if (len == 0) return 0
        if (!fill()) return -1
        val n = minOf(len, bufLen - bufPos)
        System.arraycopy(buf, bufPos, b, off, n)
        bufPos += n
        return n
    }
}

internal fun InputStream.readFullyOrThrow(b: ByteArray, off: Int, len: Int) {
    var done = 0
    while (done < len) {
        val n = read(b, off + done, len - done)
        if (n < 0) throw IOException("Unexpected end of stream")
        done += n
    }
}

internal fun ByteArray.u8(i: Int) = this[i].toInt() and 0xFF
internal fun ByteArray.u16le(i: Int) = u8(i) or (u8(i + 1) shl 8)
internal fun ByteArray.u32le(i: Int): Long = (u16le(i).toLong()) or (u16le(i + 2).toLong() shl 16)
internal fun ByteArray.i32le(i: Int): Int = u16le(i) or (u16le(i + 2) shl 16)
internal fun ByteArray.u64le(i: Int): Long = u32le(i) or (u32le(i + 4) shl 32)
