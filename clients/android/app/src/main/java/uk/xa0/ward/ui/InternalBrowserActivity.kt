package uk.xa0.ward.ui

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.net.Uri
import android.os.Bundle
import android.os.Message
import android.view.ViewGroup
import android.webkit.CookieManager
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.ArrowBack
import androidx.compose.material.icons.rounded.ArrowForward
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material.icons.rounded.OpenInNew
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import uk.xa0.ward.ui.theme.DshSpacing
import uk.xa0.ward.ui.theme.DshTheme
import uk.xa0.ward.ui.theme.DshType

class InternalBrowserActivity : ComponentActivity() {

    private var webView: WebView? = null
    private var canGoBack by mutableStateOf(false)
    private var canGoForward by mutableStateOf(false)
    private var pageTitle by mutableStateOf<String?>(null)
    private var currentUrl by mutableStateOf<String?>(null)
    private var loadProgress by mutableIntStateOf(0)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        val url = intent?.getStringExtra(EXTRA_URL)
        if (url == null || !isWebLink(url)) {
            finish()
            return
        }
        currentUrl = url

        onBackPressedDispatcher.addCallback(
            this,
            object : OnBackPressedCallback(true) {
                override fun handleOnBackPressed() {
                    val view = webView
                    if (view != null && view.canGoBack()) view.goBack() else finish()
                }
            },
        )

        setContent {
            DshTheme {
                val colors = DshTheme.colors
                Column(
                    Modifier
                        .fillMaxSize()
                        .background(colors.bgBase)
                        .statusBarsPadding()
                        .navigationBarsPadding()
                        .imePadding(),
                ) {
                    AndroidView(
                        factory = { createWebView(url) },
                        modifier = Modifier
                            .fillMaxWidth()
                            .weight(1f),
                    )
                    BrowserProgress(loadProgress)
                    BrowserToolbar(
                        location = pageTitle?.takeIf { it.isNotBlank() } ?: currentUrl ?: url,
                        canGoBack = canGoBack,
                        canGoForward = canGoForward,
                        onBack = { webView?.goBack() },
                        onForward = { webView?.goForward() },
                        onReload = { webView?.reload() },
                        canOpenExternal = currentUrl != null,
                        onOpenExternal = {
                            currentUrl?.let { openExternally(this@InternalBrowserActivity, it) }
                        },
                        onClose = { finish() },
                    )
                }
            }
        }
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun createWebView(url: String): WebView {
        val view = WebView(this)
        val settings = view.settings
        settings.javaScriptEnabled = true
        settings.domStorageEnabled = true
        settings.setSupportZoom(true)
        settings.builtInZoomControls = true
        settings.displayZoomControls = false
        settings.useWideViewPort = true
        settings.loadWithOverviewMode = true
        settings.javaScriptCanOpenWindowsAutomatically = true
        settings.setSupportMultipleWindows(true)

        val cookies = CookieManager.getInstance()
        cookies.setAcceptCookie(true)
        cookies.setAcceptThirdPartyCookies(view, true)

        view.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(
                view: WebView?,
                request: WebResourceRequest?,
            ): Boolean {
                if (view == null || request == null) return false
                if (request.url.isHttpScheme() || !request.isForMainFrame) return false
                openExternally(view.context, request.url.toString())
                return true
            }

            override fun onPageStarted(view: WebView?, url: String?, favicon: Bitmap?) {
                currentUrl = url ?: currentUrl
                pageTitle = null
                loadProgress = 0
                view?.let(::syncHistory)
            }

            override fun onPageFinished(view: WebView?, url: String?) {
                currentUrl = url ?: currentUrl
                loadProgress = 100
                view?.let(::syncHistory)
            }

            override fun doUpdateVisitedHistory(view: WebView?, url: String?, isReload: Boolean) {
                currentUrl = url ?: currentUrl
                view?.let(::syncHistory)
            }
        }

