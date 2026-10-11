cmake_minimum_required(VERSION 3.14)

# vita.toolchain.cmake and lib/libc_bridge declare very old minimums, rejected by CMake 4+
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(ff7_vita C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Vita application configuration
set(VITA_APP_NAME "FINAL FANTASY VII")
set(VITA_TITLEID "FFSQ00007")
set(VITA_VPKNAME "ff7_vita")
set(VITA_VERSION "01.00") # SFO APP_VER (must be ##.##)

# Port version: bump the last field on every build handed out for testing
# (v0.00.01, v0.00.02, ... it may go past 100). The VPK lands in build/VPK/.
set(PORT_VERSION "0.00.06")
add_definitions(-DPORT_VERSION="v${PORT_VERSION}")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# Optional: used for "send" and "dump" targets
set(PSVITAIP "192.168.0.198" CACHE STRING "PSVita IP (for FTP access)")
set(PARSECOREPATH "$ENV{PARSECOREPATH}" CACHE STRING "vita-parse-core main.py")

# Game-specific definitions
# No game data is ever shipped in the VPK: the user copies the original files
# from their own Play Store install into DATA_PATH (see docs/DADOS.md).
set(DATA_PATH "ux0:data/ffvii-vita/") # Path to data (with trailing /)
set(APK_PATH "${DATA_PATH}base.apk") # Original APK (v1.0.29)
set(SO_APK_ENTRY "lib/armeabi-v7a/libjni_ff7.so") # .so loaded straight from the APK
set(OBB_PATH "${DATA_PATH}main.30.com.square_enix.android_googleplay.FFVII.obb")
# Android passes getExternalFilesDir()/Documents to setDataPath; the game then
# uses dirname(path) + "/Documents/" for saves, options and APP.LOG.
set(DOCUMENTS_PATH "${DATA_PATH}Documents")
set(LOG_PATH "${DATA_PATH}logs.txt")
add_definitions(-DDATA_PATH="${DATA_PATH}" -DAPK_PATH="${APK_PATH}"
                -DSO_APK_ENTRY="${SO_APK_ENTRY}" -DOBB_PATH="${OBB_PATH}"
                -DDOCUMENTS_PATH="${DOCUMENTS_PATH}" -DLOG_PATH="${LOG_PATH}")

# Loader logs (info/warn/error) go to LOG_PATH even in Release builds.
# Debug builds additionally enable l_debug() and verbose loader output.
option(ENABLE_LOGGING "Write loader logs to ${LOG_PATH}" ON)
if (ENABLE_LOGGING)
  add_definitions(-DENABLE_LOGGING)
endif()

# Port type
option(NDK_PORT "This should be on if the game is done with Android NDK (has an android_main entry point)" OFF)
if (NDK_PORT)
  add_definitions(-DNDK_PORT)
endif()

option(USE_SCELIBC_IO "Use SceLibcBridge for IO and some other std functions" ON)
if (USE_SCELIBC_IO)
  add_definitions(-DUSE_SCELIBC_IO)
endif()

set(SHADER_FORMAT "GLSL") # Preferred shader format (one of 'GLSL', 'CG', 'GXP')
if (${SHADER_FORMAT} STREQUAL "GLSL")
  add_definitions(-DUSE_GLSL_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "CG")
  add_definitions(-DUSE_CG_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "GXP")
  add_definitions(-DUSE_GXP_SHADERS)
else()
  message(FATAL_ERROR "SHADER_FORMAT option can only be one of: 'GLSL', 'CG', 'GXP'")
endif()

set(valid_dump_formats GLSL CG)
option(DUMP_COMPILED_SHADERS "When using GLSL or CG shader format, cache compiled shaders on disk" OFF)
if (DUMP_COMPILED_SHADERS)
  if (SHADER_FORMAT IN_LIST valid_dump_formats)
    add_definitions(-DDUMP_COMPILED_SHADERS)
  else()
    message(WARNING "DUMP_COMPILED_SHADERS has no effect for the chosen shader format ${SHADER_FORMAT}")
  endif()
endif()

# Unique debug definition for loader's logging to not interfere with anything
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_SOLOADER)
  # Uncomment this for verbose FalsoJNI logging:
  #add_definitions(-DFALSOJNI_DEBUGLEVEL=0)
  # Uncomment this for OpenGL logging:
  #add_definitions(-DDEBUG_OPENGL)
  # Uncomment this for so_util logging:
  #add_definitions(-DSO_UTIL_VERBOSE=1)
  # Uncomment this for verbose FalsoNDK logging (separate things):
  #add_definitions(-DDEBUG_POLL_AND_WAKE=1)
  #add_definitions(-DDEBUG_EPOLL=1)
  #add_definitions(-DDEBUG_PIPEFD=1)
  #add_definitions(-DDEBUG_CALLBACKS=1)
  # Space-separated flags passed to vitaGL
  set(VITAGL_MAKE_FLAGS "SOFTFP_ABI=1 TEST=1 LOG_ERRORS=1")
