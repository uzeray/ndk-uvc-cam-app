package com.uzera.camcpp

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.res.Configuration
import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface
import android.view.TextureView
import androidx.appcompat.app.AppCompatActivity
import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.concurrent.ExecutorService
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.max

class UvcAction(
    private val activity: AppCompatActivity,
    private val extTv: TextureView,
    private val camExec: ExecutorService,
    private val hasPermissionProvider: () -> Boolean
) {
    private val DESIRED_FPS = 60

    private var extSt: SurfaceTexture? = null
    private var extSurface: Surface? = null

    private val extStarted = AtomicBoolean(false)
    private val extStarting = AtomicBoolean(false)

    private val uiHandler = Handler(Looper.getMainLooper())

    private var extModeCache: String = ""
    private var extFmtCache: String = ""
    private var extBufW: Int = 1920
    private var extBufH: Int = 1080

    private var lastSuInfo: String = ""
    private var lastSuPrepErr: String = ""

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                UsbManager.ACTION_USB_DEVICE_DETACHED -> activity.runOnUiThread { stopExt() }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    camExec.execute {
                        prepareUvcAccess()
                        activity.runOnUiThread {
                            stopExt()
                            extTv.postDelayed({ maybeStartExt() }, 200)
                        }
                    }
                }
            }
        }
    }

    fun setup() {
        extTv.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            applyExtTransform()
        }

        extTv.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                extSt = st
                st.setDefaultBufferSize(extBufW, extBufH)
                extSurface = Surface(st)

                applyExtTransform()
                maybeStartExt()
            }

            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {
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
    }

    fun registerUsbReceiver() {
        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        if (Build.VERSION.SDK_INT >= 33) {
            activity.registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            activity.registerReceiver(usbReceiver, filter)
        }
    }

    fun unregisterUsbReceiver() {
        try {
            activity.unregisterReceiver(usbReceiver)
        } catch (_: Throwable) {
        }
    }

    fun onResume() {
        applyExtTransform()
        maybeStartExt()
    }

    fun onPause() {
        stopExt()
    }

    fun onConfigurationChanged(newConfig: Configuration) {
        applyExtTransform()
    }

    fun onDestroy() {
        stopExt()
        extSurface?.release()
        extSurface = null
        extSt = null
        unregisterUsbReceiver()
    }

    fun maybeStartExt() {
        val s = extSurface ?: return
        if (!hasPermissionProvider()) return
        if (extStarted.get() || extStarting.get()) return

        extStarting.set(true)
        camExec.execute {
            val prepOk = prepareUvcAccess()
            val ok = if (prepOk) nativeStartExternalPreview(s, DESIRED_FPS) else false
            val mode = if (ok) nativeGetExtChosenMode() else ""

            activity.runOnUiThread {
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
            }
        }
    }

    fun stopExt() {
        if (!extStarted.get() && !extStarting.get()) return
        extStarted.set(false)
        extStarting.set(false)
        extModeCache = ""
        extFmtCache = ""
        camExec.execute { nativeStopExternalPreview() }
    }

    /**
     * UVC görüntüsünü extTv içine sığdırır (rot/mirror yok).
     * Amaç: “şerit” sorununu bitirmek ve 2160×800 alanı düzgün doldurmak.
     */
    private fun applyExtTransform() {
        val vw = extTv.width
        val vh = extTv.height
        if (vw <= 0 || vh <= 0) return

        val srcW = extBufW
        val srcH = extBufH
        if (srcW <= 0 || srcH <= 0) return

        val vwF = vw.toFloat()
        val vhF = vh.toFloat()
        val srcWF = srcW.toFloat()
        val srcHF = srcH.toFloat()

        var kx = 0.495f
        val ky = 1.80f

        val TRIM_RIGHT_PX = 15f
        kx = kx - (TRIM_RIGHT_PX / vwF)

        val LEFT_ANCHOR_PX = 0f   // -12f yaparsan 12px sola kayar

        val Y_OVERLAP_FIX_PX = 1f

        val scaleX = (vwF / srcWF) * kx
        val scaleY = (vhF / srcHF) * ky

        val scaledW = srcWF * scaleX

        val tx = LEFT_ANCHOR_PX + (scaledW / 2f) * 1.52f
        val ty = (vhF / 2f) + Y_OVERLAP_FIX_PX

        val m = Matrix()
        m.postTranslate(-srcWF / 2f, -srcHF / 2f)
        m.postScale(scaleX, scaleY)
        m.postTranslate(tx, ty)

        extTv.setTransform(m)
        extTv.invalidate()

        Log.i(
            TAG, "EXT anchor-left kx=$kx ky=$ky trim=$TRIM_RIGHT_PX left=$LEFT_ANCHOR_PX " +
                    "scaleX=$scaleX scaleY=$scaleY tx=$tx ty=$ty view=${vw}x${vh} src=${srcW}x${srcH}"
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

    private external fun nativeStartExternalPreview(surface: Surface, desiredFps: Int): Boolean
    private external fun nativeStopExternalPreview()

    private external fun nativeGetExtLastSensorTimestampNs(): Long
    private external fun nativeGetExtEstimatedFpsX100(): Int
    private external fun nativeGetExtLastError(): String
    private external fun nativeGetExtChosenMode(): String

    companion object {
        private const val TAG = "CamcppNDK"
    }
}
