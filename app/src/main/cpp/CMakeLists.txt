# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 3.18.1)

# Declares and names the project.

project("amprack")

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.
add_library(libzix STATIC IMPORTED)

set_target_properties(libzix PROPERTIES IMPORTED_LOCATION
        ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libzix.a)


add_library( # Sets the name of the library.
        amprack

        # Sets the library as a shared library.
        SHARED

        # Provides a relative path to your source file(s).
        vringbuffer.c
        ringbuffer.c
        native-lib.cpp
        Engine.cpp
        upwaker.c
        FullDuplexStream.cpp
        SharedLibrary.cpp
        Plugin.cpp
        PluginControl.cpp
        Plugin2.cpp
        Meter.cpp
        LockFreeQueue.cpp
        lv2_ext.cpp
        worker.c
        atom.cpp
        symap.c
        )

# ALTODFX native effects.
#
# These are standalone LADSPA libraries: the host dlopen()s them by filename
# from the app's native library directory (see R.array.ladspa_plugins), exactly
# like the prebuilt effect .so files in ../jniLibs.
add_library(altodfx_tubedrive SHARED TubeDrivePlugin.cpp)

# Deliberately not -Ofast: fast-math lets the compiler assume every value is
# finite, which would defeat the plugin's NaN/Inf sanitising. -O3 keeps the
# checks intact and is fast enough for a 4x oversampled effect.
target_compile_options(altodfx_tubedrive PRIVATE -Wall -O3)

add_library(altodfx_autowah SHARED AutoWahPlugin.cpp)

# -O3 rather than -Ofast for the same reason as Tube Drive: fast-math would
# let the compiler drop the plugin's NaN/Inf guards.
target_compile_options(altodfx_autowah PRIVATE -Wall -O3)

add_library(altodfx_distortion SHARED DistortionPlugin.cpp)
# Same -O3 rationale as the other in-tree effects.
target_compile_options(altodfx_distortion PRIVATE -Wall -O3)

add_library(altodfx_fuzz SHARED FuzzPlugin.cpp)
# Same -O3 rationale as the other in-tree effects.
target_compile_options(altodfx_fuzz PRIVATE -Wall -O3)

add_library(altodfx_muff SHARED MuffPlugin.cpp)
# Same -O3 rationale as the other in-tree effects.
target_compile_options(altodfx_muff PRIVATE -Wall -O3)

add_library(altodfx_equalizer SHARED EqualizerPlugin.cpp)
# Same -O3 rationale as the other in-tree effects.
target_compile_options(altodfx_equalizer PRIVATE -Wall -O3)

add_library(altodfx_phaser SHARED PhaserPlugin.cpp)
target_compile_options(altodfx_phaser PRIVATE -Wall -O3)

# Searches for a specified prebuilt library and stores the path as a
# variable. Because CMake includes system libraries in the search path by
# default, you only need to specify the name of the public NDK library
# you want to add. CMake verifies that the library exists before
# completing its build.

find_library( # Sets the name of the path variable.
        log-lib

        # Specifies the name of the NDK library that
        # you want CMake to locate.
        log)

# Specifies libraries CMake should link to your target library. You
# can link multiple libraries, such as libraries you define in this
# build script, prebuilt third-party libraries, or system libraries.
find_package (oboe REQUIRED CONFIG)

target_link_libraries( # Specifies the target library.
        amprack
        libzix
        oboe::oboe
        android
        log

        # Links the target library to the log library
        # included in the NDK.
        ${log-lib})

target_include_directories(amprack PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI})

# optimize
target_compile_options(amprack PRIVATE -Wall -Ofast)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_RELEASE}")