        view.webChromeClient = object : WebChromeClient() {
            override fun onProgressChanged(view: WebView?, newProgress: Int) {
                loadProgress = newProgress
            }

            override fun onReceivedTitle(view: WebView?, title: String?) {
                pageTitle = title
            }

            override fun onCreateWindow(
                view: WebView?,
                isDialog: Boolean,
                isUserGesture: Boolean,
                resultMsg: Message,
            ): Boolean {
                if (view == null) return false
                val transport = resultMsg.obj as? WebView.WebViewTransport ?: return false
                val popup = WebView(view.context)
                popup.webViewClient = object : WebViewClient() {
                    override fun shouldOverrideUrlLoading(
                        popupView: WebView?,
                        request: WebResourceRequest?,
                    ): Boolean {
                        if (popupView == null || request == null) return true
                        if (request.url.isHttpScheme()) {
                            view.loadUrl(request.url.toString())
                        } else {
                            openExternally(view.context, request.url.toString())
                        }
                        popupView.destroy()
                        return true
                    }
                }
                transport.webView = popup
                resultMsg.sendToTarget()
                return true
            }
        }

        view.loadUrl(url)
        webView = view
        return view
    }

    private fun syncHistory(view: WebView) {
        canGoBack = view.canGoBack()
        canGoForward = view.canGoForward()
    }

    override fun onDestroy() {
        webView?.let { view ->
            (view.parent as? ViewGroup)?.removeView(view)
            view.stopLoading()
            view.clearHistory()
            view.destroy()
        }
        webView = null
        super.onDestroy()
    }

    companion object {
        const val EXTRA_URL: String = "uk.xa0.ward.ui.InternalBrowserActivity.url"

        fun intent(context: Context, url: String): Intent =
            Intent(context, InternalBrowserActivity::class.java).putExtra(EXTRA_URL, url)
    }
}

private fun Uri?.isHttpScheme(): Boolean {
    val scheme = this?.scheme ?: return false
    return scheme.equals("http", ignoreCase = true) || scheme.equals("https", ignoreCase = true)
}

@Composable
private fun BrowserToolbar(
    location: String,
    canGoBack: Boolean,
    canGoForward: Boolean,
    canOpenExternal: Boolean,
    onBack: () -> Unit,
    onForward: () -> Unit,
    onReload: () -> Unit,
    onOpenExternal: () -> Unit,
    onClose: () -> Unit,
) {
    val colors = DshTheme.colors
    Box(
        Modifier
            .fillMaxWidth()
            .height(0.5.dp)
            .background(colors.borderL1),
    )
    Row(
        Modifier
            .fillMaxWidth()
            .height(48.dp)
            .background(colors.bgLayer1)
            .padding(horizontal = DshSpacing.xs),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        BrowserAction(Icons.Rounded.ArrowBack, "Back", canGoBack, onBack)
        BrowserAction(Icons.Rounded.ArrowForward, "Forward", canGoForward, onForward)
        BrowserAction(Icons.Rounded.Refresh, "Reload", true, onReload)
        BrowserAction(Icons.Rounded.OpenInNew, "Open in browser", canOpenExternal, onOpenExternal)
        Text(
            text = location,
            modifier = Modifier
                .weight(1f)
                .padding(horizontal = DshSpacing.sm),
            style = DshType.bodySmall,
            color = colors.labelSecondary,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        BrowserAction(Icons.Rounded.Close, "Close", true, onClose)
    }
}

@Composable
private fun BrowserAction(
    icon: ImageVector,
    description: String,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    val colors = DshTheme.colors
    IconButton(
        onClick = onClick,
        enabled = enabled,
        modifier = Modifier.size(40.dp),
    ) {
        Icon(
            imageVector = icon,
            contentDescription = description,
            tint = if (enabled) colors.labelSecondary else colors.labelDimmed,
            modifier = Modifier.size(20.dp),
        )
    }
}

@Composable
private fun BrowserProgress(progress: Int) {
    val colors = DshTheme.colors
    Box(
        Modifier
            .fillMaxWidth()
            .height(2.dp),
    ) {
        if (progress in 1..99) {
            Box(
                Modifier
                    .fillMaxWidth(progress / 100f)
                    .fillMaxHeight()
                    .background(colors.accent),
            )
        }
    }
}
