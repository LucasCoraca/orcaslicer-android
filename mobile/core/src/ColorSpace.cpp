// slic3r/Utils/ColorSpaceConvert: the colour space conversions (FlushVolCalc's RGB2HSV, the colour distances), copied
// out of its translation unit without its wxColour helpers
#include <algorithm>
#include <cmath>
#include <tuple>
#include "slic3r/Utils/ColorSpaceConvert.hpp"

// the file's constants (ColorSpaceConvert.cpp), which the copied functions use
const static float param_13    = 1.0f / 3.0f;
const static float param_16116 = 16.0f / 116.0f;
const static float Xn          = 0.950456f;
const static float Yn          = 1.0f;
const static float Zn          = 1.088754f;

#include "slic3r/GUI/Filaments.inc.csc"
