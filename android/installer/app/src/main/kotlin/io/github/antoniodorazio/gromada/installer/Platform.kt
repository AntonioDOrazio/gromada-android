package io.github.antoniodorazio.gromada.installer

import android.content.ActivityNotFoundException
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.provider.Settings
import io.github.antoniodorazio.gromada.retroarch.RetroArch
import java.io.File

/** Android-side helpers: storage paths, RetroArch detection and intents. */
object Platform {
    val storageRoot: String get() = Environment.getExternalStorageDirectory().absolutePath

    /** Android 11+: "All files access"; Android 10: legacy storage + WRITE_EXTERNAL_STORAGE. */
    fun hasAllFilesAccess(ctx: Context): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) Environment.isExternalStorageManager()
        else ctx.checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED

    @android.annotation.TargetApi(Build.VERSION_CODES.R)
    fun allFilesAccessIntent(ctx: Context) =
        Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:${ctx.packageName}"))

    fun displayName(ctx: Context, uri: Uri): String =
        ctx.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        } ?: uri.lastPathSegment ?: "?"

    /**
     * Filesystem path of a SAF document or tree URI from the local storage providers
     * ("primary:Download/x.cue" -> /storage/emulated/0/Download/x.cue), or null.
     */
    fun pathOf(uri: Uri): String? {
        val id = try {
            // tree URI from ACTION_OPEN_DOCUMENT_TREE: content://authority/tree/<id>
            if (DocumentsContract.isTreeUri(uri) && uri.pathSegments.size == 2)
                DocumentsContract.getTreeDocumentId(uri)
            else DocumentsContract.getDocumentId(uri)
        } catch (e: IllegalArgumentException) {
            return null
        }
        return when (uri.authority) {
            "com.android.externalstorage.documents" -> {
                val volume = id.substringBefore(':')
                val rel = id.substringAfter(':', "")
                val base = if (volume.equals("primary", true)) storageRoot else "/storage/$volume"
                if (rel.isEmpty()) base else "$base/$rel"
            }
            "com.android.providers.downloads.documents" -> if (id.startsWith("raw:")) id.removePrefix("raw:") else null
            else -> null
        }
    }

    /** Installed RetroArch builds we know how to drive. */
    fun findRetroArch(ctx: Context): List<RetroArch.Install> {
        val pm = ctx.packageManager
        return RetroArch.PACKAGES.mapNotNull { pkg ->
            try {
                val info = pm.getPackageInfo(pkg, PackageManager.GET_PERMISSIONS)
                val app = info.applicationInfo ?: return@mapNotNull null
                val installer = try {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) pm.getInstallSourceInfo(pkg).installingPackageName
                    else @Suppress("DEPRECATION") pm.getInstallerPackageName(pkg)
                } catch (e: Exception) {
                    null
                }
                RetroArch.Install(
                    packageName = pkg,
                    versionName = info.versionName ?: "?",
                    targetSdk = app.targetSdkVersion,
                    requestsAllFilesAccess = info.requestedPermissions?.contains("android.permission.MANAGE_EXTERNAL_STORAGE") == true,
                    dataDir = app.dataDir,
                    sourceDir = app.sourceDir,
                    installer = installer,
                )
            } catch (e: PackageManager.NameNotFoundException) {
                null
            }
        }
    }

    /** Asks RetroArch to copy [coreFile] into its private cores folder (CoreSideloadActivity). */
    fun sideloadCore(ctx: Context, ra: RetroArch.Install, coreFile: File, content: File? = null) {
        val intent = Intent().setComponent(ComponentName(ra.packageName, RetroArch.SIDELOAD_ACTIVITY))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        RetroArch.sideloadExtras(coreFile.absolutePath, content?.absolutePath).forEach { (k, v) -> intent.putExtra(k, v) }
        ctx.startActivity(intent)
    }

    /** Starts the game with the Gromada core, like a frontend (ES-DE, Daijisho) would. */
    fun launchGame(ctx: Context, ra: RetroArch.Install, content: File) {
        val ime = Settings.Secure.getString(ctx.contentResolver, Settings.Secure.DEFAULT_INPUT_METHOD)
        val intent = Intent(Intent.ACTION_MAIN)
            .setComponent(ComponentName(ra.packageName, RetroArch.ACTIVITY))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
        RetroArch.launchExtras(ra, storageRoot, content.absolutePath, ime).forEach { (k, v) -> intent.putExtra(k, v) }
        ctx.startActivity(intent)
    }

    fun openRetroArch(ctx: Context, ra: RetroArch.Install) {
        val intent = ctx.packageManager.getLaunchIntentForPackage(ra.packageName)
            ?: throw ActivityNotFoundException(ra.packageName)
        ctx.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }
}
