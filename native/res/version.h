#pragma once

// Single source of truth for the release version (resource.rc and the
// build script read it). Bump here only.
#define YTR_VERSION_MAJOR 4
#define YTR_VERSION_MINOR 2
#define YTR_VERSION_PATCH 2

#define YTR_STR2(x) #x
#define YTR_STR(x) YTR_STR2(x)
#define YTR_VERSION_STRING YTR_STR(YTR_VERSION_MAJOR) "." YTR_STR(YTR_VERSION_MINOR) "." YTR_STR(YTR_VERSION_PATCH)
#define YTR_VERSION_RC YTR_VERSION_MAJOR,YTR_VERSION_MINOR,YTR_VERSION_PATCH,0
