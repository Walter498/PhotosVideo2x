#pragma once
#include <cmath>
// Native gesture has already excluded bars, accessories, controls and edit mode.
static inline double PV2DoubleTapDelta(double x,double width) {
    if (!std::isfinite(x) || !std::isfinite(width) || width<=0 || x<0 || x>width) return 0;
    return x<width*0.5 ? -5.0 : 5.0;
}
