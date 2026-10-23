#pragma once

#define GHP_VERSION_MAJOR 0
#define GHP_VERSION_MINOR 2
#define GHP_VERSION_REVISION 1
#define GHP_VERSION_BUILD 3

#define GHP_STR2(x) #x
#define GHP_STR(x) GHP_STR2(x)
#define GHP_VERSION_STRING \
  GHP_STR(GHP_VERSION_MAJOR) "." GHP_STR(GHP_VERSION_MINOR) "." GHP_STR(GHP_VERSION_REVISION)

// Oldest Far build this plugin is built and tested for.
#define GHP_MIN_FAR_BUILD 6638
