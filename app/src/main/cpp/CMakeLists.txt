cmake_minimum_required(VERSION 3.22.1)

project("vuforiavideoplaybacksample")

add_library(VUFORIA_LIBRARY SHARED IMPORTED)
set_target_properties(VUFORIA_LIBRARY PROPERTIES IMPORTED_LOCATION
        ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libVuforiaEngine.so)

add_library(${CMAKE_PROJECT_NAME} SHARED
        AppController.cpp
        tiny_obj_loader.cpp
        # Android native sources
        GLESRenderer.cpp
        GLESUtils.cpp
        VuforiaWrapper.cpp)

target_include_directories(vuforiavideoplaybacksample PUBLIC include)

target_link_libraries(${CMAKE_PROJECT_NAME}
        android
        log
        GLESv3
        VUFORIA_LIBRARY)