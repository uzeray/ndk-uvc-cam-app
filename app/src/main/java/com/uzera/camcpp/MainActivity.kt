package com.uzera.camcpp

import android.Manifest
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Color
import android.os.Bundle
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.view.TextureView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

class MainActivity : AppCompatActivity() {

    private lateinit var root: FrameLayout
    private lateinit var panel: FrameLayout

    private lateinit var backTv: TextureView
    private lateinit var extTv: TextureView

    private var hasPermission = true

    private val camExec: ExecutorService = Executors.newSingleThreadExecutor()

    private lateinit var backAction: BackAction
    private lateinit var uvcAction: UvcAction

    private val PANEL_W_PX = 2160
    private val PANEL_H_PX = 1600
    private val PANEL_POS_X = 250
    private val PANEL_POS_Y = 15

    init {
        System.loadLibrary("camcpp")
    }

    private val reqCamPerm = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        hasPermission = granted
        backAction.maybeStartBack()
        uvcAction.maybeStartExt()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        supportActionBar?.show()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        backTv = TextureView(this).apply {
            translationY = 14f
        }

        extTv = TextureView(this).apply {
            translationY = -30f
        }



        val stack = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            )
            clipChildren = true
            clipToPadding = true
        }

        stack.addView(
            backTv,
            LinearLayout.LayoutParams(PANEL_W_PX, 800)
        )

        stack.addView(
            extTv,
            LinearLayout.LayoutParams(PANEL_W_PX, 800)
        )

        panel = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(PANEL_W_PX, PANEL_H_PX).also {
                it.leftMargin = PANEL_POS_X
                it.topMargin = PANEL_POS_Y

            }
            clipChildren = true
            clipToPadding = true
            addView(stack)
        }

        root = FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
            clipChildren = true
            clipToPadding = true
            addView(panel)
        }

        setContentView(root)

        root.post { scalePanelToFitIfNeeded() }

        backAction = BackAction(
            activity = this,
            backTv = backTv,
            camExec = camExec
        ) { hasPermission }

        uvcAction = UvcAction(
            activity = this,
            extTv = extTv,
            camExec = camExec
        ) { hasPermission }

        backAction.setup()
        uvcAction.setup()
        uvcAction.registerUsbReceiver()

        hasPermission = ContextCompat.checkSelfPermission(
            this, Manifest.permission.CAMERA
        ) == PackageManager.PERMISSION_GRANTED

        if (!hasPermission) reqCamPerm.launch(Manifest.permission.CAMERA)
    }

    override fun onResume() {
        super.onResume()
        backAction.onResume()
        uvcAction.onResume()
    }

    override fun onPause() {
        backAction.onPause()
        uvcAction.onPause()
        super.onPause()
    }

    override fun onDestroy() {
        try {
            uvcAction.onDestroy()
            backAction.onDestroy()
        } finally {
            camExec.shutdownNow()
            super.onDestroy()
        }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        backAction.onConfigurationChanged()
        uvcAction.onConfigurationChanged()
        root.post { scalePanelToFitIfNeeded() }
    }

    private fun scalePanelToFitIfNeeded() {
        val rw = root.width
        val rh = root.height
        if (rw <= 0 || rh <= 0) return

        val availW = (rw - PANEL_POS_X).toFloat()
        val availH = (rh - PANEL_POS_Y).toFloat()
        if (availW <= 0f || availH <= 0f) return

        val sx = availW / PANEL_W_PX.toFloat()
        val sy = availH / PANEL_H_PX.toFloat()
        val scale = minOf(1f, sx, sy)

        panel.pivotX = 0f
        panel.pivotY = 0f
        panel.scaleX = scale
        panel.scaleY = scale
    }
}
