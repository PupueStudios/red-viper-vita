cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(red_viper C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Red Viper")
set(VITA_TITLEID "RVIPER001")
set(VITA_VERSION "01.00")

set(RV_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(RV_COMMON_FLAGS "-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fomit-frame-pointer -ffast-math -Wall -Wno-unused-variable -Wno-format-truncation")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${RV_COMMON_FLAGS} -std=gnu11")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -marm -mcpu=cortex-a9 -mfpu=neon")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DDEBUGLEVEL=0")
set(CMAKE_C_FLAGS_DEBUG "-O0 -g -DDEBUGLEVEL=2")

file(GLOB RV_COMMON_C ${RV_ROOT}/source/common/*.c)
file(GLOB RV_VITA_C ${RV_ROOT}/source/vita/*.c)
set(RV_ASM ${RV_ROOT}/source/arm/drc_exec.s ${RV_ROOT}/source/arm/drc_static.s)

include_directories(
  ${RV_ROOT}/include
  ${RV_ROOT}/source/common
  ${RV_ROOT}/source/common/inih
  ${RV_ROOT}/source/vita
)

add_executable(${PROJECT_NAME}
  ${RV_COMMON_C}
  ${RV_ROOT}/source/common/inih/ini.c
  ${RV_ASM}
  ${RV_VITA_C}
)

target_link_libraries(${PROJECT_NAME}
  vitaGL
  vitashark
  SceShaccCgExt
  mathneon
  taihen_stub
  SceShaccCg_stub
  SceKernelDmacMgr_stub
  png
  jpeg
  minizip
  z
  m
  stdc++
  SceGxm_stub
  SceDisplay_stub
  SceSysmodule_stub
  SceCommonDialog_stub
  ScePgf_stub
  SceCtrl_stub
  SceTouch_stub
  SceAudio_stub
  ScePower_stub
  SceAppUtil_stub
  SceAppMgr_stub
  SceRtc_stub
  SceShellSvc_stub
  SceSysmem_stub
  SceProcessmgr_stub
  SceLibKernel_stub
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
set(RV_VPK_ASSETS FILE ${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png assets/logo.png)
file(GLOB RV_ART ${CMAKE_CURRENT_SOURCE_DIR}/assets/images/*)
foreach(art ${RV_ART})
  get_filename_component(art_name ${art} NAME)
  list(APPEND RV_VPK_ASSETS FILE ${art} assets/images/${art_name})
endforeach()

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  ${RV_VPK_ASSETS}
)
