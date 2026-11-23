cmake_minimum_required(VERSION 3.15)

project(ResolumeOMT
    VERSION 0.1.0
    LANGUAGES CXX
)

# Universal by default so the plugins load in both Apple Silicon and Intel
# builds of Resolume. This must sit before the first target: CMake latches
# the architecture list then, and a later setting is silently ignored.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()
if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(NOMINMAX)
endif()

option(RESOLUME_OMT_BUILD_TOOLS "Build the offline harness" ON)

# ---------------------------------------------------------------------------
# The OMT runtime the bundles carry. libomt is a Native AOT .NET library and
# cannot be built here; it comes from libomtnet's releases (MIT). It is
# loaded at run time, never linked (see source/OmtRuntime.h).
# ---------------------------------------------------------------------------
if(APPLE)
    set(OMT_RUNTIME_DIR "$ENV{HOME}/.local/lib/omt" CACHE PATH
        "Folder holding libomt.dylib and libvmx.dylib")
    set(OMT_RUNTIME_FILES "${OMT_RUNTIME_DIR}/libomt.dylib" "${OMT_RUNTIME_DIR}/libvmx.dylib")
elseif(WIN32)
    set(OMT_RUNTIME_DIR "" CACHE PATH
        "Folder holding libomt.dll, libvmx.dll and libomtnet.dll")
    set(OMT_RUNTIME_FILES "${OMT_RUNTIME_DIR}/libomt.dll" "${OMT_RUNTIME_DIR}/libvmx.dll")
endif()
foreach(f ${OMT_RUNTIME_FILES})
    if(NOT EXISTS "${f}")
        message(WARNING "OMT runtime file missing: ${f} - the bundles will build but report "
                        "'OMT runtime not found'. Set -DOMT_RUNTIME_DIR=...")
    endif()
endforeach()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()
set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Both plugins' code, as an OBJECT library so both bundles and the harness
# are built from exactly the same objects. OBJECT, not STATIC: the entry
# files register themselves from a file-scope constructor that nothing
# references, which an archive would let the linker drop.
# ---------------------------------------------------------------------------
add_library(somt_core OBJECT
    source/Diag.h
    source/Diag.cpp
    source/OmtRuntime.h
    source/OmtRuntime.cpp
    source/Discovery.h
    source/Discovery.cpp
    source/Receiver.h
    source/Receiver.cpp
    source/Sender.h
    source/Sender.cpp
    source/GLState.h
    source/OmtIn.h
    source/OmtIn.cpp
    source/OmtOut.h
    source/OmtOut.cpp
)
target_include_directories(somt_core PUBLIC source third_party/omt)
target_link_libraries(somt_core PUBLIC ffgl::sdk)
target_compile_definitions(somt_core PUBLIC
    RESOLUME_OMT_VERSION="${PROJECT_VERSION}"
    OMT_NO_IMPLICIT_LINK)
set_target_properties(somt_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(somt_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(somt_core PUBLIC GLEW::GLEW)
endif()
find_package(Threads REQUIRED)
target_link_libraries(somt_core PUBLIC Threads::Threads)

function(somt_add_plugin target entry bundle_name identifier)
    add_library(${target} MODULE ${entry})
    target_link_libraries(${target} PRIVATE somt_core)
    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "${bundle_name}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${identifier}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        target_link_libraries(${target} PRIVATE
            "-framework OpenGL" "-framework Carbon" "-framework AppKit")
        # The runtime goes in Contents/Frameworks, where OmtRuntime looks
        # first; libomt finds libvmx beside itself (measured, no rpath).
        foreach(f ${OMT_RUNTIME_FILES})
            if(EXISTS "${f}")
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${f}" "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks/"
                    VERBATIM)
            endif()
        endforeach()
        # Sign the bundle after the runtime is inside it (ad hoc unless
        # SOMT_SIGN_IDENTITY is set), or Gatekeeper sees a modified bundle.
        set(identity "-")
        if(DEFINED ENV{SOMT_SIGN_IDENTITY})
            set(identity "$ENV{SOMT_SIGN_IDENTITY}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND codesign --force --sign "${identity}" --timestamp=none
                    "$<TARGET_BUNDLE_DIR:${target}>/Contents/Frameworks/libvmx.dylib"
                    "$<TARGET_BUNDLE_DIR:${target}>/Contents/Frameworks/libomt.dylib" || true
            COMMAND codesign --force --sign "${identity}" --timestamp=none "$<TARGET_BUNDLE_DIR:${target}>"
            VERBATIM)
    else()
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${bundle_name}")
        foreach(f ${OMT_RUNTIME_FILES})
            if(EXISTS "${f}")
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${f}" "$<TARGET_FILE_DIR:${target}>/"
                    VERBATIM)
            endif()
        endforeach()
    endif()
endfunction()

somt_add_plugin(OmtInPlugin source/SourcePlugin.cpp "SW OMT In" "com.stoatworks.ffgl.omt.in")
somt_add_plugin(OmtOutPlugin source/EffectPlugin.cpp "SW OMT Out" "com.stoatworks.ffgl.omt.out")

# ---------------------------------------------------------------------------
# Offline harness: drives the real plugin classes in a headless GL context
# and checks them against libomt itself (see tools/omttest).
# ---------------------------------------------------------------------------
if(RESOLUME_OMT_BUILD_TOOLS AND APPLE)
    add_executable(omttest tools/omttest/main.cpp)
    target_link_libraries(omttest PRIVATE somt_core "-framework OpenGL" "-framework AppKit")
    foreach(f ${OMT_RUNTIME_FILES})
        if(EXISTS "${f}")
            add_custom_command(TARGET omttest POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${f}" "$<TARGET_FILE_DIR:omttest>/"
                VERBATIM)
        endif()
    endforeach()
endif()

if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects" CACHE PATH "" FORCE)
endif()
install(TARGETS OmtInPlugin OmtOutPlugin
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)
