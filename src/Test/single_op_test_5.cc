#include "code_engine.h"
#include "graph.h"
#include "operator.h"
#include "search_engine.h"
#include "tensor.h"

int main(int argc, char **argv) {
    auto g = new tpm::Graph();
    int n = 64, c = 256, h = 14, w = 14, f = 256, wc = 256, r = 3, s = 3;
    int dh = 2, dw = 2;
    int ph = 2, pw = 2;
    if (argc == 9) {
        n = atoi(argv[1]);
        c = atoi(argv[2]);
        h = atoi(argv[3]);
        w = atoi(argv[4]);
        f = atoi(argv[5]);
        wc = atoi(argv[6]);
        r = atoi(argv[7]);
        s = atoi(argv[8]);
    } else if (argc == 8) {
        n = atoi(argv[1]);
        c = atoi(argv[2]);
        h = atoi(argv[3]);
        w = atoi(argv[4]);
        f = atoi(argv[5]);
        r = atoi(argv[6]);
        s = atoi(argv[7]);
    } else if (argc == 5) {
        n = atoi(argv[1]);
        c = atoi(argv[2]);
        h = atoi(argv[3]);
        w = h;
        f = atoi(argv[4]);
        wc = c;
    } else if (argc > 1) {
        std::cout << "Arg formats:" << std::endl;
        std::cout << "./bin n c h w f wc r s" << std::endl;
        std::cout << "./bin n c h w f r s" << std::endl;
        std::cout << "./bin n c insize f" << std::endl;
        return -1;
    }
    std::cout << "Conv: [ " << n << ", " << c << ", " << h << ", " << w
              << "], [" << f << ", " << wc << ", " << r << ", " << s << "]"
              << std::endl;
    auto i8 = g->tensor({n, c, h, w});
    auto i9 = g->tensor({n, f, h, w});

    auto w9 = g->tensor({f, wc, r, s});

    g->conv(i8, w9, i9, ph, pw, 1, 1, dh, dw);

    g->updateConnection();

    std::shared_ptr<tpm::SubGraph> graph, bestGraph;
    graph = std::make_shared<tpm::SubGraph>(g->getOperators());
    tpm::SearchEngine searchEngine;
    searchEngine.run(graph, bestGraph);
    tpm::CodeEngine codeEngine;
    auto perfEngine = searchEngine.exportPerfEngine();
    codeEngine.importPerfEngine(perfEngine);
    codeEngine.genCode(bestGraph, "res.cu");

    return 0;
}
