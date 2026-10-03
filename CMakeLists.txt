cmake_minimum_required(VERSION 3.16)
project(FinalBuildCrowd VERSION 0.1.0 LANGUAGES C)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  include(CTest)
endif()

option(FBS_CROWD_BUILD_TESTS "Build crowd C tests" ON)
option(FBS_CROWD_ENABLE_SANITIZERS "Enable native GCC/Clang ASan and UBSan" OFF)
set(FBS_CROWD_FLECS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/flecs" CACHE PATH
  "Directory containing the pinned Flecs 4.1.6 flecs_no_addons.c/.h (vendored copy by default)")

# ---- Flecs (private, pinned, core only) --------------------------------------
set(_flecs_c "${FBS_CROWD_FLECS_SOURCE}/flecs_no_addons.c")
set(_flecs_h "${FBS_CROWD_FLECS_SOURCE}/flecs_no_addons.h")
if(NOT EXISTS "${_flecs_c}" OR NOT EXISTS "${_flecs_h}")
  message(FATAL_ERROR "Flecs source not found in FBS_CROWD_FLECS_SOURCE=${FBS_CROWD_FLECS_SOURCE}")
endif()
file(STRINGS "${_flecs_h}" _flecs_ver REGEX "^#define FLECS_VERSION_(MAJOR|MINOR|PATCH) [0-9]+")
string(REGEX REPLACE ".*MAJOR ([0-9]+).*MINOR ([0-9]+).*PATCH ([0-9]+).*" "\\1.\\2.\\3" _flecs_ver "${_flecs_ver}")
if(NOT _flecs_ver STREQUAL "4.1.6")
  message(FATAL_ERROR "The crowd module is pinned to Flecs 4.1.6; ${_flecs_h} is ${_flecs_ver}")
endif()
add_library(fbs_crowd_flecs OBJECT "${_flecs_c}")
set_target_properties(fbs_crowd_flecs PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES
  C_EXTENSIONS YES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_include_directories(fbs_crowd_flecs PUBLIC $<BUILD_INTERFACE:${FBS_CROWD_FLECS_SOURCE}>)
# Upstream code: keep its warnings out of our logs.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fbs_crowd_flecs PRIVATE -w)
endif()

# ---- fbs::crowd ----------------------------------------------------------------
add_library(fbs_crowd src/crowd.c $<TARGET_OBJECTS:fbs_crowd_flecs>)
add_library(fbs::crowd ALIAS fbs_crowd)
set_target_properties(fbs_crowd PROPERTIES
  EXPORT_NAME crowd OUTPUT_NAME fbs_crowd
  C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  POSITION_INDEPENDENT_CODE ON VERSION ${PROJECT_VERSION} SOVERSION 0)
target_include_directories(fbs_crowd PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Flecs headers are private: they never appear in the public include path.
target_include_directories(fbs_crowd SYSTEM PRIVATE "${FBS_CROWD_FLECS_SOURCE}")
if(NOT MSVC)
  target_link_libraries(fbs_crowd PUBLIC m)
endif()
if(WIN32)
  target_link_libraries(fbs_crowd PUBLIC dbghelp) # Flecs' Windows backtrace (SymInitialize)
endif()
if(NOT EMSCRIPTEN AND NOT WIN32)
  find_package(Threads REQUIRED) # Flecs' default POSIX OS API (mutexes)
  target_link_libraries(fbs_crowd PUBLIC Threads::Threads)
endif()

set(FBS_CROWD_WARNINGS "")
if(MSVC)
  set(FBS_CROWD_WARNINGS /W4)
else()
  set(FBS_CROWD_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
    -ffp-contract=off)
endif()
target_compile_options(fbs_crowd PRIVATE ${FBS_CROWD_WARNINGS})
if(FBS_CROWD_ENABLE_SANITIZERS)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT EMSCRIPTEN)
    foreach(_t fbs_crowd fbs_crowd_flecs)
      target_compile_options(${_t} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    endforeach()
    target_link_options(fbs_crowd PUBLIC -fsanitize=address,undefined)
  else()
    message(FATAL_ERROR "Crowd sanitizers require native GCC or Clang")
  endif()
endif()

if(FBS_CROWD_BUILD_TESTS AND BUILD_TESTING)
  enable_testing()
  foreach(_name core flecs_alloc)
    add_executable(fbs_crowd_test_${_name} tests/test_${_name}.c)
    target_link_libraries(fbs_crowd_test_${_name} PRIVATE fbs::crowd)
    set_target_properties(fbs_crowd_test_${_name} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_compile_options(fbs_crowd_test_${_name} PRIVATE ${FBS_CROWD_WARNINGS})
    if(NOT MSVC)
      target_compile_options(fbs_crowd_test_${_name} PRIVATE -UNDEBUG)
    endif()
    if(NOT EMSCRIPTEN)
      add_test(NAME crowd_${_name} COMMAND fbs_crowd_test_${_name})
    endif()
  endforeach()
  # The allocation test installs a counting Flecs OS API; it may see Flecs.
  target_include_directories(fbs_crowd_test_flecs_alloc SYSTEM PRIVATE "${FBS_CROWD_FLECS_SOURCE}")
  add_executable(fbs_crowd_bench tests/bench.c)
  target_link_libraries(fbs_crowd_bench PRIVATE fbs::crowd)
  set_target_properties(fbs_crowd_bench PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_compile_options(fbs_crowd_bench PRIVATE ${FBS_CROWD_WARNINGS})
  if(EMSCRIPTEN)
    foreach(_t fbs_crowd_test_core fbs_crowd_test_flecs_alloc fbs_crowd_bench)
      target_link_options(${_t} PRIVATE -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node -sEXIT_RUNTIME=1
        -sSTACK_SIZE=1048576)
      # CommonJS output: the repository package.json declares ES modules.
      set_target_properties(${_t} PROPERTIES SUFFIX ".cjs")
      add_test(NAME crowd_wasm_${_t} COMMAND node ${CMAKE_CURRENT_BINARY_DIR}/${_t}.cjs)
    endforeach()
  endif()
endif()

install(TARGETS fbs_crowd EXPORT FinalBuildCrowdTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/fbs/crowd.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fbs)
install(EXPORT FinalBuildCrowdTargets NAMESPACE fbs::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FinalBuildCrowd)
configure_package_config_file(cmake/FinalBuildCrowdConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/FinalBuildCrowdConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FinalBuildCrowd)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/FinalBuildCrowdConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/FinalBuildCrowdConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/FinalBuildCrowdConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FinalBuildCrowd)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/FinalBuildCrowd/docs)
install(FILES NOTICE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/FinalBuildCrowd)
install(FILES "${FBS_CROWD_FLECS_SOURCE}/LICENSE"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/FinalBuildCrowd RENAME FLECS-LICENSE)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  add_executable(fbs_example examples/basic.c)
  target_link_libraries(fbs_example PRIVATE fbs::crowd)
  if(BUILD_TESTING)
    add_test(NAME example COMMAND fbs_example)
  endif()
endif()

install(FILES LICENSE THIRD_PARTY_NOTICES DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/fbs-crowd)