else()
  # Space-separated flags passed to vitaGL
  set(VITAGL_MAKE_FLAGS "SOFTFP_ABI=1 NO_DEBUG=1")
endif()

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -mfloat-abi=softfp -std=gnu11 -Wno-deprecated")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q -mfloat-abi=softfp -std=gnu++20 -Wno-write-strings -Wno-psabi")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O1 -g3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1 -g3")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g0 -ffast-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g0 -ffast-math")
endif()

set(SOURCES
    source/main.c
    source/dynlib.c
    source/java.c
    source/patch.c
    source/reimpl/bits/_ctype.c
    source/reimpl/errno.c
    source/reimpl/io.c
    source/reimpl/log.c
    source/reimpl/mem.c
    source/reimpl/pthr.c
    source/reimpl/sys.c
    source/reimpl/time64.c
    source/utils/dialog.c
    source/utils/glutil.c
    source/utils/init.c
    source/utils/logger.c
    source/utils/settings.c
    source/utils/utils.c
    source/utils/zip.c
    lib/falso_jni/FalsoJNI.c
    lib/falso_jni/FalsoJNI_ImplBridge.c
    lib/falso_jni/FalsoJNI_Logger.c
    lib/stb/stb_image.c
    source/reimpl/asset_manager.c
    source/reimpl/egl_extra.c
    source/reimpl/obb_fd.c
    source/reimpl/movie.c
    lib/falso_jni/converter.c
    lib/sha1/sha1.c
    lib/fios/fios.c
    lib/so_util/so_util.c)

if(NOT NDK_PORT)
list(APPEND SOURCES
  source/reimpl/controls.c
  source/reimpl/controls.h)
endif()

add_executable(${CMAKE_PROJECT_NAME} ${SOURCES})
# vitaShaRK/SceShaccCgExt are C++: link with g++ so libstdc++ resolves properly
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# OpenSL ES built from source (lib/opensles), with its logs routed to logs.txt
set(OPENSLES_DIR ${CMAKE_SOURCE_DIR}/lib/opensles/libopensles)
set(OPENSLES_OBJS OpenSLESUT MPH_to OpenSLES_IID classes devices trace locks sles sllog
    SndFile Vita IOutputMix IOutputMixExt sync IID_to_MPH ThreadPool C3DGroup
    CAudioPlayer CAudioRecorder CEngine COutputMix IBassBoost IBufferQueue IEffectSend
    IEngine IDynamicInterfaceManagement IEnvironmentalReverb IEqualizer IMuteSolo IObject
    IPlay IPlaybackRate IPrefetchStatus IPresetReverb IRecord ISeek IVirtualizer IVolume)
