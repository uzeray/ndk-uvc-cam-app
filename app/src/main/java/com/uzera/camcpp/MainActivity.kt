package com.uzera.camcpp

import android.Manifest
import android.content.*
import android.content.pm.PackageManager
import android.graphics.Matrix
import android.graphics.Outline
import android.graphics.SurfaceTexture
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.*
import android.widget.FrameLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.roundToInt
import android.graphics.RectF

class MainActivity : AppCompatActivity() {

    companion object {
        init {
            System.loadLibrary("camcpp")
        }
    }

    private lateinit var leftTv: TextureView
    private lateinit var rightTv: TextureView
    private lateinit var debugOverlay: TextView

    private lateinit var root: FrameLayout
    private lateinit var stage: FrameLayout
    private lateinit var leftOval: FrameLayout
    private lateinit var rightOval: FrameLayout

    private var leftSurface: Surface? = null
    private var rightSurface: Surface? = null

    private var hasPermission = false

    private val BACK_ROT_DEG = -90f
    private val EXT_ROT_DEG = 0f

    private val backStarted = AtomicBoolean(false)
    private val extStarted = AtomicBoolean(false)
    private val backStarting = AtomicBoolean(false)
    private val extStarting = AtomicBoolean(false)

    private val camExec = Executors.newSingleThreadExecutor()

    private var OVAL_DIAMETER_DP = 200f   // burayı büyüt/küçült
    private var OVAL_GAP_DP = 20f         // iki göz arası boşluk (istersen)
    private fun dp(v: Float) = (v * resources.displayMetrics.density).roundToInt()

