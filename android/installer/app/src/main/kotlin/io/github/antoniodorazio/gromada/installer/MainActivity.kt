package io.github.antoniodorazio.gromada.installer

import android.app.Activity
import android.app.AlertDialog
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import io.github.antoniodorazio.gromada.extract.GameExtractor
import io.github.antoniodorazio.gromada.extract.Stage
import io.github.antoniodorazio.gromada.retroarch.RetroArch
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.concurrent.thread

class MainActivity : Activity() {
    private val prefs by lazy { getSharedPreferences("installer", Context.MODE_PRIVATE) }

    private var gameDir: File
        get() = File(prefs.getString("gameDir", null) ?: "${Platform.storageRoot}/Gromada")
        set(v) = prefs.edit().putString("gameDir", v.absolutePath).apply()
    private var playlistDir: File
        get() = File(prefs.getString("playlistDir", null) ?: RetroArch.defaultPlaylistDir(Platform.storageRoot))
        set(v) = prefs.edit().putString("playlistDir", v.absolutePath).apply()
    private val coreDir get() = File(gameDir, "core")
    private val coreFile get() = File(coreDir, RetroArch.CORE_FILE)

    private var release: Release? = null
    private var retroArchs: List<RetroArch.Install> = emptyList()

    private lateinit var log: TextView
    private val stateListener: (ExtractionJob.State) -> Unit = { onJobState(it) }
    private val logListener: (String) -> Unit = { appendLog(it) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        log = findViewById(R.id.log)

        findViewById<EditText>(R.id.repo).setText(prefs.getString("repo", BuildConfig.DEFAULT_REPO))
        findViewById<Button>(R.id.grant).setOnClickListener { requestStorage() }
        findViewById<Button>(R.id.changeDir).setOnClickListener { pickTree(REQ_GAME_DIR) }
        findViewById<Button>(R.id.changePlaylistDir).setOnClickListener { pickTree(REQ_PLAYLIST_DIR) }
        findViewById<Button>(R.id.pickImage).setOnClickListener { pickImage() }
        findViewById<Button>(R.id.cancel).setOnClickListener { ExtractionJob.cancel() }
        findViewById<Button>(R.id.checkUpdates).setOnClickListener { checkRelease() }
        findViewById<Button>(R.id.downloadCore).setOnClickListener { downloadCore() }
        findViewById<Button>(R.id.openRetroArch).setOnClickListener { withRetroArch { Platform.openRetroArch(this, it) } }
        findViewById<Button>(R.id.installCore).setOnClickListener { installCore() }
        findViewById<Button>(R.id.writePlaylist).setOnClickListener { writePlaylist() }
        findViewById<Button>(R.id.play).setOnClickListener { play() }
        findViewById<RadioGroup>(R.id.raPackages).setOnCheckedChangeListener { group, id ->
            group.findViewById<RadioButton>(id)?.tag?.let { prefs.edit().putString("raPackage", it as String).apply() }
            refresh()
        }
        appendLog("Installer ${BuildConfig.VERSION_NAME}")
        checkRelease()
    }

    override fun onStart() {
        super.onStart()
        ExtractionJob.observe(stateListener, logListener)
        refresh()
    }

    override fun onStop() {
        ExtractionJob.remove(stateListener, logListener)
        prefs.edit().putString("repo", repo()).apply()
        super.onStop()
    }

    private fun repo() = findViewById<EditText>(R.id.repo).text.toString().trim().ifEmpty { BuildConfig.DEFAULT_REPO }

    // ---------------- status ----------------