list(TRANSFORM OPENSLES_OBJS PREPEND ${OPENSLES_DIR}/)
list(TRANSFORM OPENSLES_OBJS APPEND .c)
add_library(OpenSLES_src STATIC ${OPENSLES_OBJS})
target_compile_definitions(OpenSLES_src PRIVATE USE_OUTPUTMIXEXT USE_SDL USE_SNDFILE)
target_include_directories(OpenSLES_src PRIVATE ${OPENSLES_DIR} ${CMAKE_SOURCE_DIR}/lib/opensles/include)
target_compile_options(OpenSLES_src PRIVATE -std=gnu17 -O2 -w
                       -include ${CMAKE_SOURCE_DIR}/source/utils/log_redirect_stdio.h)

add_subdirectory(lib/libc_bridge)
add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge)

if(NDK_PORT)
  add_subdirectory(lib/falso_ndk)
  add_dependencies(${CMAKE_PROJECT_NAME} FalsoNDK)
endif()

# VitaGL built from lib/vitagl via its own Makefile.
# Made this way so that testing quick adjustments to VitaGL is possible,
# as well as changing build flags in a fast and efficient way.
separate_arguments(VITAGL_MAKE_ARGS UNIX_COMMAND "${VITAGL_MAKE_FLAGS}")

set(VITAGL_DIR ${CMAKE_SOURCE_DIR}/lib/vitagl)
set(VITAGL_LIB ${VITAGL_DIR}/libvitaGL.a)

# Force a full rebuild of vitaGL whenever VITAGL_MAKE_FLAGS change
set(VITAGL_FLAGS_STAMP ${CMAKE_BINARY_DIR}/vitagl_flags.stamp)
set(vitagl_old_flags "")
if (EXISTS ${VITAGL_FLAGS_STAMP})
  file(READ ${VITAGL_FLAGS_STAMP} vitagl_old_flags)