    private val choreographer by lazy { Choreographer.getInstance() }
    private val frameCb = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            updateOverlay()
            choreographer.postFrameCallback(this)
        }
    }

    private var lastSuInfo: String = ""
    private var lastSuPrepErr: String = ""

    private val BACK_BUF_W = 1280
    private val BACK_BUF_H = 720

    private var extBufW: Int = 1280
    private var extBufH: Int = 720

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
                            rightTv.postDelayed({ maybeStartExt() }, 200)
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

        leftTv = TextureView(this)
        rightTv = TextureView(this)

        fun makeOvalContainer(tv: TextureView): FrameLayout {
            return FrameLayout(this).apply {
                clipToOutline = true
                outlineProvider = object : ViewOutlineProvider() {
                    override fun getOutline(view: View, outline: Outline) {
                        outline.setOval(0, 0, view.width, view.height)
                    }
                }
                setBackgroundColor(0xFF000000.toInt())

                addView(
                    tv,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT
                    )
                )
            }
        }

        leftOval = makeOvalContainer(leftTv)
        rightOval = makeOvalContainer(rightTv)

        debugOverlay = TextView(this).apply {
            textSize = 14f
            setPadding(16, 12, 16, 12)
            setBackgroundColor(0x66000000)
            setTextColor(0xFFFFFFFF.toInt())
            text = "init..."
        }

        stage = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
            setPadding(24, 24, 24, 24)

            addView(leftOval)
            addView(rightOval)

            addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
                layoutOvals()
                applyBackTransform()
                applyExtTransform()
            }
        }

        root = FrameLayout(this).apply {
            addView(stage)

            addView(debugOverlay)
            debugOverlay.layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT
            ).apply { gravity = Gravity.TOP or Gravity.START }
        }

        setContentView(root)

        leftTv.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            applyBackTransform()
        }
        rightTv.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            applyExtTransform()
        }

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

        leftTv.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                st.setDefaultBufferSize(BACK_BUF_W, BACK_BUF_H)
                leftSurface = Surface(st)
                layoutOvals()
                applyBackTransform()
                maybeStartBack()
            }

            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {
                layoutOvals()
                applyBackTransform()
            }

            override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
                stopBack()
                leftSurface?.release()
                leftSurface = null
                return true
            }

            override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
        }

        rightTv.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                // Burada sabit 1280x720 kalsın; gerçek mode'u native seçince extBufW/extBufH güncellenecek
                st.setDefaultBufferSize(1280, 720)
                rightSurface = Surface(st)
                layoutOvals()
                applyExtTransform()
                maybeStartExt()
            }

            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {
                layoutOvals()
                applyExtTransform()
            }

            override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
                stopExt()
                rightSurface?.release()
                rightSurface = null
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
        choreographer.postFrameCallback(frameCb)
        layoutOvals()
        applyBackTransform()
        applyExtTransform()
        maybeStartBack()
        maybeStartExt()
    }

    override fun onPause() {
        choreographer.removeFrameCallback(frameCb)
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

    private fun layoutOvals() {
        val w = stage.width
        val h = stage.height
        if (w == 0 || h == 0) return

        val padL = stage.paddingLeft
        val padR = stage.paddingRight
        val padT = stage.paddingTop
        val padB = stage.paddingBottom

        val innerW = (w - padL - padR).toFloat()
        val innerH = (h - padT - padB).toFloat()
        if (innerW <= 0f || innerH <= 0f) return

        val desired = dp(OVAL_DIAMETER_DP)
        val gapPx = dp(OVAL_GAP_DP)

        val maxByH = innerH.toInt()
        val maxByW = ((innerW - gapPx) / 2f).toInt()

        val size = minOf(desired, maxByH, maxByW).coerceAtLeast(1)

        fun applySize(v: View) {
            val lp = v.layoutParams as? FrameLayout.LayoutParams
                ?: FrameLayout.LayoutParams(size, size)
            if (lp.width != size || lp.height != size) {
                lp.width = size
                lp.height = size
                v.layoutParams = lp
            }
        }

        applySize(leftOval)
        applySize(rightOval)

        // Merkezleme noktalarını kontrol et (Göz aralığına göre %25 ve %75 idealdir)
        val cxLeft = padL + innerW * 0.25f
        val cxRight = padL + innerW * 0.75f
        val cy = padT + innerH * 0.5f

        leftOval.x = cxLeft - size / 2f
        leftOval.y = cy - size / 2f

        rightOval.x = cxRight - size / 2f
        rightOval.y = cy - size / 2f
    }

    private fun applyAspectTransform(
        tv: TextureView,
        bufW: Int,
        bufH: Int,
        rotationDegrees: Float,
        centerCrop: Boolean
    ) {
        val vw = tv.width.toFloat()
        val vh = tv.height.toFloat()
        if (vw <= 0f || vh <= 0f) return
        if (bufW <= 0 || bufH <= 0) return

        val viewRect = RectF(0f, 0f, vw, vh)
        val centerX = viewRect.centerX()
        val centerY = viewRect.centerY()

        // normalize rotation to [0,360)
        val rot = ((rotationDegrees % 360f) + 360f) % 360f
        val rot90 = (rot == 90f || rot == 270f)

        // After 90/270 rotation, buffer width/height swap for aspect math
        val contentW = if (rot90) bufH.toFloat() else bufW.toFloat()
        val contentH = if (rot90) bufW.toFloat() else bufH.toFloat()

        val bufferRect = RectF(0f, 0f, contentW, contentH).apply {
            offset(centerX - centerX(), centerY - centerY())
        }

        val m = Matrix()

        // Camera2 sample style: base mapping
        m.setRectToRect(viewRect, bufferRect, Matrix.ScaleToFit.FILL)

        val sx = vw / bufferRect.width()
        val sy = vh / bufferRect.height()
        val scale = if (centerCrop) maxOf(sx, sy) else minOf(sx, sy)

        m.postScale(scale, scale, centerX, centerY)

        if (rotationDegrees != 0f) {
            m.postRotate(rotationDegrees, centerX, centerY)
        }

        tv.setTransform(m)
        tv.invalidate()
    }

    private fun applyBackTransform() {
        applyAspectTransform(
            leftTv,
            BACK_BUF_W,
            BACK_BUF_H,
            rotationDegrees = BACK_ROT_DEG,
            centerCrop = true
        )
    }

    private fun applyExtTransform() {
        applyAspectTransform(
            rightTv,
            extBufW,
            extBufH,
            rotationDegrees = EXT_ROT_DEG,
            centerCrop = true
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
        lastSuInfo = "su(id)=$idCode ${idOut.take(80)}"
        if (idCode != 0) {
            lastSuPrepErr = "SU yok/izin yok. Magisk popup gelirse GRANT et.\n$idOut"
            return false
        }

        runSu("setenforce 0")
        val (chCode, chOut) =
            runSu("chmod 666 /dev/video* /dev/v4l-subdev* 2>/dev/null; ls -l /dev/video0 2>/dev/null")
        if (chCode != 0) {
            lastSuPrepErr = "chmod failed: $chCode\n$chOut"
            return false
        }
        return true
    }

    private fun maybeStartBack() {
        val s = leftSurface ?: return
        if (!hasPermission) return
        if (backStarted.get() || backStarting.get()) return

        backStarting.set(true)
        camExec.execute {
            val ok = nativeStartBackPreview(s, 60)
            runOnUiThread {
                backStarting.set(false)
                backStarted.set(ok)
                updateOverlay()
            }
        }
    }

    private fun maybeStartExt() {
        val s = rightSurface ?: return
        if (!hasPermission) return
        if (extStarted.get() || extStarting.get()) return

        extStarting.set(true)
        camExec.execute {
            val prepOk = prepareUvcAccess()
            val ok = if (prepOk) nativeStartExternalPreview(s, 60) else false

            // === FIX: native mode'u al -> extBufW/extBufH güncelle -> transform'u yeniden uygula ===
            val mode = if (ok) nativeGetExtChosenMode() else ""

            runOnUiThread {
                extStarting.set(false)
                extStarted.set(ok)

                if (mode.isNotBlank()) {
                    updateExtBufFromModeString(mode)
                    applyExtTransform()
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
        camExec.execute { nativeStopExternalPreview() }
    }

    private fun updateOverlay() {
        val now = SystemClock.elapsedRealtimeNanos()

        val bTs = nativeGetBackLastSensorTimestampNs()
        val bAgeMs = if (bTs > 0) (now - bTs) / 1_000_000.0 else -1.0
        val bFps = nativeGetBackEstimatedFpsX100() / 100.0
        val bChosen = nativeGetBackChosenFps()
        val bErr = nativeGetBackLastError()

        val eTs = nativeGetExtLastSensorTimestampNs()
        val eAgeMs = if (eTs > 0) (now - eTs) / 1_000_000.0 else -1.0
        val eFps = nativeGetExtEstimatedFpsX100() / 100.0
        val eChosen = nativeGetExtChosenFps()
        val eErr = nativeGetExtLastError()
        val eMode = nativeGetExtChosenMode()

        fun ageStr(v: Double) = if (v >= 0) String.format("%.2f ms", v) else "n/a"

        debugOverlay.text =
            "BACK  age=${ageStr(bAgeMs)}  fps=${String.format("%.2f", bFps)}  chosen=$bChosen\n" +
                    (if (bErr.isNotBlank()) "BACK err=$bErr\n" else "") +
                    "EXT   age=${ageStr(eAgeMs)}  fps=${
                        String.format(
                            "%.2f",
                            eFps
                        )
                    }  chosen=$eChosen  mode=$eMode\n" +
                    "EXTbuf ${extBufW}x${extBufH}\n" +
                    (if (lastSuPrepErr.isNotBlank()) "SU/perm: $lastSuPrepErr\n" else "") +
                    (if (eErr.isNotBlank()) "EXT  err=$eErr\n" else "") +
                    "DBG: $lastSuInfo"
    }

    private external fun nativeStartBackPreview(surface: Surface, desiredFps: Int): Boolean
    private external fun nativeStopBackPreview()

    private external fun nativeStartExternalPreview(surface: Surface, desiredFps: Int): Boolean
    private external fun nativeStopExternalPreview()

    private external fun nativeGetBackLastSensorTimestampNs(): Long
    private external fun nativeGetBackEstimatedFpsX100(): Int
    private external fun nativeGetBackChosenFps(): Int
    private external fun nativeGetBackLastError(): String

    private external fun nativeGetExtLastSensorTimestampNs(): Long
    private external fun nativeGetExtEstimatedFpsX100(): Int
    private external fun nativeGetExtChosenFps(): Int
    private external fun nativeGetExtLastError(): String
    private external fun nativeGetExtChosenMode(): String
}


