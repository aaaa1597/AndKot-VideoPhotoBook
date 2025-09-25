/*===============================================================================
Copyright (c) 2024 PTC Inc. and/or Its Subsidiary Companies. All Rights Reserved.

Vuforia is a trademark of PTC Inc., registered in the United States and other
countries.
===============================================================================*/

#include <jni.h>

#include "GLESRenderer.h"
#include "AppController.h"
#include "Log.h"

#include "VuforiaEngine/VuforiaEngine.h"

#include <GLES3/gl31.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#include <cassert>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

// Cross-platform AppController providing high level Vuforia Engine operations
AppController controller;

/// JVM pointer obtained in the JNI_OnLoad method below and consumed in the cross-platform code
void* javaVM;

// Struct to hold data that we need to store between calls
struct
{
    JavaVM* vm = nullptr;
    jobject activity = nullptr;
    AAssetManager* assetManager = nullptr;
    jmethodID presentErrorMethodID = nullptr;
    jmethodID initDoneMethodID = nullptr;

    GLESRenderer renderer;

    bool usingARCore{ false };
} gWrapperData;

// Provider pointers that allow for interacting with ARCore
std::optional<VuPlatformARCoreInfo> gARCoreInfo{};

// Mutex protecting access to the provider pointers in gARCoreInfo
std::mutex gARCoreInfoMutex;


// JNI Implementation
#ifdef __cplusplus
extern "C" {
#endif

// Local method declarations

/// Present a pop-up to the user with the given error string.
void callPresentError(const char* errorString);

/// Get the Fusion Provider (ARCore) pointers by querying Vuforia Engine.
/**
 * Assumes gARCoreInfoMutex has been locked.
 * @return true on success; false otherwise.
 */
bool getFusionProviderPointers();

#if 0
/// Demonstrate how to use the Fusion Provider (ARCore) pointers obtained from Vuforia Engine.
void accessFusionProviderPointers();
#endif


/// Called by JNI binding when the client code loads the library
/**
 * Called ONLY when a Java Activity is used - won't get called if a native Activity is used
 */
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void* reserved)
{
    LOG("JNI_OnLoad");

    assert(vm != nullptr);

    JNIEnv* env;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    {
        LOG("Failed to get JNI environment from JavaVM");
        return -1;
    }

    // Cache Java VM
    javaVM = vm;
    gWrapperData.vm = vm;

    LOG("Retrieved and stored JavaVM");
    return JNI_VERSION_1_6;
}


JNIEXPORT void JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_initAR(JNIEnv *env, jclass claz,
                                                              jobject activity,
                                                              jobject assetManager,
                                                              jint target) {
    // Store the Java VM pointer so we can get a JNIEnv in callbacks
    if (env->GetJavaVM(&gWrapperData.vm) != 0)
    {
        return;
    }
    gWrapperData.activity = env->NewGlobalRef(activity);
    jclass clazz = env->GetObjectClass(activity);
    gWrapperData.presentErrorMethodID = env->GetMethodID(clazz, "presentError", "(Ljava/lang/String;)V");
    gWrapperData.initDoneMethodID = env->GetMethodID(clazz, "initDone", "()V");
    env->DeleteLocalRef(clazz);

    AppController::InitConfig initConfig;
    initConfig.vbRenderBackend = VuRenderVBBackendType::VU_RENDER_VB_BACKEND_GLES3;
    initConfig.appData = activity;

    // Setup callbacks
    initConfig.errorMessageCallback = [](const char* errorString) {
        LOG("Error message callback invoked. Message: %s", errorString);
        callPresentError(errorString);
    };
    initConfig.vuforiaEngineErrorCallback = [](VuErrorCode errorCode) {
        LOG("Vuforia engine error callback invoked. Error code: 0x%02x", errorCode);

        auto resetARCoreInfo = []() {
            std::scoped_lock lock(gARCoreInfoMutex);
            gARCoreInfo.reset();
        };

        switch (errorCode)
        {
            case VU_ENGINE_ERROR_INVALID_LICENSE:
                LOG("ARCore pointers might have been invalidated. Resetting our copy.");
                resetARCoreInfo();
                callPresentError("Invalid license reported");
                break;
            case VU_ENGINE_ERROR_CAMERA_DEVICE_LOST:
                LOG("ARCore pointers might have been invalidated. Resetting our copy.");
                resetARCoreInfo();
                callPresentError("Camera device lost");
                break;
            case VU_ENGINE_ERROR_PLATFORM_FUSION_PROVIDER_INFO_INVALIDATED:
                LOG("ARCore pointers will be invalidated. Resetting our copy.");
                resetARCoreInfo();
                break;
            default:
                LOG("Got an unexpected Engine error code 0x%02x", errorCode);
                assert(false);
                break;
        }
    };
    initConfig.initDoneCallback = []() {
        LOG("InitDone callback");
        JNIEnv* env = nullptr;
        if (gWrapperData.vm->GetEnv((void**)&env, JNI_VERSION_1_6) == 0)
        {
            env->CallVoidMethod(gWrapperData.activity, gWrapperData.initDoneMethodID);
        }
    };

    // Get a native AAssetManager
    gWrapperData.assetManager = AAssetManager_fromJava(env, assetManager);
    if (gWrapperData.assetManager == nullptr)
    {
        initConfig.errorMessageCallback("Error: Failed to get the asset manager");
        return;
    }

    // Start Vuforia initialization
    controller.initAR(initConfig, target);
}


