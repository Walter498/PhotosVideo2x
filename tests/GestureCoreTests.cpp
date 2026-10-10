#include "../GestureCore.h"
#include <cassert>
#include <cstdio>
int main() {
 assert(PV2DoubleTapDelta(10,393)==-5);
 assert(PV2DoubleTapDelta(380,393)==5);
 assert(PV2DoubleTapDelta(196.5,393)==5);
 assert(PV2DoubleTapDelta(-1,393)==0);
 assert(PV2DoubleTapDelta(400,393)==0);
 assert(PV2DoubleTapDelta(5,0)==0);
 assert(PV2DoubleTapDelta(NAN,393)==0);
 assert(PV2DoubleTapDelta(0,INFINITY)==0);
 puts("PASS: double tap left/right 5s, invalid geometry boundaries");
}
