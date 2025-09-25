package com.aaa.vuforiavideoplaybacksample

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.PixelFormat
import android.graphics.SurfaceTexture
import android.opengl.GLSurfaceView
import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.util.Log
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.addCallback
import androidx.appcompat.app.AlertDialog
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.VideoSize
import androidx.media3.exoplayer.ExoPlayer
import com.aaa.vuforiavideoplaybacksample.databinding.ActivityMainBinding
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.IOException
import java.nio.ByteBuffer
import java.util.Timer
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
import kotlin.concurrent.schedule

const val IMAGE_TARGET_ID = 0
const val MODEL_TARGET_ID = 1
val REQUIRED_PERMISSIONS = arrayOf(Manifest.permission.CAMERA)

class MainActivity : AppCompatActivity() {
    private lateinit var _binding: ActivityMainBinding
    private var mVuforiaStarted = false
    private var mSurfaceChanged = false
    private var mWindowDisplayRotation = Surface.ROTATION_0
    private lateinit var gestureDetector: GestureDetector
    private var mWidth = 0
    private var mHeight = 0
    private var mPermissionsRequested = false;
    private var surfaceTexture: SurfaceTexture? = null
    private var exoPlayer: ExoPlayer? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        _binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(_binding.root)

        /* Prevent screen from dimming */
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        mVuforiaStarted = false
        mSurfaceChanged = true