JNIEXPORT jboolean JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_startAR(JNIEnv *env, jclass clazz) {
    // Update usingARCore flag to avoid checking this every frame
    auto platformController = controller.getPlatformController();
    assert(platformController);

    VuFusionProviderPlatformType fusionProviderPlatformType{ VU_FUSION_PROVIDER_PLATFORM_TYPE_UNKNOWN };
    vuPlatformControllerGetFusionProviderPlatformType(platformController, &fusionProviderPlatformType);
    gWrapperData.usingARCore = (fusionProviderPlatformType == VU_FUSION_PROVIDER_PLATFORM_TYPE_ARCORE);

    if (!controller.startAR())
    {
        return JNI_FALSE;
    }

    if (gWrapperData.usingARCore)
    {
        if (!getFusionProviderPointers())
        {
            // This error is not fatal, as these pointers can also be obtained later
            LOG("Failed to get ARCore provider pointers right after starting Vuforia");
        }
    }

    return JNI_TRUE;
}


JNIEXPORT void JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_stopAR(JNIEnv *env, jclass clazz) {
    controller.stopAR();
}


JNIEXPORT void JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_deinitAR(JNIEnv *env, jclass clazz) {
    controller.deinitAR();

    gWrapperData.assetManager = nullptr;
    env->DeleteGlobalRef(gWrapperData.activity);
    gWrapperData.activity = nullptr;
}


JNIEXPORT void JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_cameraPerformAutoFocus(JNIEnv *env, jclass clazz) {
    controller.cameraPerformAutoFocus();
}


JNIEXPORT void JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_cameraRestoreAutoFocus(JNIEnv *env, jclass clazz) {
    controller.cameraRestoreAutoFocus();
}


JNIEXPORT void JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_initRendering(JNIEnv *env, jclass clazz) {
    // Define clear color
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    if (!gWrapperData.renderer.init(gWrapperData.assetManager))
    {
        __android_log_print(ANDROID_LOG_ERROR, "aaaaa", "Error initialising renderer");
    }
}


JNIEXPORT void JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_setTextures(JNIEnv *env, jclass clazz,
                                                                              jint astronautWidth,
                                                                              jint astronautHeight,
                                                                              jobject astronautByteBuffer) {
    // Textures are loaded using the BitmapFactory which isn't available from the NDK.
    // They are loaded in the Kotlin code and passed to this method to create GLES textures.
    auto astronautBytes = static_cast<unsigned char*>(env->GetDirectBufferAddress(astronautByteBuffer));
    gWrapperData.renderer.setAstronautTexture(astronautWidth, astronautHeight, astronautBytes);
}


JNIEXPORT void JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_deinitRendering(JNIEnv *env, jclass clazz) {
    gWrapperData.renderer.deinit();
}


JNIEXPORT jboolean JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_configureRendering(JNIEnv *env, jclass clazz,
                                                                                     jint width,
                                                                                     jint height,
                                                                                     jint orientation,
                                                                                     jint rotation) {
    std::vector<int> androidOrientation{ orientation, rotation };
    return controller.configureRendering(width, height, androidOrientation.data()) ? JNI_TRUE : JNI_FALSE;
}


JNIEXPORT jboolean JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_renderFrame(JNIEnv *env, jclass clazz) {
    if (!controller.isARStarted())
    {
        return JNI_FALSE;
    }

    // Clear colour and depth buffers
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    int vbTextureUnit = 0;
    VuRenderVideoBackgroundData renderVideoBackgroundData;
    renderVideoBackgroundData.renderData = nullptr;
    renderVideoBackgroundData.textureData = nullptr;
    renderVideoBackgroundData.textureUnitData = &vbTextureUnit;
    double viewport[6];
    if (controller.prepareToRender(viewport, &renderVideoBackgroundData))
    {
        // Set viewport for current view
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
//        __android_log_print(ANDROID_LOG_DEBUG, "aaaaa", "viewport(%f, %f, %f, %f)", viewport[0], viewport[1], viewport[2], viewport[3]);

        auto renderState = controller.getRenderState();
        gWrapperData.renderer.renderVideoBackground(renderState.vbProjectionMatrix, renderState.vbMesh->pos, renderState.vbMesh->tex,
                                                    renderState.vbMesh->numFaces, renderState.vbMesh->faceIndices, vbTextureUnit);

        VuMatrix44F worldOriginProjection;
        VuMatrix44F worldOriginModelView;
        if (controller.getOrigin(worldOriginProjection, worldOriginModelView))
        {
            gWrapperData.renderer.renderWorldOrigin(worldOriginProjection, worldOriginModelView);
        }

        auto [imageTargetList, CNT] = controller.createImageTargetList();
        for (int idx = 0; idx < CNT; idx++) {
            VuObservation* observation = nullptr;
            if (vuObservationListGetElement(imageTargetList.get(), idx, &observation) != VU_SUCCESS)
                continue;

            assert(observation);
            assert(vuObservationIsType(observation, VU_OBSERVATION_IMAGE_TARGET_TYPE) == VU_TRUE);
            assert(vuObservationHasPoseInfo(observation) == VU_TRUE);

            VuMatrix44F trackableProjection;
            VuMatrix44F trackableModelView;
            VuMatrix44F trackableModelViewScaled;
            VuImageInfo modelTargetGuideViewImage;
            VuBool guideViewImageHasChanged;
            VuVector2F markerSize;
            std::string tergetName;
            if (controller.getImageTargetResult(observation, trackableProjection, trackableModelView, trackableModelViewScaled, markerSize, tergetName))
            {
                if(tergetName == "001_stones_jpg")
                    gWrapperData.renderer.renderVideoPlayback(trackableProjection, trackableModelView, trackableModelViewScaled, markerSize);
                else if(tergetName == "002_cleyon")
                    gWrapperData.renderer.renderImageTarget(trackableProjection, trackableModelView, trackableModelViewScaled);
            }
        }
        imageTargetList.reset();
    }

    controller.finishRender();

    return JNI_TRUE;
}


