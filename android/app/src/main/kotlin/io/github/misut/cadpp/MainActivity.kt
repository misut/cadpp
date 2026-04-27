package io.github.misut.cadpp

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.util.Log
import com.google.androidgamesdk.GameActivity
import java.io.File

// All app logic lives in cadpp_android_main.cpp via GameActivity's
// native_app_glue bridge. The JVM side now also bridges
// `phenotype::native::dialog::open_file` to the Storage Access
// Framework picker so the in-app "Open..." button can hand cad++ a
// new DWG path on tap. Phenotype's install hook (called from the NDK
// glue at startup) registers `onFileDialogResult` as a JNI native on
// this class, which is why that method is `external`.
class MainActivity : GameActivity() {

    companion object {
        private const val TAG = "cadpp"
        private const val REQUEST_OPEN_DOCUMENT = 0x0DCC

        @Volatile private var instance: MainActivity? = null

        // Phenotype calls this from the render thread when the user
        // taps cad++'s "Open..." button. We hop to the UI thread
        // before launching the SAF intent because startActivityForResult
        // can only run there. The cookie is opaque — it identifies
        // which native callback gets invoked when the result arrives.
        @JvmStatic
        @Suppress("unused")
        fun openFileDialog(cookie: Int, filterExtensions: String) {
            val activity = instance
            if (activity == null) {
                Log.w(TAG, "openFileDialog($cookie): no MainActivity instance")
                onFileDialogResult(cookie, null)
                return
            }
            activity.runOnUiThread {
                activity.startFileDialog(cookie, filterExtensions)
            }
        }

        // JNI-bound to phenotype_android_dialog_native_on_result via
        // phenotype_android_install_file_dialog_handler at startup —
        // see cadpp_android_main.cpp.
        @JvmStatic
        external fun onFileDialogResult(cookie: Int, path: String?)
    }

    private var pendingCookie: Int = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        instance = this
    }

    override fun onDestroy() {
        if (instance === this) instance = null
        super.onDestroy()
    }

    private fun startFileDialog(cookie: Int, filterExtensions: String) {
        pendingCookie = cookie
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            // SAF cannot reliably filter by extension across all
            // providers (Drive, Downloads, third-party clouds…), so
            // accept any file. The native parser surfaces a clear
            // error message if the user picks something that isn't
            // a DWG. `filterExtensions` is currently logged for
            // diagnostics; the picker UI will show every file type.
            type = "*/*"
        }
        Log.i(TAG, "startFileDialog($cookie, '$filterExtensions')")
        startActivityForResult(intent, REQUEST_OPEN_DOCUMENT)
    }

    @Deprecated("ActivityResultContracts is the modern replacement, but " +
                "ACTION_OPEN_DOCUMENT works fine through GameActivity's " +
                "legacy onActivityResult path and the migration would " +
                "complicate the JNI cookie threading without payoff today.")
    override fun onActivityResult(requestCode: Int, resultCode: Int,
                                  data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_OPEN_DOCUMENT) return

        val cookie = pendingCookie
        pendingCookie = 0

        if (resultCode != Activity.RESULT_OK) {
            Log.i(TAG, "file dialog cancelled (cookie=$cookie)")
            onFileDialogResult(cookie, null)
            return
        }
        val uri = data?.data
        if (uri == null) {
            Log.w(TAG, "file dialog returned RESULT_OK but data.uri is null")
            onFileDialogResult(cookie, null)
            return
        }

        val cachedPath = stageContentToCache(uri)
        if (cachedPath == null) {
            Log.e(TAG, "failed to stage $uri to cache dir")
            onFileDialogResult(cookie, null)
            return
        }
        Log.i(TAG, "file dialog picked: $uri -> $cachedPath")
        onFileDialogResult(cookie, cachedPath)
    }

    // libredwg's public API only takes a filesystem path, so we copy
    // the picked content URI to a unique file under cacheDir. Every
    // pick gets a fresh file so a re-pick doesn't surprise the parser
    // mid-read; cacheDir gets pruned by Android when storage is low.
    private fun stageContentToCache(uri: Uri): String? {
        return try {
            val displayName = queryDisplayName(uri) ?: "picked.dwg"
            val safe = displayName
                .substringAfterLast('/')
                .take(64)
                .replace(Regex("[^A-Za-z0-9._-]"), "_")
                .ifBlank { "picked.dwg" }
            val outFile = File(cacheDir, "picked-${System.currentTimeMillis()}-$safe")
            contentResolver.openInputStream(uri)?.use { input ->
                outFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            } ?: return null
            outFile.absolutePath
        } catch (e: Exception) {
            Log.e(TAG, "stageContentToCache failed for $uri", e)
            null
        }
    }

    private fun queryDisplayName(uri: Uri): String? {
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            if (!cursor.moveToFirst()) return null
            val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (idx < 0) return null
            return cursor.getString(idx)
        }
        return null
    }
}
