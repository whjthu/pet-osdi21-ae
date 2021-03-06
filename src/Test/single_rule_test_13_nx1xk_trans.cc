#include "generator.h"
#include "graph.h"
#include "operator.h"
#include "search_engine.h"
#include "tensor.h"
#include <cstdlib>
#include <iostream>

const int n = 16, c = 256, h = 28, w = 28;
const int f = 256, r = 1, s = 3;

int main() {
    auto g = new tpm::Graph();
    auto i0 = g->tensor({n, c, h, w});
    auto w0 = g->tensor({f, c, r, s});
    auto o0 = g->tensor({n, f, h, w});
    auto op0 = g->conv(i0, w0, o0, tpm::ConvOp::Same);
    auto i1 = g->tensor({n, c, h, w});
    auto w1 = g->tensor({f, c, r, s});
    auto o1 = g->tensor({n, f, h, w});
    auto op1 = g->conv(i1, w1, o1, tpm::ConvOp::Same);
    auto i2 = g->tensor({n, c, h, w});
    auto w2 = g->tensor({f, c, r, s});
    auto o2 = g->tensor({n, f, h, w});
    auto op2 = g->conv(i2, w2, o2, tpm::ConvOp::Same);
    auto i3 = g->tensor({n, c, h, w});
    auto w3 = g->tensor({f, c, s, r});
    auto o3 = g->tensor({n, f, h, w});
    auto op3 = g->conv(i3, w3, o3, tpm::ConvOp::Same);

    auto sg = new tpm::SubGraph({op0, op1, op2, op3});
    for (auto tensor : sg->getTensors())
        tensor->dataMalloc();
    for (auto tensor : sg->getInputs())
        tensor->dataRand();
    // for (auto op : sg->getOperators())
    //     op->compute();

    std::vector<std::shared_ptr<tpm::Operator>> all_ops;
    // all_ops.emplace_back(new tpm::TransposeOp(0, {0, 1, {-1, 2}, 3}, 2));
    // all_ops.emplace_back(new tpm::TransposeOp(2, {{0, 2}, 1, -1, 3}, -2));
    // all_ops.emplace_back(new tpm::ConvOp(tpm::ConvOp::Same, 1, 1, 1, 1));
    tpm::Generator mutant{};
    std::vector<tpm::SubGraph *> candidates;
    mutant.run(sg, candidates);
    std::cout << "candidates found: " << candidates.size() << std::endl;
    for (auto candidate : candidates)
        candidate->print();

    assert(candidates.size() == 3);

    // auto g1 = new tpm::Graph();
    // auto t0 = g1->tensor({n, c, h, w});
    // auto t1 = g1->tensor({f, c, r, s});
    // auto p0 = g1->conv(t0, t1, 1, 1, 1, 1, 1, 1);
    // t0->dataMalloc();
    // t1->dataMalloc();
    // auto t2 = p0->compute();
    // std::cout << "t0: " << std::endl;
    // t0->print();
    // std::cout << "t1: " << std::endl;
    // t1->print();
    // std::cout << "t2: " << std::endl;
    // t2->print();
    // auto p1 = g1->transpose(t0, 0, {0, 1, {-1, 2}, 3}, 2);
    // auto t3 = p1->compute();
    // std::cout << "t3: " << std::endl;
    // t3->print();
    // auto p2 = g1->conv(t3, t1, 1, 1, 1, 1, 1, 1);
    // auto t4 = p2->compute();
    // std::cout << "t4: " << std::endl;
    // t4->print();
    // auto p3 = g1->transpose(t4, 2, {{0, 2}, 1, -1, 3}, -2);
    // auto t5 = p3->compute();
    // std::cout << "t5: " << std::endl;
    // t5->print();

    delete g;
    delete sg;
    return 0;
}
