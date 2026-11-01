package io.github.antoniodorazio.gromada.installer

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import io.github.antoniodorazio.gromada.extract.ByteSource
import io.github.antoniodorazio.gromada.extract.ChannelByteSource
import io.github.antoniodorazio.gromada.extract.CueException
import io.github.antoniodorazio.gromada.extract.DiscImage
import io.github.antoniodorazio.gromada.extract.GameExtractor
import io.github.antoniodorazio.gromada.extract.Stage
import java.io.File
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The CD extraction, run on a background thread. A process-wide singleton so that it
 * survives activity re-creation; listeners are called on the main thread.
 */
object ExtractionJob {
    sealed class State {
        data object Idle : State()
        data class Running(val stage: Stage, val fraction: Double, val detail: String) : State()
        data class Done(val message: String) : State()
        data class Failed(val message: String) : State()
    }

    @Volatile
    var state: State = State.Idle
        private set

    private val main = Handler(Looper.getMainLooper())
    private val listeners = mutableSetOf<(State) -> Unit>()
    private val logListeners = mutableSetOf<(String) -> Unit>()
    private val cancel = AtomicBoolean()
    private var lastPost = 0L

    val running get() = state is State.Running

    fun observe(onState: (State) -> Unit, onLog: (String) -> Unit) {
        listeners += onState; logListeners += onLog
        onState(state)
    }

    fun remove(onState: (State) -> Unit, onLog: (String) -> Unit) {
        listeners -= onState; logListeners -= onLog
    }

    fun cancel() = cancel.set(true)

    private fun publish(s: State, force: Boolean = true) {
        state = s
        val now = System.nanoTime()
        if (!force && now - lastPost < 100_000_000) return // ~10 UI updates per second
        lastPost = now
        main.post { listeners.forEach { it(s) } }
    }

    private fun log(msg: String) = main.post { logListeners.forEach { it(msg) } }

    /**
     * Extracts the image made of the picked documents ([uris]) into [outDir].
     * A .cue is matched with the picked .bin files by name; if the .bin was not picked,
     * it is looked up next to the cue (needs all-files access). A lone .iso/.bin is read
     * as a data track (no music).
     */
    fun start(ctx: Context, uris: List<Uri>, outDir: File, music: Boolean) {
        if (running) return
        val app = ctx.applicationContext
        cancel.set(false)
        publish(State.Running(Stage.ANALYZE, 0.0, "Apertura dell'immagine…"))
        Thread({
            try {
                val names = uris.associateWith { Platform.displayName(app, it) }
                fun open(uri: Uri): ByteSource {
                    val pfd = app.contentResolver.openFileDescriptor(uri, "r") ?: error("Impossibile aprire $uri")
                    // Closing the channel closes this stream, which closes the descriptor (fdsan-safe).
                    val stream = ParcelFileDescriptor.AutoCloseInputStream(pfd)
                    return ChannelByteSource(stream.channel, stream)
                }
                val cueUri = uris.firstOrNull { names[it]!!.endsWith(".cue", true) }
                val image = if (cueUri != null) {
                    val cueText = app.contentResolver.openInputStream(cueUri)!!.use { String(it.readBytes(), Charsets.ISO_8859_1) }
                    val others = uris.filter { it != cueUri }
                    DiscImage.fromCue(cueText) { name, index, count ->
                        val byName = others.firstOrNull { names[it]!!.equals(name, true) }
                        val single = others.singleOrNull().takeIf { count == 1 }
                        val picked = byName ?: single ?: others.getOrNull(index)?.takeIf { others.size == count }
                        if (picked != null) {
                            log("$name → ${names[picked]}")
                            open(picked)
                        } else {
                            val dir = Platform.pathOf(cueUri)?.let { File(it).parentFile }
                            val file = dir?.let { d -> File(d, name).takeIf { it.isFile } ?: d.listFiles()?.firstOrNull { it.name.equals(name, true) } }
                                ?: throw CueException("Il cue cita \"$name\": selezionalo insieme al .cue")
                            log("$name → $file")
                            ChannelByteSource.open(file)
                        }
                    }
                } else {
                    val single = uris.singleOrNull() ?: throw CueException("Seleziona il file .cue (e il .bin) oppure un solo .iso")
                    log("Immagine senza cue: solo traccia dati, niente musica")
                    DiscImage.fromDataImage(open(single))
                }
                image.use {
                    for (t in image.tracks) log(t.toString())
                    val result = GameExtractor(image, ::log).extract(
                        outDir, music,
                        progress = { stage, done, total, detail ->
                            val f = if (total > 0) done.toDouble() / total else 0.0
                            publish(State.Running(stage, f, detail), force = false)
                        },
                        cancelled = { cancel.get() },
                    )
                    publish(State.Done("Fatto: ${result.files.size} file del gioco" +
                        if (result.musicTracks.isEmpty()) ", nessuna traccia musicale" else ", musica tracce ${result.musicTracks.first()}–${result.musicTracks.last()}"))
                }
            } catch (e: CancellationException) {
                publish(State.Failed("Annullato"))
            } catch (e: Throwable) {
                log("Errore: $e")
                publish(State.Failed(e.message ?: e.toString()))
            }
        }, "gromada-extract").start()
    }
}
