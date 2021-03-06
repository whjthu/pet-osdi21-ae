#include <common.h>
#include <iostream>
#include <operator.h>
#include <tensor.h>

using namespace tpm;

const int n = 2, c = 2, h = 4, w = 4;
int main() {
    auto t1 = new Tensor({n, c, h, w});
    // std::cout << t1->size() << std::endl;
    std::cout << "1,0,0,1: " << t1->getOffset({1, 0, 0, 1}) << std::endl;
    std::cout << "1,0,3,3: " << t1->getOffset({1, 0, 3, 3}) << std::endl;
    t1->dataMalloc();
    auto t1d = t1->getDataPtr();
    for (size_t i = 0; i < t1->size(); ++i)
        t1d[i] = i;
    t1->print();

    delete t1;

    return 0;
}
