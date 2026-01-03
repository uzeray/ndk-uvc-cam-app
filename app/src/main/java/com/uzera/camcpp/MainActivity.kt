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

        private const val BACK_BUF_W = 1280

        private const val BACK_BUF_H = 720

        private const val DESIRED_FPS = 60

        private const val OVERLAY_INTERVAL_MS = 1000L

        private const val P7P_SCREEN_W = 3120

        private const val P7P_SCREEN_H = 1440

        private const val FIXED_DIAMETER_PX = 800

        private const val FIXED_BACK_H = 600

        private const val FIXED_EXT_H = 400

        private const val FIXED_POS_X = 250

        private const val FIXED_POS_Y = 20


        private const val BACK_WIDTH_SCALE = 2.5f

        private const val EXT_WIDTH_SCALE = 3.1f

        init {
            System.loadLibrary("camcpp")
        }
    }

    private lateinit var backTv: TextureView
    private lateinit var extTv: TextureView
    private lateinit var debugOverlay: TextView

    private lateinit var root: FrameLayout
    private lateinit var stage: FrameLayout
    private lateinit var maskCircle: FrameLayout

    private var backSt: SurfaceTexture? = null
    private var extSt: SurfaceTexture? = null
    private var backSurface: Surface? = null
    private var extSurface: Surface? = null

    private var hasPermission = true

    private val BACK_ROT_DEG = -90f
    private val EXT_ROT_DEG = 0f

    private val backStarted = AtomicBoolean(false)
    private val extStarted = AtomicBoolean(false)
    private val backStarting = AtomicBoolean(false)
    private val extStarting = AtomicBoolean(false)

    private val camExec: ExecutorService = Executors.newSingleThreadExecutor()

    private enum class AlignY { TOP, BOTTOM, CENTER }

    private val BACK_ZOOM_MUL = 1.00f

    private val EXT_ZOOM_MUL = 1.00f

    private val uiHandler = Handler(Looper.getMainLooper())
    private val overlayRunnable = object : Runnable {
        override fun run() {
            updateOverlay()
            uiHandler.postDelayed(this, OVERLAY_INTERVAL_MS)
        }
    }

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

        supportActionBar?.hide()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        backTv = TextureView(this)
        extTv = TextureView(this)

        maskCircle = FrameLayout(this).apply {
            clipToOutline = true
            outlineProvider = object : ViewOutlineProvider() {
                override fun getOutline(view: View, outline: Outline) {
                    outline.setOval(0, 0, view.width, view.height)
                }
            }
            setBackgroundColor(0xFF000000.toInt())

            val stack = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams = FrameLayout.LayoutParams(FIXED_DIAMETER_PX, FIXED_DIAMETER_PX)

                addView(backTv, LinearLayout.LayoutParams(FIXED_DIAMETER_PX, FIXED_BACK_H))

                addView(extTv, LinearLayout.LayoutParams(FIXED_DIAMETER_PX, FIXED_EXT_H))
            }
            addView(stack)

            val seamBlender = View(context).apply {
                background = android.graphics.drawable.GradientDrawable(
                    android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM,
                    intArrayOf(
                        0x00000000,
                        0xAA000000.toInt(),
                        0x00000000
                    )
                )
            }

            val blenderHeight = 60
            val params = FrameLayout.LayoutParams(FIXED_DIAMETER_PX, blenderHeight)

            params.topMargin = FIXED_BACK_H - (blenderHeight / 2)

            addView(seamBlender, params)
        }

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

        stage = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                P7P_SCREEN_W,
                P7P_SCREEN_H
            )

            addView(maskCircle)

            addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
                layoutMaskCircle()
                applyBackTransform()
                applyExtTransform()
            }
        }

        root = FrameLayout(this).apply {
            addView(stage)

            addView(
                debugOverlay,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT
                ).apply {
                    gravity = Gravity.TOP or Gravity.END
                    rightMargin = 24
                    topMargin = 24
                }
            )
        }

        setContentView(root)

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

    private fun layoutMaskCircle() {
        val w = FIXED_DIAMETER_PX
        val h = FIXED_DIAMETER_PX

        val lp = maskCircle.layoutParams as? FrameLayout.LayoutParams
            ?: FrameLayout.LayoutParams(w, h)

        if (lp.width != w || lp.height != h) {
            lp.width = w
            lp.height = h
            maskCircle.layoutParams = lp
        }

        maskCircle.x = FIXED_POS_X.toFloat()
        maskCircle.y = FIXED_POS_Y.toFloat()
    }

    private fun applyTransform(
        tv: TextureView,
        bufW: Int,
        bufH: Int,
        rotationDegrees: Float,
        alignY: AlignY,
        zoomMul: Float,
        mirrorX: Boolean,
        targetW: Int,
        targetH: Int,
        extraScaleX: Float
    ) {
        val vw = targetW.toFloat()
        val vh = targetH.toFloat()

        if (bufW <= 0 || bufH <= 0) return

        val rot = ((rotationDegrees % 360f) + 360f) % 360f
        val rot90 = (rot == 90f || rot == 270f)

        val srcW = bufW.toFloat()
        val srcH = bufH.toFloat()

        val effW = if (rot90) srcH else srcW
        val effH = if (rot90) srcW else srcH

        var scale = vw / effW

        scale *= zoomMul

        val m = Matrix()

        m.postTranslate(-srcW / 2f, -srcH / 2f)

        if (rot != 0f) m.postRotate(rot)

        val finalSx = (if (mirrorX) -scale else scale) * extraScaleX
        val finalSy = scale
        m.postScale(finalSx, finalSy)

        m.postTranslate(vw / 2f, vh / 2f)

        val renderedHeight = effH * scale

        val dy = when (alignY) {
            AlignY.TOP -> {
                (renderedHeight - vh) / 2f
            }

            AlignY.BOTTOM -> {
                -(renderedHeight - vh) / 2f
            }

            AlignY.CENTER -> {
                0f
            }
        }

        m.postTranslate(0f, dy)

        tv.setTransform(m)
        tv.invalidate()
    }

    private fun applyBackTransform() {
        applyTransform(
            tv = backTv,
            bufW = BACK_BUF_W,
            bufH = BACK_BUF_H,
            rotationDegrees = BACK_ROT_DEG,
            alignY = AlignY.BOTTOM,
            zoomMul = BACK_ZOOM_MUL,
            mirrorX = false,
            targetW = FIXED_DIAMETER_PX,
            targetH = FIXED_BACK_H,
            extraScaleX = BACK_WIDTH_SCALE
        )
    }

    private fun applyExtTransform() {
        applyTransform(
            tv = extTv,
            bufW = extBufW,
            bufH = extBufH,
            rotationDegrees = EXT_ROT_DEG,
            alignY = AlignY.TOP,
            zoomMul = EXT_ZOOM_MUL,
            mirrorX = false,
            targetW = FIXED_DIAMETER_PX,
            targetH = FIXED_EXT_H,
            extraScaleX = EXT_WIDTH_SCALE
        )
    }

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
                    extSt?.setDefaultBufferSize(extBufW, extBufH)
                    applyExtTransform()
                } else {
                    extFmtCache = ""
                }
                updateOverlay()
            }
        }
    }

    private fun stopBack() {
        if (!backStarted.get() && !backStarting.get()) return
        backStarted.set(false)
        backStarting.set(false)
        camExec.execute { nativeStopBackPreview() }
    }

    private fun stopExt() {
        if (!extStarted.get() && !extStarting.get()) return
        extStarted.set(false)
        extStarting.set(false)
        extModeCache = ""
        extFmtCache = ""
        camExec.execute { nativeStopExternalPreview() }
    }

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