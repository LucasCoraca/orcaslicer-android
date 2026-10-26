// nanosvg's implementation: upstream compiles it into the GUI library (BitmapCache.cpp), which the core does not link.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