void
callPresentError(const char* errorString)
{
    JNIEnv* env = nullptr;
    if (gWrapperData.vm->GetEnv((void**)&env, JNI_VERSION_1_6) == 0)
    {
        jstring error = env->NewStringUTF(errorString);
        env->CallVoidMethod(gWrapperData.activity, gWrapperData.presentErrorMethodID, error);
        env->DeleteLocalRef(error);
    }
}


bool
getFusionProviderPointers()
{
    // This method is only called if Vuforia Engine is using ARCore, see startAR above

    auto platformController = controller.getPlatformController();
    assert(platformController);

    VuPlatformARCoreInfo arCoreInfo;
    if (vuPlatformControllerGetARCoreInfo(platformController, &arCoreInfo) != VU_SUCCESS)
    {
        return false;
    }

    gARCoreInfo = arCoreInfo;
    return true;
}


#if 0
void
accessFusionProviderPointers()
{
    // This method is only called if Vuforia Engine is using ARCore, see startAR above

    if (!gARCoreInfoMutex.try_lock())
    {
        LOG("Failed to lock mutex for accessing ARCore provider pointers");
        return;
    }

    if (!gARCoreInfo)
    {
        if (!getFusionProviderPointers())
        {
            LOG("Failed to get ARCore provider pointers");
            gARCoreInfoMutex.unlock();
            return;
        }
    }

    auto arSession = static_cast<ArSession*>(gARCoreInfo->arSession);
    auto arFrame = static_cast<ArFrame*>(gARCoreInfo->arFrame);

    ArConfig* config;
    ArConfig_create((const ArSession*)arSession, &config);
    ArSession_getConfig(arSession, config);

    // Simple demonstration of access to the ARCore objects,
    // log the focus mode and current tracking state.
    // This code runs every frame but only generates log messages
    // every 5 seconds to avoid flooding the log output.
    static std::chrono::time_point lastLog = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if ((now - lastLog) > std::chrono::seconds(5))
    {
        lastLog = now;

        ArFocusMode focusMode;
        ArConfig_getFocusMode((const ArSession*)arSession, config, &focusMode);
        LOG("focusMode is %d", focusMode);

        ArCamera* arCamera;
        ArFrame_acquireCamera(arSession, arFrame, &arCamera);
        ArTrackingState trackingState;
        ArCamera_getTrackingState(arSession, arCamera, &trackingState);
        switch (trackingState)
        {
            case AR_TRACKING_STATE_STOPPED:
                LOG("trackingState is stopped");
                break;
            case AR_TRACKING_STATE_TRACKING:
                LOG("trackingState is tracking");
                break;
            case AR_TRACKING_STATE_PAUSED:
                LOG("trackingState is paused");
                break;
        }
    }

    ArConfig_destroy(config);

    gARCoreInfoMutex.unlock();
}
#endif

#ifdef __cplusplus
}
#endif

extern "C"
JNIEXPORT jint JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_initVideoTexture(JNIEnv *env, jclass clazz) {
    gWrapperData.renderer._vTextureId = -1;
    glGenTextures(1, &gWrapperData.renderer._vTextureId);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, gWrapperData.renderer._vTextureId);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    return static_cast<jint>(gWrapperData.renderer._vTextureId);
}
extern "C"
JNIEXPORT void JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_nativeOnSurfaceChanged(JNIEnv *env,
                                                                                jclass clazz,
                                                                                jint width,
                                                                                jint height) {
    glViewport(0, 0, width, height);
}
extern "C"
JNIEXPORT void JNICALL
Java_com_aaa_vuforiavideoplaybacksample_VuforiaWrapperKt_nativeSetVideoSize(JNIEnv *env,
                                                                            jclass clazz,
                                                                            jint width,
                                                                            jint height) {
    gWrapperData.renderer._vVideoWidth = static_cast<float>(width);
    gWrapperData.renderer._vVideoHeight= static_cast<float>(height);
}