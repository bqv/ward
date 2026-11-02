package uk.xa0.ward.extension.runtime

import android.content.Context
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.util.Log
import dalvik.system.DexClassLoader
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.json.JSONObject
import uk.xa0.ward.data.ConfigStore
import uk.xa0.ward.extension.DshExtension
import uk.xa0.ward.extension.EXTENSION_ENTRY_CLASS
import uk.xa0.ward.extension.ExtensionRegistry
import uk.xa0.ward.extension.ExtensionScope
import uk.xa0.ward.extension.ExtensionTrust
import uk.xa0.ward.extension.RouterSession
import uk.xa0.ward.extension.SessionTab
import uk.xa0.ward.extension.SessionUsage
import uk.xa0.ward.extension.builtin.BuiltInExtensions

/** A loaded tab, tagged so the session strip can key it and the settings list can name it. */
data class AvailableExtensionTab(
    val extensionId: String,
    val extensionTitle: String,
    val tab: SessionTab,
) {
    /**
     * Unique across extensions even when two use the same tab id. The length
     * prefix keeps `(id, tab.id)` pairs unambiguous even if an id contains `:`.
     */
    val key: String get() = "ext:${extensionId.length}:$extensionId:${tab.id}"
}

/** One row in Settings → Extensions: built-in or installed APK, loaded or failed. */
data class ExtensionRow(
    val packageName: String,
    val extensionId: String? = null,
    val title: String,
    /** The ABI version, when the entry point was reached. */
    val version: Int? = null,
    /** The APK's `versionName`, for the reader's own bookkeeping. */
    val apkVersionName: String? = null,
    val certSha256: String? = null,
    val builtIn: Boolean = false,
    val enabled: Boolean = true,
    val tabs: List<SessionTab> = emptyList(),
    /** Why this extension did not load; an installed row has null. */
    val failure: String? = null,
) {
    val loaded: Boolean get() = failure == null && extensionId != null
    val removable: Boolean get() = !builtIn
}

/** An APK already in the adb-readable inbox, waiting for a Rescan and an Import tap. */
data class InboxExtension(
    val path: String,
    val packageName: String? = null,
    val title: String? = null,
    val apkVersionName: String? = null,
    val certSha256: String? = null,
    val decision: ExtensionTrust.Decision? = null,
    val failure: String? = null,
)

/**
 * A candidate that has been read and fingerprinted but not copied into private
 * storage yet. The trust dialog is drawn from this; [ExtensionRuntime.confirmPendingInstall]
 * re-reads the source before trusting it.
 */
data class PendingExtensionInstall(
    val sourcePath: String,
    /** The inbox file this came from, deleted only after a successful install. */
    val fromInboxPath: String? = null,
    val packageName: String,
    val title: String,
    val apkVersionName: String? = null,
    val certSha256: String,
    /** True when this package already has an installed APK (an update). */
    val update: Boolean,
)

/** Everything Settings → Extensions draws, and the enabled tabs the strip may draw. */
data class ExtensionsUiState(
    val loading: Boolean = true,
    val rows: List<ExtensionRow> = emptyList(),
    val tabs: List<AvailableExtensionTab> = emptyList(),
    val inbox: List<InboxExtension> = emptyList(),
    val pending: PendingExtensionInstall? = null,
    val busy: Boolean = false,
    val error: String? = null,
    val notice: String? = null,
)

/**
 * The runtime half of the extension ABI: discovery, trusted loading, installation,
 * enablement and the session-tab list.
 *
 * A bad extension is isolated at every step. The APK is read (package
 * information and signing certificate) before any of its code runs; a refused or
 * broken archive is recorded as a row with [ExtensionRow.failure] and the scan
 * carries on. `DshApplication` starts this on the IO dispatcher, so a slow or
 * missing archive cannot delay or crash app startup.
 *
 * Loading uses [DexClassLoader] with the app's own class loader as parent, which
 * is what lets the APK's `Entry` implement [DshExtension] from this app. The
 * consequence is stated in the ABI: loaded code is *not* sandboxed — it runs with
 * this app's whole identity.
 */
