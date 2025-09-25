package com.aaa.vuforiavideoplaybacksample

import android.app.Activity
import android.content.res.AssetManager
import java.nio.ByteBuffer

external fun initRendering()
external fun setTextures(astronautWidth: Int, astronautHeight: Int, astronautBytes: ByteBuffer)
external fun configureRendering(width: Int, height: Int, orientation: Int, rotation: Int) : Boolean
external fun renderFrame() : Boolean
external fun deinitRendering()
external fun initAR(activity: Activity, assetManager: AssetManager, target: Int)
external fun deinitAR()
external fun startAR() : Boolean
external fun stopAR()
external fun cameraPerformAutoFocus()
external fun cameraRestoreAutoFocus()
external fun initVideoTexture(): Int
external fun nativeOnSurfaceChanged(width: Int, height: Int)
external fun nativeSetVideoSize(width: Int, height: Int)