endif()
if (NOT vitagl_old_flags STREQUAL VITAGL_MAKE_FLAGS)
  file(GLOB_RECURSE vitagl_stale_objs ${VITAGL_DIR}/source/*.o)
  file(REMOVE ${vitagl_stale_objs} ${VITAGL_LIB})
  file(WRITE ${VITAGL_FLAGS_STAMP} "${VITAGL_MAKE_FLAGS}")
endif()

include(ProcessorCount)
ProcessorCount(VITAGL_NPROC)
if (VITAGL_NPROC EQUAL 0)
  set(VITAGL_NPROC 4)
endif()

add_custom_target(vitaGL_build
                  COMMAND make -C ${VITAGL_DIR} -j${VITAGL_NPROC} ${VITAGL_MAKE_ARGS}
                  BYPRODUCTS ${VITAGL_LIB}
                  COMMENT "Building VitaGL"
                  VERBATIM)

add_library(vitaGL STATIC IMPORTED)
set_target_properties(vitaGL PROPERTIES
                      IMPORTED_LOCATION ${VITAGL_LIB}
                      INTERFACE_INCLUDE_DIRECTORIES ${VITAGL_DIR}/source)
add_dependencies(${CMAKE_PROJECT_NAME} vitaGL_build)

# so_util and FalsoJNI print with sceClibPrintf directly; redirect it into
# the logger so their messages (unresolved imports, JNI lookups) reach logs.txt
set_source_files_properties(lib/so_util/so_util.c lib/falso_jni/FalsoJNI.c
                            lib/falso_jni/FalsoJNI_Logger.c lib/falso_jni/FalsoJNI_ImplBridge.c
                            PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/source/utils/log_redirect.h")

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib
                           )

target_include_directories(${CMAKE_PROJECT_NAME}
                           PUBLIC ${CMAKE_SOURCE_DIR}/source
                           )

if(NDK_PORT)
  target_link_libraries(${CMAKE_PROJECT_NAME}
                        FalsoNDK
                        )
endif()

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      OpenSLES_src
                      sndfile
                      mp3lame
                      mpg123
                      opus
                      vorbis
                      ogg
                      vorbisenc
                      vorbisfile
                      FLAC
                      stdc++
                      vitaGL
                      vitashark
                      SceShaccCgExt
                      taihen_stub
                      SceShaccCg_stub
                      z
                      m
                      ScePgf_stub
                      mathneon
                      kubridge_stub
                      ${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a
                      SceHid_stub
                      SceTouch_stub
                      SceMotion_stub
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceAudio_stub
                      SceAudioIn_stub
                      SceCommonDialog_stub
                      SceDisplay_stub
                      SceKernelDmacMgr_stub
                      SceFios2_stub
                      SceCtrl_stub
                      SceGxm_stub
                      ScePower_stub
                      SceTouch_stub
                      SceVshBridge_stub
                      )

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)
# vita.cmake's vita_create_vpk() joins its `-a` arguments into one string,
# which breaks on spaces in the project path; this rule passes them verbatim.
set(VPK_FILES
    extras/cpuinfo cpuinfo
    extras/meminfo meminfo
    extras/urandom urandom
    extras/livearea/icon0.png sce_sys/icon0.png
    extras/livearea/pic0.png sce_sys/pic0.png
    extras/livearea/startup.png sce_sys/livearea/contents/startup.png
    extras/livearea/bg0.png sce_sys/livearea/contents/bg0.png
    extras/livearea/template.xml sce_sys/livearea/contents/template.xml)

set(VPK_ARGS "")
set(VPK_DEPS "")
list(LENGTH VPK_FILES vpk_left)
while(vpk_left GREATER 0)
  list(POP_FRONT VPK_FILES vpk_src vpk_dst)
  list(APPEND VPK_ARGS -a "${CMAKE_SOURCE_DIR}/${vpk_src}=${vpk_dst}")
  list(APPEND VPK_DEPS "${CMAKE_SOURCE_DIR}/${vpk_src}")
  list(LENGTH VPK_FILES vpk_left)
endwhile()

separate_arguments(MKSFOEX_ARGS UNIX_COMMAND "${VITA_MKSFOEX_FLAGS}")
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/param.sfo
                   COMMAND ${VITASDK}/bin/vita-mksfoex ${MKSFOEX_ARGS}
                           -s APP_VER=${VITA_VERSION} -s TITLE_ID=${VITA_TITLEID}
                           ${VITA_APP_NAME} ${CMAKE_BINARY_DIR}/param.sfo
                   DEPENDS eboot.bin-self
                   VERBATIM)
set(VPK_OUT ${CMAKE_BINARY_DIR}/VPK/${VITA_VPKNAME}_v${PORT_VERSION}.vpk)
add_custom_command(OUTPUT ${VPK_OUT}
                   COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/VPK
                   COMMAND ${VITASDK}/bin/vita-pack-vpk ${VPK_ARGS}
                           -s ${CMAKE_BINARY_DIR}/param.sfo
                           -b ${CMAKE_BINARY_DIR}/eboot.bin
                           ${VPK_OUT}
                   DEPENDS ${CMAKE_BINARY_DIR}/param.sfo ${CMAKE_BINARY_DIR}/eboot.bin eboot.bin-self ${VPK_DEPS}
                   COMMENT "Building VPK/${VITA_VPKNAME}_v${PORT_VERSION}.vpk"
                   VERBATIM)
add_custom_target(${VITA_VPKNAME}.vpk-vpk ALL
                  COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-vpk.sh ${VPK_OUT}
                  DEPENDS ${VPK_OUT}
                  VERBATIM)

add_custom_target(send
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ${VITA_TITLEID} | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(send_kvdb
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ux0:/app/${VITA_TITLEID}/eboot.bin | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(dump
                  COMMAND ${CMAKE_SOURCE_DIR}/extras/scripts/get_dump.sh ${PSVITAIP}
                  COMMAND python3 ${PARSECOREPATH} coredump ${CMAKE_PROJECT_NAME}
                  )

add_custom_target(reboot
                  COMMAND echo reboot | nc ${PSVITAIP} 1338
                  )