class ExtensionRuntime(
    context: Context,
    private val configStore: ConfigStore,
    private val rpcCall: suspend (String, JSONObject) -> Any?,
) {
    private val appContext = context.applicationContext
    private val packageManager = appContext.packageManager
    private val extensionRoot = File(appContext.filesDir, EXTENSIONS_DIR)
    private val stagingRoot = File(appContext.cacheDir, STAGING_DIR)
    private val codeCacheDir = File(appContext.codeCacheDir, CODE_CACHE_DIR)
    private val work = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    private val _state = MutableStateFlow(ExtensionsUiState())
    val state: StateFlow<ExtensionsUiState> = _state.asStateFlow()

    @Volatile
    private var started = false

    /** Load once, from `DshApplication.onCreate`. Re-entrant calls are ignored. */
    fun start() {
        if (started) return
        started = true
        work.launch {
            try {
                reload()
            } catch (t: Throwable) {
                Log.w(TAG, "extension scan failed", t)
                _state.update { it.copy(loading = false, error = "Extensions could not be read: ${messageOf(t)}") }
            }
        }
    }

    /** The reader's Rescan of installed archives. */
    fun refresh() {
        work.launch {
            try {
                reload()
            } catch (t: Throwable) {
                _state.update { it.copy(error = "Extensions could not be read: ${messageOf(t)}") }
            }
        }
    }

    /**
     * The scope handed to every contribution of every loaded extension.
     *
     * The four facts after [sessionId] are the app's own live reading — route,
     * router and usage — which the shell owns and an extension only observes.
     */
    fun scope(
        sessionId: StateFlow<String?>,
        providerId: StateFlow<String?>,
        localRoute: StateFlow<Boolean>,
        router: StateFlow<RouterSession?>,
        usage: StateFlow<SessionUsage?>,
    ): ExtensionScope = RuntimeExtensionScope(
        sessionId = sessionId,
        providerId = providerId,
        localRoute = localRoute,
        router = router,
        usage = usage,
        call = rpcCall,
    )

    // --------------------------------------------------------------- enablement

    fun setEnabled(extensionId: String, enabled: Boolean) {
        if (extensionId.isBlank()) return
        val config = configStore.load()
        val disabled = if (enabled) {
            config.disabledExtensionIds - extensionId
        } else {
            config.disabledExtensionIds + extensionId
        }
        configStore.save(config.copy(disabledExtensionIds = disabled))
        _state.update { current ->
            val rows = current.rows.map { row ->
                if (row.extensionId == extensionId) {
                    row.copy(enabled = enabled && row.failure == null)
                } else {
                    row
                }
            }
            current.copy(rows = rows, tabs = tabsFor(rows, disabled))
        }
    }

    // --------------------------------------------------------------- removal

    fun remove(packageName: String) {
        work.launch {
            _state.update { it.copy(busy = true, error = null, notice = null) }
            try {
                val dir = checkedExtensionDir(packageName)
                deleteTree(dir)
                _state.update { it.copy(notice = "Removed $packageName.") }
                reload()
            } catch (t: Throwable) {
                _state.update { it.copy(error = "Could not remove the extension: ${messageOf(t)}") }
            } finally {
                _state.update { it.copy(busy = false) }
            }
        }
    }

    // --------------------------------------------------------------- install

    /** Copy a picked APK into private staging and raise the trust prompt. */
    fun installFromUri(uri: Uri) {
        work.launch {
            _state.update { it.copy(busy = true, error = null, notice = null) }
            try {
                val staged = File(stagingRoot.apply { mkdirs() }, "extension-${System.nanoTime()}.apk")
                val opened = appContext.contentResolver.openInputStream(uri)
                    ?: throw IllegalStateException("The selected file could not be opened.")
                opened.use { input -> staged.outputStream().use { output -> input.copyTo(output) } }
                stage(staged, fromInboxPath = null)
            } catch (t: Throwable) {
                fail("Could not read the selected APK", t)
            }
        }
    }

    /** Copy one inbox APK into private staging and raise the same trust prompt. */
    fun importFromInbox(candidate: InboxExtension) {
        work.launch {
            _state.update { it.copy(busy = true, error = null, notice = null) }
            try {
                if (candidate.failure != null) {
                    throw IllegalStateException(candidate.failure)
                }
                val source = File(candidate.path)
                if (!source.isFile) throw IllegalStateException("The inbox APK is gone; rescan.")
                val staged = File(stagingRoot.apply { mkdirs() }, "extension-${System.nanoTime()}.apk")
                source.inputStream().use { input -> staged.outputStream().use { output -> input.copyTo(output) } }
                stage(staged, fromInboxPath = source.absolutePath)
            } catch (t: Throwable) {
                fail("Could not read the inbox APK", t)
            }
        }
    }

    /**
     * Confirm the trust dialog: re-read the staged APK, enforce the pin, copy it
     * into `filesDir/extensions/<package>/base.apk`, then rescan.
     */
    fun confirmPendingInstall() {
        work.launch {
            val pending = _state.value.pending ?: return@launch
            _state.update { it.copy(busy = true, error = null, notice = null) }
            try {
                val source = File(pending.sourcePath)
                val info = inspect(source)
                if (info.packageName != pending.packageName) {
                    throw IllegalStateException(
                        "The APK changed while the trust prompt was open (package " +
                            "${info.packageName} instead of ${pending.packageName}).",
                    )
                }
                if (ExtensionTrust.normalise(info.certSha256) !=
                    ExtensionTrust.normalise(pending.certSha256)
                ) {
                    throw IllegalStateException("The APK changed while the trust prompt was open.")
                }
                val config = configStore.load()
                val pins = try {
                    ExtensionTrust.pin(config.extensionTrust, info.packageName, info.certSha256)
                } catch (t: ExtensionTrust.ExtensionTrustException) {
                    throw IllegalStateException(t.message ?: "The signing certificate is not trusted.")
                }

                extensionRoot.mkdirs()
                val dir = checkedExtensionDir(info.packageName)
                if (!dir.exists()) dir.mkdirs()
                val temp = File(dir, "base.apk.tmp")
                if (temp.exists()) temp.delete()
                source.inputStream().use { input -> temp.outputStream().use { output -> input.copyTo(output) } }
                val destination = File(dir, "base.apk")
                if (destination.exists() && !destination.delete()) {
                    throw IllegalStateException("The installed APK could not be replaced.")
                }
                if (!temp.renameTo(destination)) {
                    temp.copyTo(destination, overwrite = true)
                    temp.delete()
                }
                // Android 14 requires dynamically loaded code to be read-only.
                destination.setReadOnly()

                configStore.save(config.copy(extensionTrust = pins))
                pending.fromInboxPath?.let { path -> runCatching { File(path).delete() } }
                source.delete()
                _state.update {
                    it.copy(
                        pending = null,
                        notice = "Installed ${info.title} (${info.packageName}).",
                    )
                }
                reload()
            } catch (t: Throwable) {
                Log.w(TAG, "extension install failed", t)
                _state.update {
                    it.copy(
                        pending = null,
                        error = "The extension was not installed: ${messageOf(t)}",
                    )
                }
            } finally {
                _state.update { it.copy(busy = false) }
            }
        }
    }

    /** Dismiss the trust prompt and discard the staged copy. */
    fun cancelPendingInstall() {
        val pending = _state.value.pending ?: return
        runCatching { File(pending.sourcePath).delete() }
        _state.update { it.copy(pending = null) }
    }

    // --------------------------------------------------------------- inbox

    /** Read the adb-readable inbox. No watcher: the reader presses Rescan. */
    fun rescanInbox() {
        work.launch {
            _state.update { it.copy(busy = true, error = null, notice = null) }
            try {
                val trust = configStore.load().extensionTrust
                val dir = inboxDir()
                val candidates = if (dir == null || !dir.isDirectory) {
                    emptyList()
                } else {
                    dir.listFiles { file -> file.isFile && file.name.endsWith(".apk", true) }
                        ?.sortedBy { it.name }
                        ?.map { file -> inspectInbox(file, trust) }
                        .orEmpty()
                }
                _state.update {
                    it.copy(
                        inbox = candidates,
                        notice = if (dir == null) {
                            "This device has no external files directory to import from."
                        } else {
                            "Found ${candidates.size} APK(s) in the import directory."
                        },
                    )
                }
            } catch (t: Throwable) {
                _state.update { it.copy(error = "The import directory could not be read: ${messageOf(t)}") }
            } finally {
                _state.update { it.copy(busy = false) }
            }
        }
    }

    fun clearMessage() {
        _state.update { it.copy(error = null, notice = null) }
    }

    // --------------------------------------------------------------- loading

    private fun reload() {
        val config = configStore.load()
        val disabled = config.disabledExtensionIds
        val pins = config.extensionTrust
        val registry = ExtensionRegistry()
        val rows = mutableListOf<ExtensionRow>()

        BuiltInExtensions.all.forEach { extension ->
            rows += register(extension, registry, null, null, builtIn = true)
        }

        val dirs = extensionRoot.listFiles()
            ?.filter { it.isDirectory }
            ?.sortedBy { it.name }
            .orEmpty()

        dirs.forEach { dir ->
            val apk = File(dir, "base.apk")
            if (!apk.isFile) {
                rows += ExtensionRow(
                    packageName = dir.name,
                    title = dir.name,
                    builtIn = false,
                    enabled = false,
                    failure = "No base.apk in this extension directory.",
                )
                return@forEach
            }
            try {
                val info = inspect(apk)
                when (val decision = ExtensionTrust.decision(pins, info.packageName, info.certSha256)) {
                    ExtensionTrust.Decision.FirstUse -> {
                        rows += ExtensionRow(
                            packageName = info.packageName,
                            title = info.title,
                            apkVersionName = info.apkVersionName,
                            certSha256 = info.certSha256,
                            enabled = false,
                            failure = "Not trusted on this device. Install it from this section " +
                                "to review its signing certificate.",
                        )
                    }
                    is ExtensionTrust.Decision.Mismatch -> {
                        rows += ExtensionRow(
                            packageName = info.packageName,
                            title = info.title,
                            apkVersionName = info.apkVersionName,
                            certSha256 = info.certSha256,
                            enabled = false,
                            failure = "The APK's signing certificate does not match the pin " +
                                "(${ExtensionTrust.display(decision.pinnedFingerprint)}).",
                        )
                    }
                    ExtensionTrust.Decision.Pinned -> {
                        rows += loadFromApk(apk, info, registry)
                    }
                }
            } catch (t: Throwable) {
                Log.w(TAG, "extension ${dir.name} failed to load", t)
                rows += ExtensionRow(
                    packageName = dir.name,
                    title = dir.name,
                    builtIn = false,
                    enabled = false,
                    failure = messageOf(t),
                )
            }
        }

        val sortedRows = rows.sortedWith(
            compareByDescending<ExtensionRow> { it.builtIn }
                .thenBy { it.title.lowercase() }
                .thenBy { it.packageName },
        ).map { row ->
            val id = row.extensionId
            row.copy(enabled = row.failure == null && (id == null || id !in disabled))
        }
        _state.update {
            it.copy(
                loading = false,
                rows = sortedRows,
                tabs = tabsFor(sortedRows, disabled),
            )
        }
    }

    private fun loadFromApk(
        apk: File,
        info: ArchiveInfo,
        registry: ExtensionRegistry,
    ): ExtensionRow {
        codeCacheDir.mkdirs()
        val loader = DexClassLoader(
            apk.absolutePath,
            codeCacheDir.absolutePath,
            null,
            appContext.classLoader,
        )
        val entryClass = Class.forName(EXTENSION_ENTRY_CLASS, true, loader)
        val instance = entryClass.getDeclaredConstructor().newInstance()
        if (instance !is DshExtension) {
            return ExtensionRow(
                packageName = info.packageName,
                title = info.title,
                apkVersionName = info.apkVersionName,
                certSha256 = info.certSha256,
                enabled = false,
                failure = "$EXTENSION_ENTRY_CLASS does not implement DshExtension.",
            )
        }
        val extension = instance
        return when (val result = registry.add(extension)) {
            ExtensionRegistry.AddResult.Added -> ExtensionRow(
                packageName = info.packageName,
                extensionId = extension.id,
                title = extension.title.ifBlank { info.title },
                version = extension.version,
                apkVersionName = info.apkVersionName,
                certSha256 = info.certSha256,
                builtIn = false,
                enabled = true,
                tabs = registry.tabsFor(extension.id),
            )
            is ExtensionRegistry.AddResult.Rejected -> ExtensionRow(
                packageName = info.packageName,
                extensionId = extension.id,
                title = extension.title.ifBlank { info.title },
                version = extension.version,
                apkVersionName = info.apkVersionName,
                certSha256 = info.certSha256,
                builtIn = false,
                enabled = false,
                failure = result.reason,
            )
        }
    }

    private fun register(
        extension: DshExtension,
        registry: ExtensionRegistry,
        info: ArchiveInfo?,
        failure: String?,
        builtIn: Boolean,
    ): ExtensionRow {
        if (failure != null) {
            return ExtensionRow(
                packageName = info?.packageName ?: extension.id,
                extensionId = extension.id,
                title = extension.title.ifBlank { extension.id },
                apkVersionName = info?.apkVersionName,
                certSha256 = info?.certSha256,
                builtIn = builtIn,
                enabled = false,
                failure = failure,
            )
        }
        return when (val result = registry.add(extension)) {
            ExtensionRegistry.AddResult.Added -> ExtensionRow(
                packageName = info?.packageName ?: extension.id,
                extensionId = extension.id,
                title = extension.title.ifBlank { info?.title ?: extension.id },
                version = extension.version,
                apkVersionName = info?.apkVersionName,
                certSha256 = info?.certSha256,
                builtIn = builtIn,
                enabled = true,
                tabs = registry.tabsFor(extension.id),
            )
            is ExtensionRegistry.AddResult.Rejected -> ExtensionRow(
                packageName = info?.packageName ?: extension.id,
                extensionId = extension.id,
                title = extension.title.ifBlank { extension.id },
                version = extension.version,
                apkVersionName = info?.apkVersionName,
                certSha256 = info?.certSha256,
                builtIn = builtIn,
                enabled = false,
                failure = result.reason,
            )
        }
    }

    private fun tabsFor(
        rows: List<ExtensionRow>,
        disabled: Set<String>,
    ): List<AvailableExtensionTab> =
        rows.asSequence()
            .filter { row ->
                val id = row.extensionId
                row.failure == null && id != null && id !in disabled
            }
            .flatMap { row ->
                row.tabs.asSequence().map { tab ->
                    AvailableExtensionTab(
                        extensionId = row.extensionId!!,
                        extensionTitle = row.title,
                        tab = tab,
                    )
                }
            }
            .sortedWith(compareBy({ it.tab.order }, { it.tab.id }, { it.extensionId }))
            .toList()

    // --------------------------------------------------------------- archives

    private data class ArchiveInfo(
        val packageName: String,
        val title: String,
        val apkVersionName: String?,
        val certSha256: String,
    )

    /**
     * Read an APK without installing or executing it.
     *
     * `getPackageArchiveInfo` parses the archive's manifest; it is the only way to
     * learn the package name and signing certificate before any code runs. The
     * application label is read with the archive path set as the source, which is
     * what lets [PackageManager.getApplicationLabel] resolve a non-installed APK.
     */
    @Suppress("DEPRECATION")
    private fun inspect(file: File): ArchiveInfo {
        if (!file.isFile) throw IllegalStateException("The APK file is missing.")
        if (file.length() > MAX_APK_BYTES) {
            throw IllegalStateException("The APK is larger than ${MAX_APK_BYTES / (1024 * 1024)} MB.")
        }
        val archiveFlags = if (Build.VERSION.SDK_INT >= 28) {
            PackageManager.GET_SIGNING_CERTIFICATES
        } else {
            PackageManager.GET_SIGNATURES
        }
        val info = packageManager.getPackageArchiveInfo(
            file.absolutePath,
            archiveFlags,
        ) ?: throw IllegalStateException("The selected file is not a readable APK.")

        val packageName = info.packageName
            ?: throw IllegalStateException("The APK has no package name.")
        if (!ExtensionTrust.isWellFormedPackage(packageName)) {
            throw IllegalStateException("The APK's package name is not usable here.")
        }
        val certificates = signerCertificates(info)
        if (certificates.isEmpty()) {
            throw IllegalStateException("The APK is not signed.")
        }

        val appInfo = info.applicationInfo
        appInfo?.sourceDir = file.absolutePath
        appInfo?.publicSourceDir = file.absolutePath
        val title = appInfo
            ?.let { application -> runCatching { packageManager.getApplicationLabel(application).toString() } }
            ?.getOrNull()
            ?.takeIf { it.isNotBlank() }
            ?: packageName
        val versionName = info.versionName
            ?: if (Build.VERSION.SDK_INT >= 28) info.longVersionCode.toString() else info.versionCode.toString()

        return ArchiveInfo(
            packageName = packageName,
            title = title,
            apkVersionName = versionName,
            certSha256 = ExtensionTrust.fingerprintSha256(certificates.first()),
        )
    }

    @Suppress("DEPRECATION")
    private fun signerCertificates(info: PackageInfo): List<ByteArray> {
        if (Build.VERSION.SDK_INT >= 28) {
            val signing = info.signingInfo ?: return emptyList()
            // `apkContentsSigners` is the certificate actually in this APK, not the
            // rotation history of a package that was installed through the platform.
            // This APK never was, so the pin must describe the archive in hand.
            return signing.apkContentsSigners?.map { it.toByteArray() }.orEmpty()
        }
        return info.signatures?.map { it.toByteArray() }.orEmpty()
    }

    private fun inspectInbox(file: File, trust: Map<String, String>): InboxExtension = try {
        val info = inspect(file)
        InboxExtension(
            path = file.absolutePath,
            packageName = info.packageName,
            title = info.title,
            apkVersionName = info.apkVersionName,
            certSha256 = info.certSha256,
            decision = ExtensionTrust.decision(trust, info.packageName, info.certSha256),
        )
    } catch (t: Throwable) {
        InboxExtension(path = file.absolutePath, failure = messageOf(t))
    }

    private fun stage(staged: File, fromInboxPath: String?) {
        try {
            if (staged.length() > MAX_APK_BYTES) {
                throw IllegalStateException("The APK is larger than ${MAX_APK_BYTES / (1024 * 1024)} MB.")
            }
            val info = inspect(staged)
            val config = configStore.load()
            when (val decision = ExtensionTrust.decision(config.extensionTrust, info.packageName, info.certSha256)) {
                is ExtensionTrust.Decision.Mismatch -> throw IllegalStateException(
                    "“${info.packageName}” is pinned to a different signing certificate " +
                        "(${ExtensionTrust.display(decision.pinnedFingerprint)}). This APK cannot update it.",
                )
                else -> Unit
            }
            val installed = checkedExtensionDir(info.packageName).let { File(it, "base.apk").isFile }
            _state.update {
                it.copy(
                    busy = false,
                    pending = PendingExtensionInstall(
                        sourcePath = staged.absolutePath,
                        fromInboxPath = fromInboxPath,
                        packageName = info.packageName,
                        title = info.title,
                        apkVersionName = info.apkVersionName,
                        certSha256 = info.certSha256,
                        update = installed,
                    ),
                )
            }
        } catch (t: Throwable) {
            runCatching { staged.delete() }
            fail("Could not read that APK", t)
        }
    }

    // --------------------------------------------------------------- paths

    private fun inboxDir(): File? =
        appContext.getExternalFilesDir(null)?.let { File(File(it, EXTENSIONS_DIR), INBOX_DIR) }

    private fun checkedExtensionDir(packageName: String): File {
        if (!ExtensionTrust.isWellFormedPackage(packageName)) {
            throw IllegalStateException("“$packageName” is not a valid package name.")
        }
        extensionRoot.mkdirs()
        val root = extensionRoot.canonicalFile
        val dir = File(root, packageName).canonicalFile
        if (dir.parentFile != root) {
            throw IllegalStateException("The extension path is outside private storage.")
        }
        return dir
    }

    private fun deleteTree(file: File) {
        val children = file.listFiles()
        if (children != null) {
            children.forEach { child -> deleteTree(child) }
        }
        file.delete()
    }

    private fun fail(prefix: String, t: Throwable) {
        Log.w(TAG, prefix, t)
        _state.update { it.copy(busy = false, error = "$prefix: ${messageOf(t)}") }
    }

    private fun messageOf(t: Throwable): String {
        var cause: Throwable = t
        while (cause.cause != null && cause.cause !== cause) {
            cause = cause.cause!!
        }
        return cause.message?.takeIf { it.isNotBlank() } ?: cause::class.java.simpleName
    }

    private companion object {
        const val TAG = "DshExtensions"
        const val EXTENSIONS_DIR = "extensions"
        const val STAGING_DIR = "extension-import"
        const val CODE_CACHE_DIR = "extensions"
        const val INBOX_DIR = "inbox"
        /** Defensive cap: a phone extension has no business being larger. */
        const val MAX_APK_BYTES = 64L * 1024 * 1024
    }
}

private class RuntimeExtensionScope(
    override val sessionId: StateFlow<String?>,
    override val providerId: StateFlow<String?>,
    override val localRoute: StateFlow<Boolean>,
    override val router: StateFlow<RouterSession?>,
    override val usage: StateFlow<SessionUsage?>,
    private val call: suspend (String, JSONObject) -> Any?,
) : ExtensionScope {
    override suspend fun rpc(method: String, args: JSONObject): Any? = call(method, args)
}
