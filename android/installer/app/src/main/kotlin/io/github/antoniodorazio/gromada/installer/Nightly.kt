package io.github.antoniodorazio.gromada.installer

import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/** The rolling "nightly" GitHub release that CI publishes from main. */
data class Release(
    val tag: String,
    val name: String,
    /** Commit the release was created from (`gh release create --target $GITHUB_SHA`). */
    val commit: String,
    val publishedAt: String,
    val assets: List<Asset>,
) {
    data class Asset(val id: Long, val name: String, val url: String, val size: Long, val updatedAt: String)

    fun asset(name: String) = assets.firstOrNull { it.name == name }
}

object GitHub {
    const val CORE_ASSET = "gromada_libretro_android.so"
    const val INFO_ASSET = "gromada_libretro.info"
    const val APK_ASSET = "gromada-installer.apk"

    /** GET /repos/{repo}/releases/tags/{tag} (REST, unauthenticated: 60 requests/hour). */
    fun fetchRelease(repo: String, tag: String = "nightly"): Release {
        require(Regex("[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+").matches(repo)) { "Repository non valido: $repo" }
        val conn = open("https://api.github.com/repos/$repo/releases/tags/$tag")
        conn.setRequestProperty("Accept", "application/vnd.github+json")
        conn.setRequestProperty("X-GitHub-Api-Version", "2022-11-28")
        try {
            when (val code = conn.responseCode) {
                200 -> Unit
                404 -> throw IOException("Release '$tag' non trovata in $repo")
                403, 429 -> throw IOException("GitHub ha limitato le richieste (HTTP $code): riprova più tardi")
                else -> throw IOException("GitHub ha risposto HTTP $code")
            }
            val json = JSONObject(conn.inputStream.bufferedReader().use { it.readText() })
            val assets = json.getJSONArray("assets")
            return Release(
                tag = json.getString("tag_name"),
                name = json.optString("name", tag),
                commit = json.optString("target_commitish", ""),
                publishedAt = json.optString("published_at", ""),
                assets = (0 until assets.length()).map { i ->
                    val a = assets.getJSONObject(i)
                    Release.Asset(a.getLong("id"), a.getString("name"), a.getString("browser_download_url"),
                        a.getLong("size"), a.optString("updated_at", ""))
                },
            )
        } finally {
            conn.disconnect()
        }
    }

    /** Downloads [asset] to [target] (via a .part file); checks the size GitHub reports. */
    fun download(asset: Release.Asset, target: File, onProgress: (Long, Long) -> Unit = { _, _ -> }) {
        target.parentFile?.mkdirs()
        val part = File(target.parentFile, target.name + ".part")
        val conn = open(asset.url)
        try {
            if (conn.responseCode != 200) throw IOException("Download di ${asset.name}: HTTP ${conn.responseCode}")
            var done = 0L
            conn.inputStream.use { input ->
                FileOutputStream(part).use { out ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        done += n
                        onProgress(done, asset.size)
                    }
                    out.fd.sync()
                }
            }
            if (done != asset.size) throw IOException("Download di ${asset.name} incompleto ($done di ${asset.size} byte)")
            if (target.exists() && !target.delete()) throw IOException("Impossibile sostituire $target")
            if (!part.renameTo(target)) throw IOException("Impossibile rinominare $part")
        } catch (e: Exception) {
            part.delete()
            throw e
        } finally {
            conn.disconnect()
        }
    }

    private fun open(url: String): HttpURLConnection {
        // HttpURLConnection follows the https -> https redirect to GitHub's asset storage.
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 15_000
        conn.readTimeout = 30_000
        conn.instanceFollowRedirects = true
        conn.setRequestProperty("User-Agent", "gromada-installer/${BuildConfig.VERSION_NAME}")
        return conn
    }
}
