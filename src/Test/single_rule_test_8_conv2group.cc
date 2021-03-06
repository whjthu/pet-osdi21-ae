#include "generator.h"
#include "graph.h"
#include "operator.h"
#include "search_engine.h"
#include "tensor.h"
#include <cstdlib>
#include <iostream>

const int n = 1, c = 2, h = 4, w = 4;
const int f = 4, r = 3, s = 3;

int main() {
    auto g = new tpm::Graph();
    auto i0 = g->tensor({n, c, h, w});
    auto w0 = g->tensor({f, c, r, s});
    auto o1 = g->tensor({n, f, h, w});
    auto op0 = g->conv(i0, w0, o1, 1, 1, 1, 1, 1, 1);

    auto sg = new tpm::SubGraph({op0});
    for (auto tensor : sg->getTensors())
        tensor->dataMalloc();
    // for (auto tensor : sg->getInputs())
    //     tensor->dataRand();
    // for (auto op : sg->getOperators())
    //     op->compute();

    std::vector<std::shared_ptr<tpm::Operator>> all_ops;
    all_ops.emplace_back(new tpm::ExtendOp(1, 1));
    all_ops.emplace_back(new tpm::ConvOp(1, 1, 1, 1, 1, 1));
    tpm::Generator mutant{};
    std::vector<tpm::SubGraph *> candidates;
    mutant.run(sg, candidates, 2, all_ops);
    std::cout << "candidates found: " << candidates.size() << std::endl;
    for (auto candidate : candidates)
        candidate->print();

    assert(candidates.size() == 2);

    // auto g1 = new tpm::Graph();
    // auto t0 = g1->tensor({n, c, h, w});
    // auto t1 = g1->tensor({f, c, r, s});
    // auto p0 = g1->conv(t0, t1, 1, 1, 1, 1, 1, 1);
    // t0->dataRand();
    // t1->dataRand();
    // auto t2 = p0->compute();
    // std::cout << "t0: " << std::endl;
    // t0->print();
    // std::cout << "t1: " << std::endl;
    // t1->print();
    // std::cout << "t2: " << std::endl;
    // t2->print();
    // auto p1 = g1->extend(t0, 1, 1);
    // auto t3 = p1->compute();
    // std::cout << "t3: " << std::endl;
    // t3->print();
    // auto p3 = g1->conv(t3, t1, 1, 1, 1, 1, 1, 1);
    // auto t4 = p3->compute();
    // std::cout << "t4: " << std::endl;
    // t4->print();
    // std::cout << "t2: " << std::endl;
    // t2->print();

    delete g;
    delete sg;
    return 0;
}
