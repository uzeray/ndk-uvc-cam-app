package com.uzera.camcpp

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Matrix
import android.graphics.Outline
import android.graphics.RectF
import android.graphics.SurfaceTexture
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.text.TextUtils
import android.view.Gravity
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.view.ViewGroup
import android.view.ViewOutlineProvider
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.max

class MainActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "CamcppNDK"

        // for back camera buffer width
        private const val BACK_BUF_W = 1280

        // for back camera buffer height
        private const val BACK_BUF_H = 720

        // for target fps
        private const val DESIRED_FPS = 60

        // for overlay refresh interval
        private const val OVERLAY_INTERVAL_MS = 1000L

        // for Pixel 7 Pro landscape screen width (3120px)
        private const val P7P_SCREEN_W = 3120

        // for Pixel 7 Pro landscape screen height (1440px)
        private const val P7P_SCREEN_H = 1440

        // for circle mask diameter (50mm lens sim on Pixel 7 Pro)
        private const val FIXED_DIAMETER_PX = 800

        // for back cam view height (top 70% of diameter)
        private const val FIXED_BACK_H = 600

        // for external camera view height (bottom 30% of diameter)
        private const val FIXED_EXT_H = 400

        // for circle X position (Left Eye Center @ 25% of 3120 = 780px. 780 - 500 radius = 280)
        private const val FIXED_POS_X = 250

        // for circle Y position (Vertically centered: 1440/2 = 720. 720 - 500 radius = 220)
        private const val FIXED_POS_Y = 80

        // ==========================================
        // YENİ EKLENEN X EKSENİ GENİŞLETME AYARLARI
        // ==========================================

        // for modifying ONLY width (X-axis) of back camera (1.0 = normal, 1.2 = wider)
        private const val BACK_WIDTH_SCALE = 2.5f

        // for modifying ONLY width (X-axis) of ext camera (1.0 = normal, 1.2 = wider)
        private const val EXT_WIDTH_SCALE = 3.1f

        init {
            System.loadLibrary("camcpp")
        }
    }

    // ====== Views ======
    private lateinit var backTv: TextureView
    private lateinit var extTv: TextureView
    private lateinit var debugOverlay: TextView

    private lateinit var root: FrameLayout
    private lateinit var stage: FrameLayout
    private lateinit var maskCircle: FrameLayout

    // ====== Surfaces / SurfaceTextures ======
    private var backSt: SurfaceTexture? = null
    private var extSt: SurfaceTexture? = null
    private var backSurface: Surface? = null
    private var extSurface: Surface? = null

    private var hasPermission = true

    // Rotations
    private val BACK_ROT_DEG = -90f
    private val EXT_ROT_DEG = 0f

    private val backStarted = AtomicBoolean(false)
    private val extStarted = AtomicBoolean(false)
    private val backStarting = AtomicBoolean(false)
    private val extStarting = AtomicBoolean(false)

    private val camExec: ExecutorService = Executors.newSingleThreadExecutor()

    // for alignment enum (CENTER EKLENDİ)
    private enum class AlignY { TOP, BOTTOM, CENTER }

    // for zoom multiplier back (1.0 = no zoom)
    private val BACK_ZOOM_MUL = 1.00f

    // for zoom multiplier ext (1.0 = no zoom)
    private val EXT_ZOOM_MUL = 1.00f

    private val uiHandler = Handler(Looper.getMainLooper())
    private val overlayRunnable = object : Runnable {
        override fun run() {
            updateOverlay()
            uiHandler.postDelayed(this, OVERLAY_INTERVAL_MS)
        }
    }

    // EXT native-chosen mode cache
    private var extModeCache: String = ""
    private var extFmtCache: String = ""
    private var extBufW: Int = 1280
    private var extBufH: Int = 720

    private var lastSuInfo: String = ""
    private var lastSuPrepErr: String = ""

    private val reqCamPerm = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        hasPermission = granted
        maybeStartBack()
        maybeStartExt()
    }

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                UsbManager.ACTION_USB_DEVICE_DETACHED -> runOnUiThread { stopExt() }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    camExec.execute {
                        prepareUvcAccess()
                        runOnUiThread {
                            stopExt()
                            extTv.postDelayed({ maybeStartExt() }, 200)
                        }
                    }
                }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // for hiding status bar
        supportActionBar?.hide()
        // for keeping screen on
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        // for creating texture views
        backTv = TextureView(this)
        extTv = TextureView(this)

        // for creating circle mask container
        maskCircle = FrameLayout(this).apply {
            clipToOutline = true
            outlineProvider = object : ViewOutlineProvider() {
                override fun getOutline(view: View, outline: Outline) {
                    outline.setOval(0, 0, view.width, view.height)
                }
            }
            setBackgroundColor(0xFF000000.toInt())

            // Stack Container
            val stack = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams = FrameLayout.LayoutParams(FIXED_DIAMETER_PX, FIXED_DIAMETER_PX)

                // 1. BACK CAMERA (Üst Kısım)
                addView(backTv, LinearLayout.LayoutParams(FIXED_DIAMETER_PX, FIXED_BACK_H))

                // 2. EXT CAMERA (Alt Kısım)
                addView(extTv, LinearLayout.LayoutParams(FIXED_DIAMETER_PX, FIXED_EXT_H))
            }
            addView(stack)

            // [YENİ]: DİKİŞ İZİ GİZLEYİCİ (SEAM BLENDER)
            // Bu View, tam iki kameranın birleştiği noktaya oturur.
            val seamBlender = View(context).apply {
                // Arka planı yukarıdan aşağıya (Şeffaf -> Siyah -> Şeffaf) yapar.
                // Bu, keskin çizgiyi yumuşatır.
                background = android.graphics.drawable.GradientDrawable(
                    android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM,
                    intArrayOf(
                        0x00000000, // Üst: Şeffaf
                        0xAA000000.toInt(), // Orta: Yarı Saydam Siyah (Çizgiyi gizler)
                        0x00000000  // Alt: Şeffaf
                    )
                )
            }

            // Blender'ı tam arayüze yerleştiriyoruz.
            // Yüksekliği 60px olsun (30px yukarı, 30px aşağı taşar).
            val blenderHeight = 60
            val params = FrameLayout.LayoutParams(FIXED_DIAMETER_PX, blenderHeight)

            // Konumu: Back kameranın bittiği yerin biraz yukarısından başlasın
            params.topMargin = FIXED_BACK_H - (blenderHeight / 2)

            addView(seamBlender, params)
        }

        // for debug overlay text
        debugOverlay = TextView(this).apply {
            textSize = 12f
            setPadding(12, 8, 12, 8)
            setBackgroundColor(0x66000000)
            setTextColor(0xFFFFFFFF.toInt())
            setSingleLine(false)
            maxLines = 2
            ellipsize = TextUtils.TruncateAt.END
            text = "Loading..."
        }

        // for stage container using Pixel 7 Pro resolution
        stage = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                P7P_SCREEN_W,
                P7P_SCREEN_H
            )

            addView(maskCircle)

            // for positioning mask when layout happens
            addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
                layoutMaskCircle()
                // Transforms are called here too to ensure matrix is set
                applyBackTransform()
                applyExtTransform()
            }
        }

        // for root frame
        root = FrameLayout(this).apply {
            addView(stage)

            addView(
                debugOverlay,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT
                ).apply {
                    // for placing debug text on Top-Right
                    gravity = Gravity.TOP or Gravity.END
                    rightMargin = 24
                    topMargin = 24
                }
            )
        }

        setContentView(root)

        // for view layout changes triggers
        backTv.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> applyBackTransform() }
        extTv.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> applyExtTransform() }

        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(usbReceiver, filter)
        }

        // for back camera surface listener
        backTv.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                backSt = st
                st.setDefaultBufferSize(BACK_BUF_W, BACK_BUF_H)
                backSurface = Surface(st)

                layoutMaskCircle()
                applyBackTransform()
                maybeStartBack()
            }

            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {
                layoutMaskCircle()
                applyBackTransform()
            }

            override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
                stopBack()
                backSurface?.release()
                backSurface = null
                backSt = null
                return true
            }

            override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
        }

        // for ext camera surface listener
        extTv.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                extSt = st
                st.setDefaultBufferSize(extBufW, extBufH)
                extSurface = Surface(st)

                layoutMaskCircle()
                applyExtTransform()
                maybeStartExt()
            }

            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {
                layoutMaskCircle()
                applyExtTransform()
            }

            override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
                stopExt()
                extSurface?.release()
                extSurface = null
                extSt = null
                return true
            }

            override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
        }

        hasPermission = ContextCompat.checkSelfPermission(
            this, Manifest.permission.CAMERA
        ) == PackageManager.PERMISSION_GRANTED

        if (!hasPermission) reqCamPerm.launch(Manifest.permission.CAMERA)
    }

    override fun onResume() {
        super.onResume()

        layoutMaskCircle()
        applyBackTransform()
        applyExtTransform()

        maybeStartBack()
        maybeStartExt()

        uiHandler.removeCallbacks(overlayRunnable)
        uiHandler.post(overlayRunnable)
    }

    override fun onPause() {
        uiHandler.removeCallbacks(overlayRunnable)

        stopBack()
        stopExt()
        super.onPause()
    }

    override fun onDestroy() {
        try {
            unregisterReceiver(usbReceiver)
        } catch (_: Throwable) {
        }
        camExec.shutdownNow()
        super.onDestroy()
    }

    // for setting exact pixel position and size for the circle
    private fun layoutMaskCircle() {
        // for hardcoded circle width
        val w = FIXED_DIAMETER_PX
        // for hardcoded circle height
        val h = FIXED_DIAMETER_PX

        val lp = maskCircle.layoutParams as? FrameLayout.LayoutParams
            ?: FrameLayout.LayoutParams(w, h)

        if (lp.width != w || lp.height != h) {
            lp.width = w
            lp.height = h
            maskCircle.layoutParams = lp
        }

        // for hardcoded X position (Left Eye 25%)
        maskCircle.x = FIXED_POS_X.toFloat()
        // for hardcoded Y position
        maskCircle.y = FIXED_POS_Y.toFloat()
    }

    // for calculating transformation matrix using FIXED values
    private fun applyTransform(
        tv: TextureView,
        bufW: Int,
        bufH: Int,
        rotationDegrees: Float,
        alignY: AlignY,
        zoomMul: Float,
        mirrorX: Boolean,
        targetW: Int, // for passing hardcoded view width
        targetH: Int,  // for passing hardcoded view height
        extraScaleX: Float // NEW: for stretching/squashing width ONLY
    ) {
        // for hardcoded target width (cast to float)
        val vw = targetW.toFloat()
        // for hardcoded target height (cast to float)
        val vh = targetH.toFloat()

        if (bufW <= 0 || bufH <= 0) return

        val rot = ((rotationDegrees % 360f) + 360f) % 360f
        val rot90 = (rot == 90f || rot == 270f)

        val srcW = bufW.toFloat()
        val srcH = bufH.toFloat()

        // for effective width after rotation
        val effW = if (rot90) srcH else srcW
        // for effective height after rotation
        val effH = if (rot90) srcW else srcH

        // for scale calculation: match WIDTH of the circle
        var scale = vw / effW

        // for optional zoom
        scale *= zoomMul

        val m = Matrix()

        // for centering source image
        m.postTranslate(-srcW / 2f, -srcH / 2f)

        // for rotating image
        if (rot != 0f) m.postRotate(rot)

        // for mirroring X axis if needed AND applying extra width scale
        // NOTE: X scale is multiplied by extraScaleX to stretch/squash horizontally
        val finalSx = (if (mirrorX) -scale else scale) * extraScaleX
        val finalSy = scale
        m.postScale(finalSx, finalSy)

        // for moving to center of the view (X is centered by default)
        m.postTranslate(vw / 2f, vh / 2f)

        // for calculating actual rendered height
        val renderedHeight = effH * scale

        // for calculating alignment offset
        val dy = when (alignY) {
            AlignY.TOP -> {
                // for aligning image top to view top
                (renderedHeight - vh) / 2f
            }

            AlignY.BOTTOM -> {
                // for aligning image bottom to view bottom
                -(renderedHeight - vh) / 2f
            }

            AlignY.CENTER -> {
                // for centering vertically (no shift needed as we are already centered)
                0f
            }
        }

        // for applying alignment shift
        m.postTranslate(0f, dy)

        tv.setTransform(m)
        // for invalidating view to redraw
        tv.invalidate()
    }

    // for applying back camera transform using fixed constants
    private fun applyBackTransform() {
        applyTransform(
            tv = backTv,
            bufW = BACK_BUF_W,
            bufH = BACK_BUF_H,
            rotationDegrees = BACK_ROT_DEG,
            alignY = AlignY.BOTTOM, // for aligning to bottom
            zoomMul = BACK_ZOOM_MUL,
            mirrorX = false, // for mirroring back camera
            targetW = FIXED_DIAMETER_PX, // for passing fixed width
            targetH = FIXED_BACK_H,       // for passing fixed height
            extraScaleX = BACK_WIDTH_SCALE // NEW: pass width scale
        )
    }

    // for applying ext camera transform using fixed constants
    private fun applyExtTransform() {
        applyTransform(
            tv = extTv,
            bufW = extBufW,
            bufH = extBufH,
            rotationDegrees = EXT_ROT_DEG,
            alignY = AlignY.TOP, // for aligning to top
            zoomMul = EXT_ZOOM_MUL,
            mirrorX = false, // for no mirror on ext
            targetW = FIXED_DIAMETER_PX, // for passing fixed width
            targetH = FIXED_EXT_H,        // for passing fixed height
            extraScaleX = EXT_WIDTH_SCALE // NEW: pass width scale
        )
    }

    // for parsing mode string
    private fun updateExtBufFromModeString(mode: String) {
        val m = Regex("""(\d+)\s*x\s*(\d+)""").find(mode)
            ?: Regex("""(\d+)x(\d+)""").find(mode)
        if (m != null) {
            val w = m.groupValues[1].toIntOrNull()
            val h = m.groupValues[2].toIntOrNull()
            if (w != null && h != null && w > 0 && h > 0) {
                extBufW = w
                extBufH = h
            }
        }
        extFmtCache = mode.trim().split(Regex("\\s+")).firstOrNull().orEmpty()
    }

    // for running shell command
    private fun runSu(cmd: String): Pair<Int, String> {
        return try {
            val p = ProcessBuilder("su", "-c", cmd)
                .redirectErrorStream(true)
                .start()

            val out = StringBuilder()
            BufferedReader(InputStreamReader(p.inputStream)).use { br ->
                var line: String?
                while (br.readLine().also { line = it } != null) {
                    out.append(line).append('\n')
                }
            }

            val code = p.waitFor()
            code to out.toString().trim()
        } catch (t: Throwable) {
            -1 to (t.message ?: "su failed")
        }
    }

    // for preparing permissions
    private fun prepareUvcAccess(): Boolean {
        lastSuPrepErr = ""

        val (idCode, idOut) = runSu("id")
        lastSuInfo = "su=$idCode ${idOut.take(60)}"
        if (idCode != 0) {
            lastSuPrepErr = "SU permission missing\n$idOut"
            return false
        }

        runSu("setenforce 0")
        val (chCode, _) = runSu(
            "chmod 666 /dev/video* /dev/v4l-subdev* 2>/dev/null; ls -l /dev/video0 2>/dev/null"
        )
        if (chCode != 0) {
            lastSuPrepErr = "chmod failed: $chCode"
            return false
        }
        return true
    }

    // for starting back camera
    private fun maybeStartBack() {
        val s = backSurface ?: return
        if (!hasPermission) return
        if (backStarted.get() || backStarting.get()) return

        backStarting.set(true)
        camExec.execute {
            val ok = nativeStartBackPreview(s, DESIRED_FPS)
            runOnUiThread {
                backStarting.set(false)
                backStarted.set(ok)
                updateOverlay()
            }
        }
    }

    // for starting ext camera
    private fun maybeStartExt() {
        val s = extSurface ?: return
        if (!hasPermission) return
        if (extStarted.get() || extStarting.get()) return

        extStarting.set(true)
        camExec.execute {
            val prepOk = prepareUvcAccess()
            val ok = if (prepOk) nativeStartExternalPreview(s, DESIRED_FPS) else false
            val mode = if (ok) nativeGetExtChosenMode() else ""

            runOnUiThread {
                extStarting.set(false)
                extStarted.set(ok)

                extModeCache = mode
                if (mode.isNotBlank()) {
                    updateExtBufFromModeString(mode)
                    // for buffer size update
                    extSt?.setDefaultBufferSize(extBufW, extBufH)
                    applyExtTransform()
                } else {
                    extFmtCache = ""
                }
                updateOverlay()
            }
        }
    }

    // for stopping back camera
    private fun stopBack() {
        if (!backStarted.get() && !backStarting.get()) return
        backStarted.set(false)
        backStarting.set(false)
        camExec.execute { nativeStopBackPreview() }
    }

    // for stopping ext camera
    private fun stopExt() {
        if (!extStarted.get() && !extStarting.get()) return
        extStarted.set(false)
        extStarting.set(false)
        extModeCache = ""
        extFmtCache = ""
        camExec.execute { nativeStopExternalPreview() }
    }

    // for updating debug overlay
    private fun updateOverlay() {
        val now = SystemClock.elapsedRealtimeNanos()

        val bTs = nativeGetBackLastSensorTimestampNs()
        val bAgeMs = if (bTs > 0) (now - bTs) / 1_000_000.0 else -1.0
        val bFps = nativeGetBackEstimatedFpsX100() / 100.0
        val bErr = nativeGetBackLastError()

        val eTs = nativeGetExtLastSensorTimestampNs()
        val eAgeMs = if (eTs > 0) (now - eTs) / 1_000_000.0 else -1.0
        val eFps = nativeGetExtEstimatedFpsX100() / 100.0
        val eErr = nativeGetExtLastError()

        fun ms(v: Double) = if (v >= 0) String.format("%.1f", v) else "n/a"
        fun fps(v: Double) = if (v >= 0) String.format("%.2f", v) else "n/a"

        val backLine = buildString {
            append("BACK  RES ${BACK_BUF_W}x${BACK_BUF_H}  FPS ${fps(bFps)}  LAT ${ms(bAgeMs)} ms")
            if (bErr.isNotBlank()) append("  ERR")
        }

        val extRes = "${extBufW}x${extBufH}"
        val extFmt = if (extFmtCache.isNotBlank()) extFmtCache else "EXT"
        val extLine = buildString {
            append("EXT   RES $extRes  FPS ${fps(eFps)}  LAT ${ms(eAgeMs)} ms  $extFmt")
            if (eErr.isNotBlank() || lastSuPrepErr.isNotBlank()) append("  ERR")
        }

        debugOverlay.text = "$backLine\n$extLine"
    }

    // ---- JNI ----
    private external fun nativeStartBackPreview(surface: Surface, desiredFps: Int): Boolean
    private external fun nativeStopBackPreview()

    external fun nativeGetOpenCvVersion(): String
    external fun nativeOpenCvSmokeTest(): String

    private external fun nativeStartExternalPreview(surface: Surface, desiredFps: Int): Boolean
    private external fun nativeStopExternalPreview()

    private external fun nativeGetBackLastSensorTimestampNs(): Long
    private external fun nativeGetBackEstimatedFpsX100(): Int
    private external fun nativeGetBackLastError(): String

    private external fun nativeGetExtLastSensorTimestampNs(): Long
    private external fun nativeGetExtEstimatedFpsX100(): Int
    private external fun nativeGetExtLastError(): String
    private external fun nativeGetExtChosenMode(): String
}