    private fun refresh() {
        val storageOk = Platform.hasAllFilesAccess(this)
        findViewById<TextView>(R.id.permissionStatus).text =
            if (storageOk) "✓ Accesso ai file concesso." else getString(R.string.permission_why)
        findViewById<Button>(R.id.grant).visibility = if (storageOk) View.GONE else View.VISIBLE

        val dir = gameDir
        findViewById<TextView>(R.id.gameDir).text = "${getString(R.string.game_dir_label)}: $dir"
        val installed = storageOk && GameExtractor.isGameFolder(dir)
        val music = File(dir, "music").listFiles()?.count { it.name.endsWith(".flac", true) || it.name.endsWith(".ogg", true) } ?: 0
        findViewById<TextView>(R.id.gameStatus).text = when {
            !storageOk -> "Concedi prima il permesso."
            installed -> "✓ Gioco presente" + if (music > 0) " (musica: $music tracce)." else " (senza musica)."
            else -> "Gioco non ancora estratto."
        }

        // core
        val downloaded = prefs.getString("coreCommit", null)?.takeIf { coreFile.isFile }
        val sideloaded = prefs.getString("sideloadedCommit", null)
        val r = release
        findViewById<TextView>(R.id.coreStatus).text = buildString {
            append(if (r != null) "Nightly: ${r.name}, ${formatDate(r.publishedAt)}\n" else "Nightly: non ancora controllata\n")
            append(if (downloaded != null) "Scaricato: ${downloaded.take(7)} → $coreFile\n" else "Core non ancora scaricato.\n")
            if (r != null && downloaded != null && r.commit != downloaded) append("⬆ È disponibile un core più recente.\n")
            if (downloaded != null) append(
                if (sideloaded == downloaded) "✓ Installato in RetroArch (${downloaded.take(7)})."
                else "Da installare in RetroArch (passo 4)."
            )
        }
        findViewById<Button>(R.id.updateApp).visibility =
            if (r?.asset(GitHub.APK_ASSET) != null && BuildConfig.GIT_COMMIT != "dev" && r.commit != BuildConfig.GIT_COMMIT) View.VISIBLE else View.GONE
        findViewById<Button>(R.id.updateApp).setOnClickListener {
            r?.asset(GitHub.APK_ASSET)?.let { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(it.url))) }
        }

        refreshRetroArch()
    }

    private fun refreshRetroArch() {
        retroArchs = Platform.findRetroArch(this)
        val group = findViewById<RadioGroup>(R.id.raPackages)
        val chosen = selectedRetroArch()
        if (group.childCount != retroArchs.size) {
            group.removeAllViews()
            for (ra in retroArchs) group.addView(RadioButton(this).apply {
                id = View.generateViewId()
                tag = ra.packageName
                text = "${ra.packageName} ${ra.versionName}"
                textSize = 16f
            })
        }
        for (i in 0 until group.childCount) {
            val b = group.getChildAt(i) as RadioButton
            if (b.tag == chosen?.packageName && !b.isChecked) b.isChecked = true
        }
        findViewById<TextView>(R.id.raStatus).text = when {
            retroArchs.isEmpty() -> "RetroArch non è installato. Installa la versione dal sito retroarch.com (RetroArch64 / AArch64 per Retroid), aprilo una volta e concedigli l'accesso ai file."
            chosen == null -> ""
            else -> describe(chosen)
        }
        findViewById<TextView>(R.id.manualSteps).text = getString(R.string.manual_steps) +
            "\nFile del core: $coreFile\nPlaylist: ${File(playlistDir, RetroArch.PLAYLIST_FILE)}"
    }

    private fun describe(ra: RetroArch.Install) = buildString {
        append("Versione ${ra.versionName}, targetSdk ${ra.targetSdk}")
        append(if (ra.fromPlayStore) ", installato da Google Play.\n" else ".\n")
        append(when (ra.storage) {
            RetroArch.StorageAccess.LEGACY -> "✓ Compatibile: legge i file condivisi se ha il permesso di archiviazione (aprilo una volta e accetta)."
            RetroArch.StorageAccess.ALL_FILES -> "Compatibile se a RetroArch è concesso “Accesso a tutti i file” (Impostazioni Android → App → RetroArch → Autorizzazioni)."
            RetroArch.StorageAccess.SCOPED -> "⚠ Questa build (Google Play) non può leggere file fuori dalle sue cartelle: non può caricare né il core né il gioco. Installa RetroArch dal sito retroarch.com."
        })
    }

    private fun selectedRetroArch(): RetroArch.Install? {
        val pkg = prefs.getString("raPackage", null)
        return retroArchs.firstOrNull { it.packageName == pkg }
            ?: retroArchs.firstOrNull { it.storage != RetroArch.StorageAccess.SCOPED }
            ?: retroArchs.firstOrNull()
    }

    private fun formatDate(iso: String): String = try {
        val d = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.ROOT).apply { timeZone = java.util.TimeZone.getTimeZone("UTC") }.parse(iso)
        SimpleDateFormat("d MMM yyyy HH:mm", Locale.ITALIAN).format(d ?: Date())
    } catch (e: Exception) {
        iso
    }

    private fun appendLog(msg: String) {
        log.append(msg + "\n")
    }

    private fun toast(msg: String) {
        appendLog(msg)
        AlertDialog.Builder(this).setMessage(msg).setPositiveButton(android.R.string.ok, null).show()
    }

    // ---------------- permission and folders ----------------

    private fun requestStorage() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                startActivity(Platform.allFilesAccessIntent(this))
            } catch (e: ActivityNotFoundException) {
                startActivity(Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            }
        } else {
            requestPermissions(arrayOf(android.Manifest.permission.WRITE_EXTERNAL_STORAGE), REQ_PERMISSION)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        refresh()
    }

    private fun pickTree(req: Int) {
        try {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), req)
        } catch (e: ActivityNotFoundException) {
            toast("Nessun selettore di cartelle disponibile.")
        }
    }

    private fun pickImage() {
        if (!Platform.hasAllFilesAccess(this)) return toast("Concedi prima l'accesso ai file (passo 1).")
        if (ExtractionJob.running) return
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType("*/*")
            .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        startActivityForResult(intent, REQ_IMAGE)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK || data == null) return
        when (requestCode) {
            REQ_IMAGE -> {
                val uris = buildList {
                    data.clipData?.let { c -> for (i in 0 until c.itemCount) add(c.getItemAt(i).uri) }
                    if (isEmpty()) data.data?.let { add(it) }
                }
                if (uris.isEmpty()) return
                val target = gameDir
                // Refuse to mix the game into an unrelated folder (our own core/ subfolder is fine).
                val foreign = target.listFiles()?.filter { it.name != "core" && it.name != GameExtractor.MANIFEST }.orEmpty()
                if (foreign.isNotEmpty() && !GameExtractor.isGameFolder(target)) {
                    return toast("La cartella $target esiste e non è vuota: scegli una cartella vuota o la cartella del gioco.")
                }
                appendLog("Estrazione in $target")
                ExtractionJob.start(this, uris, target, findViewById<CheckBox>(R.id.music).isChecked)
            }
            REQ_GAME_DIR, REQ_PLAYLIST_DIR -> {
                val path = data.data?.let(Platform::pathOf)
                    ?: return toast("Scegli una cartella della memoria interna o della scheda SD.")
                if (requestCode == REQ_GAME_DIR) gameDir = File(path) else playlistDir = File(path)
                refresh()
            }
        }
    }

    private fun onJobState(s: ExtractionJob.State) {
        val bar = findViewById<ProgressBar>(R.id.progress)
        val text = findViewById<TextView>(R.id.progressText)
        val cancel = findViewById<Button>(R.id.cancel)
        val running = s is ExtractionJob.State.Running
        bar.visibility = if (running) View.VISIBLE else View.GONE
        cancel.visibility = if (running) View.VISIBLE else View.GONE
        findViewById<Button>(R.id.pickImage).isEnabled = !running
        text.visibility = if (s is ExtractionJob.State.Idle) View.GONE else View.VISIBLE
        if (running) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        when (s) {
            is ExtractionJob.State.Running -> {
                bar.progress = (s.fraction * 1000).toInt()
                val stage = when (s.stage) {
                    Stage.ANALYZE -> "Analisi"
                    Stage.GAME_FILES -> "File del gioco"
                    Stage.MUSIC -> "Musica (FLAC)"
                    Stage.DONE -> "Fine"
                }
                text.text = "$stage: ${(s.fraction * 100).toInt()}%  ${s.detail}"
            }
            is ExtractionJob.State.Done -> { text.text = "✓ ${s.message}"; refresh() }
            is ExtractionJob.State.Failed -> { text.text = "✗ ${s.message}"; refresh() }
            ExtractionJob.State.Idle -> Unit
        }
    }

    // ---------------- core ----------------

    private fun checkRelease(then: (() -> Unit)? = null) {
        val repo = repo()
        thread {
            try {
                val r = GitHub.fetchRelease(repo)
                runOnUiThread {
                    release = r
                    appendLog("Nightly ${r.name} (${r.commit.take(7)}): ${r.assets.joinToString { it.name }}")
                    refresh()
                    then?.invoke()
                }
            } catch (e: Exception) {
                runOnUiThread { appendLog("Controllo aggiornamenti non riuscito: ${e.message}") }
            }
        }
    }

    private fun downloadCore() {
        if (!Platform.hasAllFilesAccess(this)) return toast("Concedi prima l'accesso ai file (passo 1).")
        val r = release ?: return checkRelease { downloadCore() }
        val core = r.asset(GitHub.CORE_ASSET) ?: return toast("La release ${r.name} non contiene ${GitHub.CORE_ASSET}.")
        val info = r.asset(GitHub.INFO_ASSET)
        val button = findViewById<Button>(R.id.downloadCore)
        button.isEnabled = false
        thread {
            try {
                GitHub.download(core, coreFile) { done, total ->
                    runOnUiThread { button.text = "Download… ${if (total > 0) done * 100 / total else 0}%" }
                }
                if (info != null) GitHub.download(info, File(coreDir, RetroArch.INFO_FILE))
                prefs.edit().putString("coreCommit", r.commit).putString("coreAsset", core.updatedAt).apply()
                runOnUiThread { appendLog("Core ${r.commit.take(7)} scaricato in $coreFile") }
            } catch (e: Exception) {
                runOnUiThread { toast("Download non riuscito: ${e.message}") }
            } finally {
                runOnUiThread {
                    button.isEnabled = true
                    button.setText(R.string.download_core)
                    refresh()
                }
            }
        }
    }

    // ---------------- RetroArch ----------------

    private fun withRetroArch(block: (RetroArch.Install) -> Unit) {
        val ra = selectedRetroArch() ?: return toast("RetroArch non è installato.")
        try {
            block(ra)
        } catch (e: ActivityNotFoundException) {
            toast("RetroArch non accetta la richiesta (${e.message}). Usa il procedimento manuale descritto sotto.")
        } catch (e: SecurityException) {
            toast("RetroArch ha rifiutato la richiesta (${e.message}). Usa il procedimento manuale descritto sotto.")
        }
    }

    private fun installCore() = withRetroArch { ra ->
        if (!coreFile.isFile) return@withRetroArch toast("Scarica prima il core (passo 3).")
        val go = {
            Platform.sideloadCore(this, ra, coreFile)
            prefs.edit().putString("sideloadedCommit", prefs.getString("coreCommit", null))
                .putString("sideloadedPackage", ra.packageName).apply()
            appendLog("Richiesta installazione del core a ${ra.packageName}")
        }
        val msg = "RetroArch copierà il core nella sua cartella privata e poi si aprirà (o si chiuderà, se era già aperto).\n\n" +
            "Se compare “Done!” e RetroArch si apre, esci da RetroArch (Menu principale → Esci) e torna qui.\n" +
            "Se compare un errore “Input file doesn't exist”, RetroArch non ha il permesso di leggere i file: aprilo e concedilo."
        if (ra.storage == RetroArch.StorageAccess.SCOPED)
            AlertDialog.Builder(this).setMessage(describe(ra) + "\n\nProvare comunque?")
                .setPositiveButton("Prova") { _, _ -> go() }.setNegativeButton(android.R.string.cancel, null).show()
        else AlertDialog.Builder(this).setMessage(msg).setPositiveButton("Continua") { _, _ -> go() }
            .setNegativeButton(android.R.string.cancel, null).show()
    }

    private fun writePlaylist() = withRetroArch { ra ->
        val content = File(gameDir, "gromada.exe")
        if (!content.isFile) return@withRetroArch toast("Estrai prima il gioco (passo 2).")
        try {
            val dir = playlistDir
            if (!dir.isDirectory && !dir.mkdirs()) throw java.io.IOException("Impossibile creare $dir")
            File(dir, RetroArch.PLAYLIST_FILE).writeText(RetroArch.playlist(content.absolutePath, ra.corePath))
            toast("Playlist scritta: ${File(dir, RetroArch.PLAYLIST_FILE)}\nIn RetroArch la trovi tra le playlist (riavvia RetroArch se era aperto). " +
                "Se hai cambiato la cartella delle playlist in RetroArch, indica qui la stessa cartella.")
        } catch (e: Exception) {
            toast("Playlist non scritta: ${e.message}")
        }
    }

    private fun play() = withRetroArch { ra ->
        val content = File(gameDir, "gromada.exe")
        if (!content.isFile) return@withRetroArch toast("Estrai prima il gioco (passo 2).")
        if (prefs.getString("sideloadedPackage", null) != ra.packageName)
            appendLog("Attenzione: il core non risulta installato in ${ra.packageName} (passo 4).")
        Platform.launchGame(this, ra, content)
    }

    companion object {
        private const val REQ_IMAGE = 1
        private const val REQ_GAME_DIR = 2
        private const val REQ_PLAYLIST_DIR = 3
        private const val REQ_PERMISSION = 4
    }
}
