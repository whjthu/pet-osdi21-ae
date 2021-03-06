#include "code_engine.h"
#include "graph.h"
#include "operator.h"
#include "search_engine.h"
#include "tensor.h"

const int m = 16, n = 1024, k = 1024;
using namespace tpm;
int main(int argc, char **argv) {
    auto g = Graph{};
    auto i0 = g.tensor({m, k});
    auto w0 = g.tensor({k, n});
    auto w1 = g.tensor({k, n});
    auto w2 = g.tensor({k, n});
    // auto w3 = g.tensor({k, n});
    // auto w4 = g.tensor({k, n});
    g.matmul(i0, w0);
    g.matmul(i0, w1);
    g.matmul(i0, w2);

    g.updateConnection();

    std::shared_ptr<tpm::SubGraph> graph, bestGraph;
    graph = std::make_shared<tpm::SubGraph>(g.getOperators());
    tpm::SearchEngine searchEngine;
    searchEngine.run(graph, bestGraph);
    tpm::CodeEngine codeEngine;
    auto perfEngine = searchEngine.exportPerfEngine();
    codeEngine.importPerfEngine(perfEngine);
    codeEngine.genCode(bestGraph, "res.cu");

    return 0;
}