        _binding.viwGlsurface.setEGLContextClientVersion(3)
        _binding.viwGlsurface.holder.setFormat(PixelFormat.TRANSLUCENT)
        _binding.viwGlsurface.setEGLConfigChooser(8,8,8,8,0,0)
        _binding.viwGlsurface.setZOrderOnTop(true)
        _binding.viwGlsurface.setRenderer(object : GLSurfaceView.Renderer {
            override fun onSurfaceCreated(gl: GL10, config: EGLConfig) {
                initRendering()
            }
            override fun onSurfaceChanged(gl: GL10, width: Int, height: Int) {
                // Store values for later use
                mWidth = width
                mHeight = height

                // Re-load textures in case they got destroyed
                val astronautbitmap = loadBitmapFromAssets(this@MainActivity, "ImageTargets/Astronaut.jpg")
                val astronautTexture: ByteBuffer? = astronautbitmap?.let { bitmap ->
                                                        ByteBuffer.allocateDirect(bitmap.byteCount).apply {
                                                            bitmap.copyPixelsToBuffer(this)
                                                            rewind()
                                                        }
                                                    }

                if (astronautTexture != null) {
                    setTextures(astronautbitmap.width, astronautbitmap.height, astronautTexture)
                } else {
                    Log.e("VuforiaSample", "Failed to load astronaut or lander texture")
                }

                val textureId = initVideoTexture()
                if (textureId < 0)
                    throw RuntimeException("Failed to create native texture")
                CoroutineScope(Dispatchers.Main).launch {
                    surfaceTexture = SurfaceTexture(textureId)
                    val surface = Surface(surfaceTexture)
                    exoPlayer?.setVideoSurface(surface)
                }

                nativeOnSurfaceChanged(width, height)

                // Update flag to tell us we need to update Vuforia configuration
                mSurfaceChanged = true
            }

            override fun onDrawFrame(gl: GL10) {
                if (mVuforiaStarted) {

                    surfaceTexture?.updateTexImage()

                    if (mSurfaceChanged || mWindowDisplayRotation != this@MainActivity.display.rotation) {
                        mSurfaceChanged = false
                        mWindowDisplayRotation = this@MainActivity.display.rotation

                        // Pass rendering parameters to Vuforia Engine
                        configureRendering(mWidth, mHeight, resources.configuration.orientation, mWindowDisplayRotation)
                    }

                    // OpenGL rendering of Video Background and augmentations is implemented in native code
                    val didRender = renderFrame()
                    if (didRender && _binding.loadingIndicator.visibility != View.GONE) {
                        lifecycleScope.launch {
                            _binding.loadingIndicator.visibility = View.GONE
                        }
                    }
                }
            }
        })
        _binding.viwGlsurface.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {}
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                deinitRendering()
            }
        })
        _binding.viwGlsurface.visibility = View.GONE

        if (runtimePermissionsGranted()) {
            // Start Vuforia initialization in a coroutine
            lifecycleScope.launch {
                initializeVuforia()
            }
        }

        gestureDetector = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
                override fun onSingleTapUp(e: MotionEvent): Boolean {
                    cameraPerformAutoFocus()
                    Timer("RestoreAutoFocus", false).schedule(2000) {
                        cameraRestoreAutoFocus()
                    }
                    return true
                }

                override fun onDoubleTap(e: MotionEvent): Boolean {
                    super.onDoubleTap(e)
                    return true
                }
            })

        onBackPressedDispatcher.addCallback(this) {
            // Hide the GLView while we clean up
            _binding.viwGlsurface.visibility = View.INVISIBLE
            // Stop Vuforia Engine and call parent to navigate back
            stopAR()
            mVuforiaStarted = false
            deinitAR()
        }

        exoPlayer = ExoPlayer.Builder(this).build().apply {
            /* Specify the video file located in res/raw. */
            val uri = "android.resource://${this@MainActivity.packageName}/${R.raw.vuforiasizzlereel}"
            Log.d("aaaaa", "uri=$uri")
            val mediaItem = MediaItem.fromUri(uri)
            setMediaItem(mediaItem)

            repeatMode = Player.REPEAT_MODE_ONE /* Loop Playback. */
            playWhenReady = true /* Start playback immediately. */

            addListener(object : Player.Listener {
                override fun onVideoSizeChanged(videoSize: VideoSize) {
                    /* Pass the video size to the C++ side. */
                    nativeSetVideoSize(videoSize.width, videoSize.height)
                }
            })

            prepare()
        }
    }

    private fun runtimePermissionsGranted(): Boolean {
        var result = true
        for (permission in REQUIRED_PERMISSIONS) {
            result = result && ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED
        }
        return result
    }

    private suspend fun initializeVuforia() {
        return withContext(Dispatchers.Default) {
            initAR(this@MainActivity, this@MainActivity.assets, IMAGE_TARGET_ID)
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        gestureDetector.onTouchEvent(event)
        return super.onTouchEvent(event)
    }

    override fun onResume() {
        super.onResume()
        if (runtimePermissionsGranted()) {
            if (!mPermissionsRequested && mVuforiaStarted) {
                lifecycleScope.launch(Dispatchers.Unconfined) {
                    startAR()
                }
            }
        } else {
            ActivityCompat.requestPermissions(this, REQUIRED_PERMISSIONS, 0)
            mPermissionsRequested = true
        }
    }

    override fun onPause() {
        super.onPause()
        stopAR()
    }

    override fun onDestroy() {
        super.onDestroy()
        exoPlayer?.release()
        exoPlayer = null
        // Hide the GLView while we clean up
        _binding.viwGlsurface.visibility = View.INVISIBLE
        // Stop Vuforia Engine and call parent to navigate back
        stopAR()
        mVuforiaStarted = false
        deinitAR()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        mPermissionsRequested = false

        if (permissions.isEmpty()) {
            // Permissions request was cancelled
            Toast.makeText(this, "The permission request was cancelled. You must grant Camera permission to access AR features of this application.", Toast.LENGTH_LONG).show()
            finish()
            return
        }

        if (runtimePermissionsGranted()) {
            // Start Vuforia initialization in a coroutine
            lifecycleScope.launch(Dispatchers.Unconfined) {
                initializeVuforia()
            }
        } else {
            Toast.makeText(this, "You must grant Camera permission to access AR features of this application.", Toast.LENGTH_LONG).show()
            finish()
        }
    }

    @Suppress("unused")
    private fun presentError(message: String) {
        val builder: AlertDialog.Builder = AlertDialog.Builder(this@MainActivity)
                                                        .setTitle(R.string.error_dialog_title)
                                                        .setMessage(message)
                                                        .setPositiveButton(R.string.ok) { _, _ ->
                                                                                stopAR()
                                                                                deinitAR()
                                                                                this@MainActivity.finish()
                                                                            }
        // This is called from another coroutine not on the Main thread
        // Showing the UI needs to be on the main thread
        lifecycleScope.launch(Dispatchers.Main) {
            builder.create().show()
        }
    }


    @Suppress("unused")
    private fun initDone() {
        mVuforiaStarted = startAR()
        if (!mVuforiaStarted) {
            Log.e("VuforiaSample", "Failed to start AR")
        }
        // Show the GLView
        lifecycleScope.launch(Dispatchers.Main) {
            _binding.viwGlsurface.visibility = View.VISIBLE
        }
    }

    companion object {
        // Used to load the 'vuforiavideoplaybacksample' library on application startup.
        init {
            System.loadLibrary("vuforiavideoplaybacksample")
        }
    }
}

fun loadBitmapFromAssets(context: Context, fileName: String): Bitmap? {
    return try {
        val inputStream = context.assets.open(fileName)
        BitmapFactory.decodeStream(inputStream)
    } catch (e: IOException) {
        e.printStackTrace()
        null
    }
}
