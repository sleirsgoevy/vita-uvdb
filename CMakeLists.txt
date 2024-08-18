cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(uvdb)
include("${VITASDK}/share/vita.cmake" REQUIRED)

add_library(uvdb STATIC uvdb.c)

# due to bug in vita.cmake (fself/vita-create-elf flags being cached), we need to build test executable first

add_executable(uvdb_test
  ${VITASDK}/share/gcc-arm-vita-eabi/samples/common/debugScreen.c
  test.c
)

target_include_directories(uvdb_test
    PRIVATE
    /$(VITASDK)/share/gcc-arm-vita-eabi/samples/common/
)

target_link_libraries(uvdb_test
  uvdb
  taihen_stub
  kubridge_stub
  SceLibKernel_stub
  SceNetPs_stub_weak
  SceKernelModulemgr_stub
  SceKernelThreadMgr_stub
  SceProcessmgr_stub
  SceSysmem_stub
  SceDisplay_stub
)

vita_create_self(uvdb_test.self uvdb_test
  UNSAFE
)

vita_create_vpk(uvdb_test.vpk UVDB00001 uvdb_test.self
  VERSION "01.00"
  NAME "UVDB-Test"
)

add_executable(uvdb_module
  module.c
)

target_link_libraries(uvdb_module
  uvdb
  taihen_stub
  kubridge_stub
  SceLibKernel_stub
  SceNetPs_stub_weak
  SceKernelModulemgr_stub
  SceKernelThreadMgr_stub
  SceProcessmgr_stub
  SceSysmem_stub
)

set_target_properties(uvdb_module
  PROPERTIES LINK_FLAGS "-nostdlib"
)

vita_create_self(uvdb.suprx uvdb_module
  UNSAFE
  CONFIG uvdb.yml
)

install(FILES ${CMAKE_BINARY_DIR}/libuvdb.a
  DESTINATION lib
)

install(FILES uvdb.h
  DESTINATION include
)

install(FILES ${CMAKE_BINARY_DIR}/uvdb.suprx
  DESTINATION modules
)
