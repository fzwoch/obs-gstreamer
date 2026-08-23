cmake_minimum_required(VERSION 3.28...3.30)

# Link raw libraries instead of an IMPORTED_TARGET: the macOS app-bundle
# helper walks :: targets and would try to embed GStreamer dylibs without
# their transitive glib dependency tree. Raw libs resolve against the system
# installation at runtime instead.
find_package(PkgConfig REQUIRED)
pkg_check_modules(
  GSTREAMER
  REQUIRED
  gstreamer-1.0
  gstreamer-video-1.0
  gstreamer-audio-1.0
  gstreamer-app-1.0
  gstreamer-net-1.0)

# Mirror meson's vcs_tag(): embed the plugin git hash as the version string.
find_package(Git)
set(VCS_TAG unknown)
if(Git_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE VCS_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
message(STATUS "obs-gstreamer version: ${VCS_TAG}")

# The sources use installed-layout includes (<obs/obs-module.h>). In-tree,
# headers live in <source>/libobs, so provide the missing 'obs' path level
# via a symlink next to the generated version.c.
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/obs")
  file(CREATE_LINK "${CMAKE_SOURCE_DIR}/libobs" "${CMAKE_CURRENT_BINARY_DIR}/obs"
       SYMBOLIC)
endif()

add_library(obs-gstreamer MODULE)
add_library(OBS::gstreamer ALIAS obs-gstreamer)

configure_file(version.c.in version.c @ONLY)

target_sources(
  obs-gstreamer
  PRIVATE gstreamer.c
          gstreamer-source.c
          gstreamer-encoder.c
          gstreamer-filter.c
          gstreamer-output.c
          plugin-i18n.h
          ${CMAKE_CURRENT_BINARY_DIR}/version.c)

target_include_directories(obs-gstreamer
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                   ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(obs-gstreamer SYSTEM
                           PRIVATE ${GSTREAMER_INCLUDE_DIRS})
target_link_directories(obs-gstreamer PRIVATE ${GSTREAMER_LIBRARY_DIRS})

target_link_libraries(obs-gstreamer PRIVATE OBS::libobs ${GSTREAMER_LIBRARIES})

# Vendored plugin code is not warning-clean under OBS's -Werror set.
target_compile_options(obs-gstreamer PRIVATE -Wno-unused-parameter)

set_target_properties_obs(obs-gstreamer PROPERTIES FOLDER plugins PREFIX "")
