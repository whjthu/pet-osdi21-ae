#include "generator.h"
#include "graph.h"
#include "operator.h"
#include "search_engine.h"
#include "tensor.h"
#include "perf_engine.h"
#include <cstdio>
#include <iostream>

const int kh = 3, kw = 3,  dh = 1,  dw = 1,  ph = 0,  pw = 0,
           sh = 2,  sw = 2;

int main() {
    tpm::PerfEngine pe;
    auto i0 = tpm::Tensor({1, 64, 162, 162});
    auto maxpool = tpm::MaxPoolOp(&i0, kh, kw, dh, dw, ph, pw, sh, sw);
    auto avgpool = tpm::AvgPoolOp(&i0, kh, kw, ph, pw, sh, sw);
    printf("Maxpool perf %lf ms\n", maxpool.perf(&pe));
    printf("Avgpool perf %lf ms\n", avgpool.perf(&pe));
    return 0;
}
