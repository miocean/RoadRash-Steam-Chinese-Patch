// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// Single source of the mod version: shown in the game window title, the log and the installer.
#define RRHD_VERSION "2.17.3"
#define RRHD_NAME "RoadRash30-MultiFix"
// build/hd_build.hpp (written by build.py) defines RRHD_BUILD_STAMP and RRHD_BUILD_SUFFIX: a date code in test builds, empty in releases.
#if defined(__has_include)
#if __has_include("hd_build.hpp")
#include "hd_build.hpp"
#endif
#endif
#ifndef RRHD_BUILD_STAMP
#define RRHD_BUILD_STAMP ""
#define RRHD_BUILD_SUFFIX ""
#endif
#define RRHD_WINDOW_TITLE "Road Rash - 30th Anniversary [MultiFix " RRHD_VERSION RRHD_BUILD_SUFFIX "]"
