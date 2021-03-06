#include "code_engine.h"
#include "ffi.h"
#include "perf_engine.h"
#include "transpose.h"
#include <fstream>
#include <sys/stat.h>
#include <unordered_set>

namespace tpm {

std::string CodeEngine::actToStr(Operator::ActType act) {
    switch (act) {
    case Operator::None:
        return "CUDNN_ACTIVATION_IDENTITY";
    case Operator::Relu:
        return "CUDNN_ACTIVATION_RELU";
    case Operator::Sigmoid:
        return "CUDNN_ACTIVATION_SIGMOID";
    default:
        assert(false);
    }
}

std::pair<std::string, std::string> CodeEngine::genTranspose(
    const std::vector<const TransposeOp *> ops, const std::string &funcName,
    const std::string &inputName, const std::string &outputName) {
    std::vector<std::shared_ptr<TransBasic>> microOps;
    assert(!ops.empty());
    for (const TransposeOp *op : ops)
        for (auto &&item : op->getTTParam())
            microOps.emplace_back(item);

    auto &&dimA = getDim(*ops.front()->getInputs()[0]);
    auto &&dimO = getDim(*ops.back()->getOutputs()[0]);

    std::vector<TransBasic *> microOps_;
    microOps_.reserve(microOps.size());
    for (auto &&item : microOps)
        microOps_.emplace_back(item.get());
    auto lambda = TransposeEngine::getInstance().getLambda(microOps_, dimA);

    auto size = lambda.size();
    if (lambda.substr(size - 11, 11) == "I[0][a,c,b]") {
        assert(dimA.size() == 3);
        std::cout << "Transpose 3d: a,c,b hit!" << std::endl;
        std::string invoke_code = "";
        invoke_code += "{\n";
        invoke_code += "    dim3 gridSize(80, 1);\n";
        invoke_code += "    dim3 blockSize(32 * 4, 1);\n";
        invoke_code += "    kernel_transpose_3d<<<gridSize, blockSize>>>(" + inputName + ", ";
        invoke_code += outputName + ", ";
        invoke_code += std::to_string(dimA[0]) + ", ";
        invoke_code += std::to_string(dimA[1]) + ", ";
        invoke_code += std::to_string(dimA[2]) + ");\n";
        invoke_code += "    cudaCheckError();\n";
        invoke_code += "}\n";
        return std::make_pair("", invoke_code);
    }
    if (lambda.substr(size - 13, 13) == "I[0][a,d,b,c]") {
        assert(dimA.size() == 4);
        std::cout << "Transpose 4d: a,d,b,c hit!" << std::endl;
        std::string invoke_code = "";
        invoke_code += "{\n";
        invoke_code += "    dim3 gridSize(80, 1);\n";
        invoke_code += "    dim3 blockSize(32 * 4, 1);\n";
        invoke_code += "    kernel_transpose_3d<<<gridSize, blockSize>>>(" + inputName + ", ";
        invoke_code += outputName + ", ";
        invoke_code += std::to_string(dimA[0]) + ", ";
        invoke_code += std::to_string(dimA[1] * dimA[2]) + ", ";
        invoke_code += std::to_string(dimA[3]) + ");\n";
        invoke_code += "    cudaCheckError();\n";
        invoke_code += "}\n";
        return std::make_pair("", invoke_code);
    }

    auto ret = getTVMCode({dimA}, {"float32"}, dimO, lambda, funcName,
                          {inputName}, outputName);
    return ret;
}

inline bool CodeEngine::check_existed(const std::string &filename) {
    struct stat buf;
    return (stat(filename.c_str(), &buf) == 0);
}

std::string CodeEngine::genCode(std::shared_ptr<SubGraph> &graph) {
    clear();
    genHeader();
    std::string line;
    line = "int main() {";
    emit(line);
    shiftTab(1);
    emit("cudnnHandle_t cudnn;");
    emit("cublasHandle_t cublas;");
    emit("checkCudnnError(cudnnCreate(&cudnn));");
    emit("checkCublasError(cublasCreate(&cublas));");

    emit("size_t wsSize = 7ll << 30;");
    emit("float *wsData;");
    emit("checkCudaError(cudaMalloc((void **) &wsData, wsSize));");

    emit("curandGenerator_t gen;");
    emit("checkCurandError(curandCreateGenerator(&gen, "
         "CURAND_RNG_PSEUDO_DEFAULT));");
    emit("checkCurandError(curandSetPseudoRandomGeneratorSeed(gen, (unsigned "
         "long long)clock()));");

    auto tensors = graph->getTensors();
    for (auto t : tensors) {
        genTensorAlloc(*t);
    }
    // FIXME: Hack for bias
    for (auto &&op : graph->getOperators()) {
        if (op->getType() == Operator::Conv) {
            Tensor *t = ((ConvOp *)op)->getBias();
            if (t != nullptr) {
                genTensorAlloc(*t, true);
            }
        }
        if (op->getType() == Operator::Matmul) {
            Tensor *t = ((MatmulOp *)op)->getBias();
            if (t != nullptr) {
                genTensorAlloc(*t);
            }
        }
    }

    // Optimization for Split and Concat
    for (auto &&op : graph->getOperators()) {
        if (op->getType() == Operator::Split &&
            ((SplitOp *)op)->getDim() == 0) {
            int offset = 0;
            auto &&in = op->getInputs()[0];
            for (auto &&out : op->getOutputs()) {
                emit(getVarName(*out) + " = " + getVarName(*in) + " + " +
                     std::to_string(offset) + ";");
                offset += getTensorNElem(*out);
            }
        }
        if (op->getType() == Operator::Concat &&
            ((ConcatOp *)op)->getDim() == 0) {
            int offset = 0;
            auto &&out = op->getOutput();
            for (auto &&in : op->getInputs()) {
                emit(getVarName(*in) += " = " + getVarName(*out) + " + " +
                                        std::to_string(offset) + ";");
                offset += getTensorNElem(*in);
            }
        }
    }

    // reversed DFS post-order is topo-order
    std::unordered_set<const Operator *> flag;
    std::vector<Operator *> opsRev;
    std::function<void(Operator *)> dfs = [&](Operator *op) {
        if (flag.count(op)) {
            return;
        }
        flag.insert(op);
        for (auto &&next : op->getSuccessors()) {
            dfs(next);
        }
        opsRev.emplace_back(op);
    };
    for (auto &&op : graph->getOperators()) {
        dfs(op);
    }

    emit("");
    emit("// Desc");
    for (auto it = opsRev.rbegin(); it != opsRev.rend(); it++) {
        genDesc(**it);
    }

    emit("cudaEvent_t st, ed;");
    emit("float duration;");
    emit("checkCudaError(cudaEventCreate(&st));");
    emit("checkCudaError(cudaEventCreate(&ed));");

    emit("");
    emit("// Compute");
    emit("int warmup = 100, rounds = 1000;");
    emit("for (int i = 0; i < warmup + rounds; i++) {");
    shiftTab(1);
    emit("if (i == warmup) {");
    shiftTab(1);
    emit("checkCudaError(cudaEventRecord(st, 0));");
    shiftTab(-1);
    emit("}");
    for (auto it = opsRev.rbegin(); it != opsRev.rend(); it++) {
        genCompute(**it);
    }
    shiftTab(-1);
    emit("}");
    assert(transposeMap.empty());

    emit("checkCudaError(cudaEventRecord(ed, 0));");
    emit("checkCudaError(cudaEventSynchronize(st));");
    emit("checkCudaError(cudaEventSynchronize(ed));");
    emit("checkCudaError(cudaEventElapsedTime(&duration, st, ed));");
    emit("std::cout << \" Time(ms) : \" << duration / rounds << std::endl;");

    for (auto t : tensors) {
        genTensorFree(*t);
    }
    // FIXME: Hack for bias
    for (auto &&op : graph->getOperators()) {
        if (op->getType() == Operator::Conv) {
            Tensor *t = ((ConvOp *)op)->getBias();
            if (t != nullptr)
                genTensorFree(*t);
        }
        if (op->getType() == Operator::Matmul) {
            Tensor *t = ((MatmulOp *)op)->getBias();
            if (t != nullptr)
                genTensorFree(*t);
        }
    }

    // TODO: Destroy all the descriptors

    shiftTab(-1);
    line = "}";
    emit(line);

    return render();
}

int CodeEngine::genCode(std::shared_ptr<SubGraph> &graph,
                        const std::string &filename) {
    if (check_existed(filename)) {
        std::cout << "File " << filename << " existed." << std::endl;
        // return 1;
    }

    std::string code = genCode(graph);
    std::ofstream fout(filename);
    fout << code;
    return 0;
}

int CodeEngine::clear() {
    head = "";
    main = "";
    transposeMap.clear();
    return 0;
}

int CodeEngine::shiftTab(int n) {
    if (tabs + n < 0) {
        std::cout << "invalid tab shift." << std::endl;
        return 1;
    }
    tabs += n;
    return 0;
}

int CodeEngine::emit(std::string line) {
    std::string tmp = "";
    for (int i = 0; i < tabs; i++) {
        tmp += "\t";
    }
    tmp += line + "\n";
    main += tmp;
    return 0;
}

std::string CodeEngine::render() {
    std::string code("");
    code += head;
    code += "\n";
    code += main;
    return code;
}

std::string CodeEngine::getVarName(const Tensor &t) {
    return "var_" + std::to_string(t.getHash());
}

std::string CodeEngine::getTensorDescName(const Tensor &t) {
    return "desc_tensor_" + std::to_string(t.getHash());
}

std::string CodeEngine::getFilterDescName(const Tensor &t) {
    return "desc_filter_" + std::to_string(t.getHash());
}

std::string CodeEngine::getDescName(const Operator &op) {
    return "desc_op_" + std::to_string(op.getGuid());
}

void CodeEngine::genHeader() {
    head += "#include <cudnn.h>\n";
    head += "#include <cublas_v2.h>\n";
    head += "#include <curand.h>\n";
    head += "#include <ctime>\n";
    head += "#include <cstdio>\n";
    head += "#include <iostream>\n";
    head += "#include <cub/cub.cuh>\n";
    head += "\n";
    head +=
        "inline const char *cublasGetErrorString(cublasStatus_t error) { \\\n";
    head += "    switch (error) { \\\n";
    head += "    case CUBLAS_STATUS_SUCCESS: \\\n";
    head += "        return \" CUBLAS_STATUS_SUCCESS \"; \\\n";
    head += "    case CUBLAS_STATUS_NOT_INITIALIZED: \\\n";
    head += "        return \" CUBLAS_STATUS_NOT_INITIALIZED \"; \\\n";
    head += "    case CUBLAS_STATUS_ALLOC_FAILED: \\\n";
    head += "        return \" CUBLAS_STATUS_ALLOC_FAILED \"; \\\n";
    head += "    case CUBLAS_STATUS_INVALID_VALUE: \\\n";
    head += "        return \" CUBLAS_STATUS_INVALID_VALUE \"; \\\n";
    head += "    case CUBLAS_STATUS_ARCH_MISMATCH: \\\n";
    head += "        return \" CUBLAS_STATUS_ARCH_MISMATCH \"; \\\n";
    head += "    case CUBLAS_STATUS_MAPPING_ERROR: \\\n";
    head += "        return \" CUBLAS_STATUS_MAPPING_ERROR \"; \\\n";
    head += "    case CUBLAS_STATUS_EXECUTION_FAILED: \\\n";
    head += "        return \" CUBLAS_STATUS_EXECUTION_FAILED \"; \\\n";
    head += "    case CUBLAS_STATUS_INTERNAL_ERROR: \\\n";
    head += "        return \" CUBLAS_STATUS_INTERNAL_ERROR \"; \\\n";
    head += "    case CUBLAS_STATUS_NOT_SUPPORTED: \\\n";
    head += "        return \" CUBLAS_STATUS_NOT_SUPPORTED \"; \\\n";
    head += "    case CUBLAS_STATUS_LICENSE_ERROR: \\\n";
    head += "        return \" CUBLAS_STATUS_LICENSE_ERROR \"; \\\n";
    head += "    } \\\n";
    head += "    return \" < unknown > \"; \\\n";
    head += "}\n";
    head += "\n";
    head +=
        "inline const char *curandGetErrorString(curandStatus_t error) { \\\n";
    head += "    switch (error) { \\\n";
    head += "    case CURAND_STATUS_SUCCESS: \\\n";
    head += "        return \" CURAND_STATUS_SUCCESS \"; \\\n";
    head += "    case CURAND_STATUS_VERSION_MISMATCH: \\\n";
    head += "        return \" CURAND_STATUS_VERSION_MISMATCH \"; \\\n";
    head += "    case CURAND_STATUS_NOT_INITIALIZED: \\\n";
    head += "        return \" CURAND_STATUS_NOT_INITIALIZED \"; \\\n";
    head += "    case CURAND_STATUS_ALLOCATION_FAILED: \\\n";
    head += "        return \" CURAND_STATUS_ALLOCATION_FAILED \"; \\\n";
    head += "    case CURAND_STATUS_TYPE_ERROR: \\\n";
    head += "        return \" CURAND_STATUS_TYPE_ERROR \"; \\\n";
    head += "    case CURAND_STATUS_OUT_OF_RANGE: \\\n";
    head += "        return \" CURAND_STATUS_OUT_OF_RANGE \"; \\\n";
    head += "    case CURAND_STATUS_LENGTH_NOT_MULTIPLE: \\\n";
    head += "        return \" CURAND_STATUS_LENGTH_NOT_MULTIPLE \"; \\\n";
    head += "    case CURAND_STATUS_DOUBLE_PRECISION_REQUIRED: \\\n";
    head +=
        "        return \" CURAND_STATUS_DOUBLE_PRECISION_REQUIRED \"; \\\n";
    head += "    case CURAND_STATUS_LAUNCH_FAILURE: \\\n";
    head += "        return \" CURAND_STATUS_LAUNCH_FAILURE \"; \\\n";
    head += "    case CURAND_STATUS_PREEXISTING_FAILURE: \\\n";
    head += "        return \" CURAND_STATUS_PREEXISTING_FAILURE \"; \\\n";
    head += "    case CURAND_STATUS_INITIALIZATION_FAILED: \\\n";
    head += "        return \" CURAND_STATUS_INITIALIZATION_FAILED \"; \\\n";
    head += "    case CURAND_STATUS_ARCH_MISMATCH: \\\n";
    head += "        return \" CURAND_STATUS_ARCH_MISMATCH \"; \\\n";
    head += "    case CURAND_STATUS_INTERNAL_ERROR: \\\n";
    head += "        return \" CURAND_STATUS_INTERNAL_ERROR \"; \\\n";
    head += "    } \\\n";
    head += "    return \" < unknown > \"; \\\n";
    head += "}\n";
    head += "\n";
    head += "#define checkCudaError(call) \\\n";
    head += "{ \\\n";
    head += "    auto err = call; \\\n";
    head += "    if (cudaSuccess != err) { \\\n";
    head += "        fprintf(stderr, \"Cuda error in file '%s' in line %i : "
            "%s.\\n\", \\\n";
    head +=
        "                __FILE__, __LINE__, cudaGetErrorString(err)); \\\n";
    head += "        exit(EXIT_FAILURE); \\\n";
    head += "    } \\\n";
    head += "}\n";
    head += "\n";
    head += "#define checkCudnnError(call) \\\n";
    head += "{ \\\n";
    head += "    auto err = call; \\\n";
    head += "    if (CUDNN_STATUS_SUCCESS != err) { \\\n";
    head += "        fprintf(stderr, \"Cuda error in file '%s' in line %i : "
            "%s.\\n\", \\\n";
    head += "        __FILE__, __LINE__, cudnnGetErrorString(err)); \\\n";
    head += "        exit(EXIT_FAILURE); \\\n";
    head += "    } \\\n";
    head += "}\n";
    head += "#define checkCublasError(call) \\\n";
    head += "{ \\\n";
    head += "   auto err = call; \\\n";
    head += "   if (CUBLAS_STATUS_SUCCESS != err) { \\\n";
    head += "       fprintf(stderr, \"Cuda error in file '%s' in line %i : "
            "%s.\\n\", \\\n";
    head += "                    __FILE__, __LINE__, "
            "cublasGetErrorString(err)); \\\n";
    head += "       exit(EXIT_FAILURE); \\\n";
    head += "   } \\\n";
    head += "}\n";
    head += "#define checkCurandError(call) \\\n";
    head += "{ \\\n";
    head += "    auto err = call; \\\n";
    head += "    if (CURAND_STATUS_SUCCESS != err) { \\\n";
    head += "        fprintf(stderr, \"Cuda error in file '%s' in line %i : "
            "%s.\\n\", \\\n";
    head +=
        "                __FILE__, __LINE__, curandGetErrorString(err)); \\\n";
    head += "        exit(EXIT_FAILURE); \\\n";
    head += "    } \\\n";
    head += "}\n";

    head += "\n/* online_softmax: cub is required */\n";
    head += "struct __align__(8) MD\n";
    head += "{\n";
    head += "    float m;\n";
    head += "    float d;\n";
    head += "};\n";
    head += "\n";
    head += "__device__ __forceinline__ MD reduce_md_op(MD a, MD b)\n";
    head += "{\n";
    head += "    bool a_bigger = (a.m > b.m);\n";
    head += "    MD bigger_m = a_bigger ? a : b;\n";
    head += "    MD smaller_m = a_bigger ? b : a;\n";
    head += "    MD res;\n";
    head += "    res.d = bigger_m.d + smaller_m.d * __expf(smaller_m.m - bigger_m.m);\n";
    head += "    res.m = bigger_m.m;\n";
    head += "    return res;\n";
    head += "}\n";
    head += "\n";
    head += "template<int THREADBLOCK_SIZE>\n";
    head += "__launch_bounds__(THREADBLOCK_SIZE)\n";
    head += "__global__ void online_softmax(\n";
    head += "    const float * __restrict x,\n";
    head += "    float * __restrict y,\n";
    head += "    int V)\n";
    head += "{\n";
    head += "    int thread_id = threadIdx.x;\n";
    head += "    int vector_id = blockIdx.x;\n";
    head += "\n";
    head += "    // reposition x and y to data for the current vector\n";
    head += "    x += vector_id * V;\n";
    head += "    y += vector_id * V;\n";
    head += "\n";
    head += "    typedef cub::BlockReduce<MD, THREADBLOCK_SIZE> BlockReduce;\n";
    head += "\n";
    head += "    __shared__ typename BlockReduce::TempStorage temp_storage;\n";
    head += "    __shared__ MD md_total;\n";
    head += "\n";
    head += "    MD md_partial;\n";
    head += "    md_partial.m = -FLT_MAX;\n";
    head += "    md_partial.d = 0.0F;\n";
    head += "    for(int elem_id = thread_id; elem_id < V; elem_id += THREADBLOCK_SIZE)\n";
    head += "    {\n";
    head += "        MD new_elem;\n";
    head += "        new_elem.m = x[elem_id];\n";
    head += "        new_elem.d = 1.0F;\n";
    head += "        md_partial = reduce_md_op(md_partial, new_elem);\n";
    head += "    }\n";
    head += "\n";
    head += "    MD md = BlockReduce(temp_storage).Reduce(md_partial, reduce_md_op);\n";
    head += "    if (thread_id == 0)\n";
    head += "        md_total = md;\n";
    head += "    __syncthreads();\n";
    head += "\n";
    head += "    float d_total_inverse = __fdividef(1.0F, md_total.d);\n";
    head += "    for(int elem_id = thread_id; elem_id < V; elem_id += THREADBLOCK_SIZE)\n";
    head += "        y[elem_id] = __expf(x[elem_id] - md_total.m) * d_total_inverse;\n";
    head += "}\n";
    head += "/* online_softmax: cub is required */\n\n";
    head += "#define cudaCheckError() __cudaCheckError(__FILE__, __LINE__)\n";
    head += "void __cudaCheckError(const char *file, const int line) {\n";
    head += "    cudaError err = cudaGetLastError();\n";
    head += "    if (cudaSuccess != err) {\n";
    head += "        std::cout << \"[ERROR] \" << file << \"::\" << line\n";
    head += "                  << \": cudaCheckError() failed. \" << cudaGetErrorString(err)\n";
    head += "                  << std::endl;\n";
    head += "        exit(-1);\n";
    head += "    }\n";
    head += "    return;\n";
    head += "}\n\n";
    head += "__global__ void kernel_transpose_3d(float *dst, float *src, const int b, const int m, const int n) {\n";
    head += "    float buf[32];\n";
    head += "    int warp_id = threadIdx.x / 32;\n";
    head += "    int lane_id = threadIdx.x % 32;\n";
    head += "    int nm = m / 32;\n";
    head += "    int nn = n / 32;\n";
    head += "    int nt = b * nm * nn;\n";
    head += "    int base = blockIdx.x * 4 + warp_id;\n";
    head += "    int step = gridDim.x * 4;\n";
    head += "\n";
    head += "    for (int idx = base; idx < nt; idx += step) {\n";
    head += "        int ib = idx;\n";
    head += "        int in = ib % nn;\n";
    head += "        ib /= nn;\n";
    head += "        int im = ib % nm;\n";
    head += "        ib /= nm;\n";
    head += "        int offset_src = ib * m * n + im * 32 * n + in * 32;\n";
    head += "        int offset_dst = ib * m * n + in * 32 * m + im * 32;\n";
    head += "#pragma unroll\n";
    head += "        for (int i = 0; i < 32; i++) {\n";
    head += "            int j = (i + lane_id) % 32;\n";
    head += "            buf[i] = src[offset_src + i * n + j];\n";
    head += "        }\n";
    head += "#pragma unroll\n";
    head += "        for (int j = 0; j < 32; j++) {\n";
    head += "            int i = (j + 32 - lane_id) % 32;\n";
    head += "            dst[offset_dst + j * m + i] = buf[i];\n";
    head += "        }\n";
    head += "    }\n";
    head += "    return;\n";
    head += "}\n";
}

Dim CodeEngine::getDim(const Tensor &t) {
    Dim dim = t.getDims();
    assert(perfEngine != nullptr);
    if (perfEngine->withPenalty()) {
        Dim &&penalty = t.getPenalty();
        assert(penalty.size() == dim.size());
        for (size_t i = 0, iEnd = dim.size(); i < iEnd; i++) {
            assert(penalty[i] >= 0);
            dim[i] += penalty[i];
        }
    }
    return dim;
}

size_t CodeEngine::getTensorNElem(const Tensor &t) {
    size_t size = 1;
    for (auto dim : getDim(t)) {
        size *= dim;
    }
    return size;
}

size_t CodeEngine::getTensorSize(const Tensor &t) {
    assert(t.getDType() == Tensor::Float32 || t.getDType() == Tensor::Int32);
    return 4 * getTensorNElem(t);
}

void CodeEngine::genTensorAlloc(const Tensor &t, bool isConvBias) {
    std::string line;
    std::string var_name = getVarName(t);
    Dim dims = getDim(t);
    int size = getTensorSize(t);

    if (isConvBias) {
        assert(dims.size() == 1);
        dims = {1, dims[0], 1, 1};
    }

    line = "// " + var_name + " ( Dim: ";
    for (auto dim : dims) {
        line += std::to_string(dim) + " ";
    }
    line += ")";
    emit(line);
    switch (t.getDType()) {
    case Tensor::Float32:
        emit("float *" + var_name + " = 0;");
        emit("checkCudaError(cudaMalloc((void **) &" + var_name + ", " +
             std::to_string(size) + "));");
        emit("checkCurandError(curandGenerateUniform(gen, " + var_name + ", " +
             std::to_string(getTensorNElem(t)) + "));");
        break;
    case Tensor::Int32:
        emit("int32_t *" + var_name + " = 0;");
        emit("checkCudaError(cudaMalloc((void **) &" + var_name + ", " +
             std::to_string(size) + "));");
        // Integers are used as indices, so no random
        emit("checkCudaError(cudaMemset(" + var_name + ", 0, " +
             std::to_string(size) + "));");
        break;
    default:
        assert(false);
    }

    std::vector<int> paddedDim = dims;
    while (paddedDim.size() < 4) {
        paddedDim.insert(paddedDim.begin(), 1);
    }
    assert(paddedDim.size() == 4);

    emit("cudnnTensorDescriptor_t " + getTensorDescName(t) + ";");
    emit("checkCudnnError(cudnnCreateTensorDescriptor(&" +
         getTensorDescName(t) + "));");
    if (t.getType() == Tensor::Weight) {
        emit("cudnnFilterDescriptor_t " + getFilterDescName(t) + ";");
        emit("checkCudnnError(cudnnCreateFilterDescriptor(&" +
             getFilterDescName(t) + "));");
    }

    std::string dtype;
    switch (t.getDType()) {
    case Tensor::Float32:
        dtype = "CUDNN_DATA_FLOAT";
        break;
    case Tensor::Int32:
        dtype = "CUDNN_DATA_INT32";
        break;
    default:
        assert(false);
    }

    line = "checkCudnnError(cudnnSetTensor4dDescriptor(";
    line += getTensorDescName(t) + ", ";
    line += "CUDNN_TENSOR_NCHW, " + dtype + ", ";
    line += std::to_string(paddedDim[0]) + ", ";
    line += std::to_string(paddedDim[1]) + ", ";
    line += std::to_string(paddedDim[2]) + ", ";
    line += std::to_string(paddedDim[3]) + "));";
    emit(line);

    if (t.getType() == Tensor::Weight) {
        line = "checkCudnnError(cudnnSetFilter4dDescriptor(";
        line += getFilterDescName(t) + ", ";
        line += dtype + ", CUDNN_TENSOR_NCHW, ";
        line += std::to_string(paddedDim[0]) + ", ";
        line += std::to_string(paddedDim[1]) + ", ";
        line += std::to_string(paddedDim[2]) + ", ";
        line += std::to_string(paddedDim[3]) + "));";
        emit(line);
    }
}

void CodeEngine::genTensorFree(const Tensor &t) {
    emit("checkCudnnError(cudnnDestroyTensorDescriptor(" +
         getTensorDescName(t) + "));");
    if (t.getType() == Tensor::Weight) {
        emit("checkCudnnError(cudnnDestroyFilterDescriptor(" +
             getFilterDescName(t) + "));");
    }
    emit("cudaFree(" + getVarName(t) + ");");
}

void CodeEngine::genDesc(const Operator &op) {
    emit("");
    switch (op.getType()) {
    case Operator::Conv:
        genConvDesc(static_cast<const ConvOp &>(op));
        break;
    case Operator::Matmul:
        genMatmulDesc(static_cast<const MatmulOp &>(op));
        break;
    case Operator::Pad:
        genPadDesc(static_cast<const PadOp &>(op));
        break;
    case Operator::Slice:
        genSliceDesc(static_cast<const SliceOp &>(op));
        break;
    case Operator::Activation:
        genActivationDesc(static_cast<const ActivationOp &>(op));
        break;
    case Operator::AvgPool:
        genAvgPoolDesc(static_cast<const AvgPoolOp &>(op));
        break;
    case Operator::MaxPool:
        genMaxPoolDesc(static_cast<const MaxPoolOp &>(op));
        break;
    case Operator::Add:
        genAddDesc(static_cast<const AddOp &>(op));
        break;
    case Operator::Mul:
        genMulDesc(static_cast<const MulOp &>(op));
        break;
    case Operator::Transpose:
        genTransposeDesc(static_cast<const TransposeOp &>(op));
        break;
    case Operator::Gather:
        genGatherDesc(static_cast<const GatherOp &>(op));
        break;
    case Operator::Split:
        genSplitDesc(static_cast<const SplitOp &>(op));
        break;
    case Operator::Concat:
        genConcatDesc(static_cast<const ConcatOp &>(op));
        break;
    case Operator::Extend:
        genExtendDesc(static_cast<const ExtendOp &>(op));
        break;
    case Operator::Reshape:
        genReshapeDesc(static_cast<const ReshapeOp &>(op));
        break;
    case Operator::Softmax:
        genSoftmaxDesc(static_cast<const SoftmaxOp &>(op));
        break;
    default:
        op.print();
        assert(false);
    }
}

void CodeEngine::genCompute(const Operator &op) {
    emit("");
    switch (op.getType()) {
    case Operator::Conv:
        genConvCompute(static_cast<const ConvOp &>(op));
        break;
    case Operator::Matmul:
        genMatmulCompute(static_cast<const MatmulOp &>(op));
        break;
    case Operator::Pad:
        genPadCompute(static_cast<const PadOp &>(op));
        break;
    case Operator::Slice:
        genSliceCompute(static_cast<const SliceOp &>(op));
        break;
    case Operator::Activation:
        genActivationCompute(static_cast<const ActivationOp &>(op));
        break;
    case Operator::AvgPool:
    case Operator::MaxPool:
        genPoolCompute(op);
        break;
    case Operator::Add:
        genAddCompute(static_cast<const AddOp &>(op));
        break;
    case Operator::Mul:
        genMulCompute(static_cast<const MulOp &>(op));
        break;
    case Operator::Transpose:
        genTransposeCompute(static_cast<const TransposeOp &>(op));
        break;
    case Operator::Gather:
        genGatherCompute(static_cast<const GatherOp &>(op));
        break;
    case Operator::Split:
        genSplitCompute(static_cast<const SplitOp &>(op));
        break;
    case Operator::Concat:
        genConcatCompute(static_cast<const ConcatOp &>(op));
        break;
    case Operator::Extend:
        genExtendCompute(static_cast<const ExtendOp &>(op));
        break;
    case Operator::Reshape:
        genReshapeCompute(static_cast<const ReshapeOp &>(op));
        break;
    case Operator::Softmax:
        genSoftmaxCompute(static_cast<const SoftmaxOp &>(op));
        break;
    default:
        op.print();
        assert(false);
    }
}

void CodeEngine::genConvDesc(const ConvOp &op) {
    emit("cudnnConvolutionDescriptor_t " + getDescName(op) + ";");
    emit("checkCudnnError(cudnnCreateConvolutionDescriptor(&" +
         getDescName(op) + "));");
    std::string line = "";
    line += "checkCudnnError(cudnnSetConvolution2dDescriptor(";
    line += getDescName(op) + ", ";
    line += std::to_string(op.getPh()) + ", ";
    line += std::to_string(op.getPw()) + ", ";
    line += std::to_string(op.getSh()) + ", ";
    line += std::to_string(op.getSw()) + ", ";
    line += std::to_string(op.getDh()) + ", ";
    line += std::to_string(op.getDw()) + ", ";
    line += "CUDNN_CONVOLUTION, CUDNN_DATA_FLOAT));";
    emit(line);
    int arg_g = getDim(*op.getInputs()[0])[1] / getDim(*op.getInputs()[1])[1];
    if (arg_g > 1) {
        emit("checkCudnnError(cudnnSetConvolutionGroupCount(" +
             getDescName(op) + ", " + std::to_string(arg_g) + "));");
    }

    if (op.getAct() != Operator::None) {
        emit("cudnnActivationDescriptor_t " + getDescName(op) + "_act ;");
        emit("checkCudnnError(cudnnCreateActivationDescriptor(&" +
             getDescName(op) + "_act));");
        std::string line = "";
        line += "checkCudnnError(cudnnSetActivationDescriptor(";
        line += getDescName(op) + "_act, ";
        line += actToStr(op.getAct()) + ", ";
        line += "CUDNN_NOT_PROPAGATE_NAN, 0));";
        // NOT_PROPAGATE_NAN is requierd by
        // cudnnConvolotionBiasActivationForward
        emit(line);
    }
}

void CodeEngine::genConvCompute(const ConvOp &op) {
    std::string alpha = "alpha_" + std::to_string(op.getGuid());
    std::string beta = "beta_" + std::to_string(op.getGuid());
    emit("float " + alpha + " = 1.0f, " + beta + " = 0.0f;");

    assert(perfEngine->getOpPerf(Operator::Conv,
                                 op.getArgs(perfEngine->withPenalty())) <
           INFINITY);
    int algo =
        (int)perfEngine->getConvAlgo(op.getArgs(perfEngine->withPenalty()));
    if (op.getAct() == Operator::None && op.getBias() == nullptr) {
        std::string line = "";
        line += "checkCudnnError(cudnnConvolutionForward(cudnn, ";
        line += "&" + alpha + ", ";
        line += getTensorDescName(*op.getInputs()[0]) + ", ";
        line += getVarName(*op.getInputs()[0]) + ", ";
        line += getFilterDescName(*op.getInputs()[1]) + ", ";
        line += getVarName(*op.getInputs()[1]) + ", ";
        line += getDescName(op) + ", ";
        line += "(cudnnConvolutionFwdAlgo_t)" + std::to_string(algo) + ", ";
        line += "wsData, ";
        line += "wsSize, ";
        line += "&" + beta + ", ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + "));";
        emit(line);

    } else if (op.getAct() == Operator::None && op.getBias() != nullptr) {
        // Only the CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM algo is
        // enabled with CUDNN_ACTIVATION_IDENTITY.
        // So, fallback to 2 seperated calls
        std::string line = "";
        line += "checkCudnnError(cudnnConvolutionForward(cudnn, ";
        line += "&" + alpha + ", ";
        line += getTensorDescName(*op.getInputs()[0]) + ", ";
        line += getVarName(*op.getInputs()[0]) + ", ";
        line += getFilterDescName(*op.getInputs()[1]) + ", ";
        line += getVarName(*op.getInputs()[1]) + ", ";
        line += getDescName(op) + ", ";
        line += "(cudnnConvolutionFwdAlgo_t)" + std::to_string(algo) + ", ";
        line += "wsData, ";
        line += "wsSize, ";
        line += "&" + beta + ", ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + "));";
        emit(line);
        emit(beta + " = 1.0f;");
        line = "";
        line += "checkCudnnError(cudnnAddTensor(cudnn, ";
        line += "&" + alpha + ", ";
        line += getTensorDescName(*op.getBias()) + ", ";
        line += getVarName(*op.getBias()) + ", ";
        line += "&" + beta + ", ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + "));";
        emit(line);

    } else if (op.getAct() != Operator::None && op.getBias() == nullptr) {
        std::string line = "";
        line += "checkCudnnError(cudnnConvolutionForward(cudnn, ";
        line += "&" + alpha + ", ";
        line += getTensorDescName(*op.getInputs()[0]) + ", ";
        line += getVarName(*op.getInputs()[0]) + ", ";
        line += getFilterDescName(*op.getInputs()[1]) + ", ";
        line += getVarName(*op.getInputs()[1]) + ", ";
        line += getDescName(op) + ", ";
        line += "(cudnnConvolutionFwdAlgo_t)" + std::to_string(algo) + ", ";
        line += "wsData, ";
        line += "wsSize, ";
        line += "&" + beta + ", ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + "));";
        emit(line);
        emit(beta + " = 1.0f;");
        line = "";
        line += "checkCudnnError(cudnnActivationForward(cudnn, ";
        line += getDescName(op) + "_act, ";
        line += "&" + alpha + ", ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + ", ";
        line += "&" + beta + ", ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + "));";
        emit(line);
    } else if (op.getAct() != Operator::None && op.getBias() != nullptr) {
        std::string line = "";
        line += "checkCudnnError(cudnnConvolutionBiasActivationForward(cudnn, ";
        line += "&" + alpha + ", ";
        line += getTensorDescName(*op.getInputs()[0]) + ", ";
        line += getVarName(*op.getInputs()[0]) + ", ";
        line += getFilterDescName(*op.getInputs()[1]) + ", ";
        line += getVarName(*op.getInputs()[1]) + ", ";
        line += getDescName(op) + ", ";
        line += "(cudnnConvolutionFwdAlgo_t)" + std::to_string(algo) + ", ";
        line += "wsData, ";
        line += "wsSize, ";
        line += "&" + beta + ", ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + ", ";
        line += getTensorDescName(*op.getBias()) + ", ";
        line += getVarName(*op.getBias()) + ", ";
        line += getDescName(op) + "_act, ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + "));";
        emit(line);
    } else {
        assert(false);
    }
}

void CodeEngine::genMatmulDesc(const MatmulOp &op) {
    if (op.getAct() != Operator::None) {
        emit("cudnnActivationDescriptor_t " + getDescName(op) + "_act ;");
        emit("checkCudnnError(cudnnCreateActivationDescriptor(&" +
             getDescName(op) + "_act));");
        std::string line = "";
        line += "checkCudnnError(cudnnSetActivationDescriptor(";
        line += getDescName(op) + "_act, ";
        line += actToStr(op.getAct()) + ", ";
        line += "CUDNN_NOT_PROPAGATE_NAN, 0));";
        emit(line);
    }
}

void CodeEngine::genMatmulCompute(const MatmulOp &op) {
    auto A = op.getInputs()[0], B = op.getInputs()[1];
    auto &&dimA = getDim(*A), &&dimB = getDim(*B);
    auto b = dimA[0];
    auto m = op.getTransA() ? dimA[2] : dimA[1];
    auto n = op.getTransB() ? dimB[1] : dimB[2];
    auto k = op.getTransA() ? dimA[1] : dimA[2];
    const int lda = op.getTransA() ? k : m, ldb = op.getTransB() ? n : k,
              ldc = n;

    std::string alpha = "alpha_" + std::to_string(op.getGuid());
    std::string beta = "beta_" + std::to_string(op.getGuid());
    emit("float " + alpha + " = 1.0f, " + beta + " = 0.0f;");
    std::string line = "";
    line += "cublasGemmStridedBatchedEx(cublas, ";
    line += op.getTransB() ? "CUBLAS_OP_N, " : "CUBLAS_OP_T, "; // opB
    line += op.getTransA() ? "CUBLAS_OP_N, " : "CUBLAS_OP_T, "; // opA
    line += std::to_string(n) + ", ";
    line += std::to_string(m) + ", ";
    line += std::to_string(k) + ", ";
    line += "&" + alpha + ", ";
    line += getVarName(*op.getInputs()[1]) + ", "; // B
    line += "CUDA_R_32F, ";
    line += std::to_string(ldb) + ", ";
    line += std::to_string(k * n) + ", ";
    line += getVarName(*op.getInputs()[0]) + ", "; // A
    line += "CUDA_R_32F, ";
    line += std::to_string(lda) + ", ";
    line += std::to_string(m * k) + ", ";
    line += "&" + beta + ", ";
    line += getVarName(*op.getOutputs()[0]) + ", "; // C
    line += "CUDA_R_32F, ";
    line += std::to_string(ldc) + ", ";
    line += std::to_string(m * n) + ", ";
    line += std::to_string(b) + ", ";
    line += "CUDA_R_32F, ";
    line += "(cublasGemmAlgo_t)" +
            std::to_string((int)perfEngine->getMatmulAlgo(op.getArgs())) + ");";
    emit(line);

    if (op.getAct() != Operator::None) {
        emit(beta + " = 1.0f;");
        line = "";
        line += "checkCudnnError(cudnnActivationForward(cudnn, ";
        line += getDescName(op) + "_act, ";
        line += "&" + alpha + ", ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + ", ";
        line += "&" + beta + ", ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + "));";
        emit(line);
    }

    if (op.getBias() != nullptr) {
        emit(beta + " = 1.0f;");
        line = "";
        line += "checkCudnnError(cudnnAddTensor(cudnn, ";
        line += "&" + alpha + ", ";
        line += getTensorDescName(*op.getBias()) + ", ";
        line += getVarName(*op.getBias()) + ", ";
        line += "&" + beta + ", ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + "));";
        emit(line);
    }
}

void CodeEngine::genPadDesc(const PadOp &op) {
    emit("cudnnTensorTransformDescriptor_t " + getDescName(op) + ";");
    emit("checkCudnnError(cudnnCreateTensorTransformDescriptor(&" +
         getDescName(op) + "));");

    emit("{"); // so the arrays will be locals
    shiftTab(1);

    std::string line = "int padBefore[] = {";
    line += std::to_string(op.getBegin()[0]) + ", ";
    line += std::to_string(op.getBegin()[1]) + ", ";
    line += std::to_string(op.getBegin()[2]) + ", ";
    line += std::to_string(op.getBegin()[3]) + "};";
    emit(line);
    line = "int padAfter[] = {";
    line += std::to_string(op.getEnd()[0]) + ", ";
    line += std::to_string(op.getEnd()[1]) + ", ";
    line += std::to_string(op.getEnd()[2]) + ", ";
    line += std::to_string(op.getEnd()[3]) + "};";
    emit(line);
    line = "";
    line += "checkCudnnError(cudnnSetTensorTransformDescriptor(";
    line += getDescName(op) + ", ";
    line += "4, CUDNN_TENSOR_NCHW, padBefore, padAfter, nullptr, "
            "CUDNN_TRANSFORM_FOLD));";
    emit(line);

    shiftTab(-1);
    emit("}");
}

void CodeEngine::genPadCompute(const PadOp &op) {
    std::string alpha = "alpha_" + std::to_string(op.getGuid());
    std::string beta = "beta_" + std::to_string(op.getGuid());
    emit("float " + alpha + " = 1.0f, " + beta + " = 0.0f;");
    std::string line = "";
    line += "checkCudnnError(cudnnTransformTensorEx(cudnn, ";
    line += getDescName(op) + ", ";
    line += "&" + alpha + ", ";
    line += getTensorDescName(*op.getInputs()[0]) + ", ";
    line += getVarName(*op.getInputs()[0]) + ", ";
    line += "&" + beta + ", ";
    line += getTensorDescName(*op.getOutputs()[0]) + ", ";
    line += getVarName(*op.getOutputs()[0]) + "));";
    emit(line);
}

void CodeEngine::genSliceDesc(const SliceOp &op) {
    // Empty
}

void CodeEngine::genSliceCompute(const SliceOp &op) {
    const Dim &inDim = getDim(*op.getInputs()[0]);
    const Dim &outDim = getDim(*op.getOutput());
    size_t nDim = outDim.size();
    std::string lambda = "lambda ";
    for (size_t i = 0; i < nDim; i++) {
        lambda += std::string(1, 'a' + i) + (i < nDim - 1 ? ", " : "");
    }
    lambda += ": I[0][";
    for (size_t i = 0; i < nDim; i++) {
        lambda += std::string(1, 'a' + i) + " - " +
                  std::to_string(op.getBegin()[i]) + (i < nDim - 1 ? ", " : "");
    }
    lambda += "]";

    std::string func = "slice_" + std::to_string(op.getGuid());
    std::string input = getVarName(*op.getInputs()[0]);
    std::string output = getVarName(*op.getOutputs()[0]);

    auto res =
        getTVMCode({inDim}, {"float32"}, outDim, lambda, func, {input}, output);

    head += "\n" + res.first + "\n";
    main += "\n" + res.second + "\n";
}

void CodeEngine::genActivationDesc(const ActivationOp &op) {
    emit("cudnnActivationDescriptor_t " + getDescName(op) + ";");
    emit("checkCudnnError(cudnnCreateActivationDescriptor(&" + getDescName(op) +
         "));");
    std::string line = "";
    line += "checkCudnnError(cudnnSetActivationDescriptor(";
    line += getDescName(op) + ", ";
    line += actToStr(op.getActType()) + ", ";
    line += "CUDNN_NOT_PROPAGATE_NAN, 0));";
    emit(line);
}

void CodeEngine::genActivationCompute(const ActivationOp &op) {
    std::string alpha = "alpha_" + std::to_string(op.getGuid());
    std::string beta = "beta_" + std::to_string(op.getGuid());
    emit("float " + alpha + " = 1.0f, " + beta + " = 0.0f;");
    std::string line = "";
    line += "checkCudnnError(cudnnActivationForward(cudnn, ";
    line += getDescName(op) + ", ";
    line += "&" + alpha + ", ";
    line += getTensorDescName(*op.getInputs()[0]) + ", ";
    line += getVarName(*op.getInputs()[0]) + ", ";
    line += "&" + beta + ", ";
    line += getTensorDescName(*op.getOutputs()[0]) + ", ";
    line += getVarName(*op.getOutputs()[0]) + "));";
    emit(line);
}

void CodeEngine::genMaxPoolDesc(const MaxPoolOp &op) {
    emit("cudnnPoolingDescriptor_t " + getDescName(op) + ";");
    emit("checkCudnnError(cudnnCreatePoolingDescriptor(&" + getDescName(op) +
         "));");
    std::string line = "";
    line += "checkCudnnError(cudnnSetPooling2dDescriptor(";
    line += getDescName(op) + ", ";
    line += "CUDNN_POOLING_MAX, CUDNN_NOT_PROPAGATE_NAN, ";
    line += std::to_string(op.getKh()) + ", ";
    line += std::to_string(op.getKw()) + ", ";
    line += std::to_string(op.getPh()) + ", ";
    line += std::to_string(op.getPw()) + ", ";
    line += std::to_string(op.getSh()) + ", ";
    line += std::to_string(op.getSw()) + "));";
    assert(op.getDh() == 1);
    assert(op.getDw() == 1);
    emit(line);
}

void CodeEngine::genAvgPoolDesc(const AvgPoolOp &op) {
    emit("cudnnPoolingDescriptor_t " + getDescName(op) + ";");
    emit("checkCudnnError(cudnnCreatePoolingDescriptor(&" + getDescName(op) +
         "));");
    std::string line = "";
    line += "checkCudnnError(cudnnSetPooling2dDescriptor(";
    line += getDescName(op) + ", ";
    line +=
        "CUDNN_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING, "; // be consistent with
                                                         // import_onnx.py
    line += "CUDNN_NOT_PROPAGATE_NAN, ";
    line += std::to_string(op.getKh()) + ", ";
    line += std::to_string(op.getKw()) + ", ";
    line += std::to_string(op.getPh()) + ", ";
    line += std::to_string(op.getPw()) + ", ";
    line += std::to_string(op.getSh()) + ", ";
    line += std::to_string(op.getSw()) + "));";
    emit(line);
}

void CodeEngine::genPoolCompute(const Operator &op) {
    std::string alpha = "alpha_" + std::to_string(op.getGuid());
    std::string beta = "beta_" + std::to_string(op.getGuid());
    emit("float " + alpha + " = 1.0f, " + beta + " = 0.0f;");
    std::string line = "";
    line += "checkCudnnError(cudnnPoolingForward(cudnn, ";
    line += getDescName(op) + ", ";
    line += "&" + alpha + ", ";
    line += getTensorDescName(*op.getInputs()[0]) + ", ";
    line += getVarName(*op.getInputs()[0]) + ", ";
    line += "&" + beta + ", ";
    line += getTensorDescName(*op.getOutputs()[0]) + ", ";
    line += getVarName(*op.getOutputs()[0]) + "));";
    emit(line);
}

void CodeEngine::genAddDesc(const AddOp &op) {
    // Empty
}

void CodeEngine::genAddCompute(const AddOp &op) {
    // TODO inplace operation assignment is not correct
    std::string alpha = "alpha_" + std::to_string(op.getGuid());
    emit("float " + alpha + " = 1.0f;");
    std::string line = "";
    line += "checkCublasError(cublasSaxpy(cublas, ";
    line += std::to_string(op.getInputs()[1]->size()) + ", ";
    line += "&" + alpha + ", ";
    line += getVarName(*op.getInputs()[0]) + ", ";
    line += "1, ";
    line += getVarName(*op.getInputs()[1]) + ", ";
    line += "1));";
    emit(line);
}

#if 0
void CodeEngine::genAddCompute(const AddOp &op) {
    Dim dimO = {(int)getTensorNElem(*op.getOutput())};
    std::vector<Dim> inDims;
    std::vector<std::string> inDTypes, inNames;
    std::string lambda = "lambda a: ";
    for (size_t inId = 0, inNum = op.getInputs().size(); inId < inNum; inId++) {
        auto &&in = op.getInputs()[inId];
        int inSize = getTensorNElem(*in);
        lambda += "I[" + std::to_string(inId) + "][a % " +
                  std::to_string(inSize) + "]" +
                  (inId < inNum - 1 ? " + " : "");
        inDims.emplace_back(Dim{inSize});
        inDTypes.emplace_back("float32");
        inNames.emplace_back(getVarName(*in));
    }
    std::string func = "add_" + std::to_string(op.getGuid());
    std::string output = getVarName(*op.getOutput());

    auto res =
        getTVMCode(inDims, inDTypes, dimO, lambda, func, {inNames}, output);

    head += "\n" + res.first + "\n";
    main += "\n" + res.second + "\n";
}
#endif

void CodeEngine::genMulDesc(const MulOp &op) {
    emit("cudnnOpTensorDescriptor_t " + getDescName(op) + ";");
    emit("checkCudnnError(cudnnCreateOpTensorDescriptor(&" + getDescName(op) +
         "));");
    std::string line = "";
    line += "checkCudnnError(cudnnSetOpTensorDescriptor(";
    line += getDescName(op) + ", ";
    line += "CUDNN_OP_TENSOR_MUL, CUDNN_DATA_FLOAT, CUDNN_NOT_PROPAGATE_NAN));";
    emit(line);
}

void CodeEngine::genMulCompute(const MulOp &op) {
    std::string alpha1 = "alpha1_" + std::to_string(op.getGuid());
    std::string alpha2 = "alpha2_" + std::to_string(op.getGuid());
    std::string beta = "beta_" + std::to_string(op.getGuid());
    emit("float " + alpha1 + " = 1.0f, " + alpha2 + " = 1.0f, " + beta +
         " = 0.0f;");
    std::string line = "";
    line += "checkCudnnError(cudnnOpTensor(cudnn, ";
    line += getDescName(op) + ", ";
    line += "&" + alpha1 + ", ";
    line += getTensorDescName(*op.getInputs()[0]) + ", ";
    line += getVarName(*op.getInputs()[0]) + ", ";
    line += "&" + alpha2 + ", ";
    line += getTensorDescName(*op.getInputs()[1]) + ", ";
    line += getVarName(*op.getInputs()[1]) + ", ";
    line += "&" + beta + ", ";
    line += getTensorDescName(*op.getOutputs()[0]) + ", ";
    line += getVarName(*op.getOutputs()[0]) + "));";
    emit(line);
    for (size_t i = 2, iEnd = op.getInputs().size(); i < iEnd; i++) {
        std::string line = "";
        line += "checkCudnnError(cudnnOpTensor(cudnn, ";
        line += getDescName(op) + ", ";
        line += "&" + alpha1 + ", ";
        line += getTensorDescName(*op.getInputs()[i]) + ", ";
        line += getVarName(*op.getInputs()[i]) + ", ";
        line += "&" + alpha2 + ", ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + ", ";
        line += "&" + beta + ", ";
        line += getTensorDescName(*op.getOutputs()[0]) + ", ";
        line += getVarName(*op.getOutputs()[0]) + "));";
        emit(line);
    }
}

void CodeEngine::genTransposeDesc(const TransposeOp &op) {
    // Empty
}

void CodeEngine::genTransposeCompute(const TransposeOp &op) {
    if (!transposeMap.count(&op)) {
        transposeMap[&op].reset(new std::vector<const TransposeOp *>());
    }
    transposeMap.at(&op)->emplace_back(&op);

    if (op.getSuccessors().size() == 1 &&
        op.getSuccessors()[0]->getPredecessors().size() == 1 &&
        op.getSuccessors()[0]->isTransposeOp()) {
        transposeMap[static_cast<TransposeOp *>(op.getSuccessors()[0])] =
            transposeMap.at(&op);
        transposeMap.erase(&op);
        return;
    }

    const std::vector<const TransposeOp *> &ops = *transposeMap.at(&op);

    std::string func = "transpose_" + std::to_string(op.getGuid());
    std::string input = getVarName(*ops.front()->getInputs()[0]);
    std::string output = getVarName(*ops.back()->getOutputs()[0]);

    auto res = genTranspose(ops, func, input, output);

    std::string comment = "// ";
    for (auto &&op : ops) {
        comment += op->toString() + " -> ";
    }

    head += "\n" + res.first + "\n";
    main += comment + "\n" + res.second + "\n";

    transposeMap.erase(&op);
}

void CodeEngine::genGatherDesc(const GatherOp &op) {
    // Empty
}

void CodeEngine::genGatherCompute(const GatherOp &op) {
    const Dim &dimA = getDim(*op.getInputs()[0]);
    const Dim &dimB = getDim(*op.getInputs()[1]);
    const Dim &dimO = getDim(*op.getOutput());
    std::string lambda = "lambda ";
    for (size_t i = 0, iEnd = dimO.size(); i < iEnd; i++) {
        lambda += std::string(1, 'a' + i) + (i < iEnd - 1 ? ", " : "");
    }
    lambda += ": I[0][";
    int axisCnt = dimB.size();
    for (size_t i = 0, iEnd = dimA.size(); i < iEnd; i++) {
        if ((int)i == op.getAxis()) {
            lambda += "I[1][(";
            for (size_t j = 0, jEnd = dimB.size(); j < jEnd; j++) {
                lambda += std::string(1, 'a' + j) + (j < jEnd - 1 ? ", " : "");
            }
            lambda += ")]";
        } else {
            lambda += std::string(1, 'a' + axisCnt++);
        }
        lambda += (i < iEnd - 1 ? ", " : "");
    }
    lambda += "]";

    std::string func = "gather_" + std::to_string(op.getGuid());
    std::string input0 = getVarName(*op.getInputs()[0]);
    std::string input1 = getVarName(*op.getInputs()[1]);
    std::string output = getVarName(*op.getOutputs()[0]);

    auto res = getTVMCode({dimA, dimB}, {"float32", "int32"}, dimO, lambda,
                          func, {input0, input1}, output);

    head += "\n" + res.first + "\n";
    main += "\n" + res.second + "\n";
}

void CodeEngine::genSplitDesc(const SplitOp &op) {
    // Empty
}

void CodeEngine::genSplitCompute(const SplitOp &op) {
    if (op.getDim() == 0) {
        return;
    }

    int offset = 0;
    const Dim &dimA = getDim(*op.getInputs()[0]);
    const int axis = op.getDim();
    for (size_t outId = 0, outNum = op.getOutputs().size(); outId < outNum;
         outId++) {
        auto &&out = op.getOutputs()[outId];
        const Dim &dimO = getDim(*out);
        std::string lambda = "lambda ";
        for (size_t i = 0, iEnd = dimO.size(); i < iEnd; i++) {
            lambda += std::string(1, 'a' + i) + (i < iEnd - 1 ? ", " : "");
        }
        lambda += ": I[0][";
        for (size_t i = 0, iEnd = dimA.size(); i < iEnd; i++) {
            if ((int)i == axis) {
                lambda += std::string(1, 'a' + i) + " + " +
                          std::to_string(offset) + (i < iEnd - 1 ? ", " : "");
            } else {
                lambda += std::string(1, 'a' + i) + (i < iEnd - 1 ? ", " : "");
            }
        }
        lambda += "]";

        std::string func = "split_" + std::to_string(op.getGuid()) + "_" +
                           std::to_string(outId);
        std::string input = getVarName(*op.getInputs()[0]);
        std::string output = getVarName(*out);

        auto res = getTVMCode({dimA}, {"float32"}, dimO, lambda, func, {input},
                              output);

        head += "\n" + res.first + "\n";
        main += "\n" + res.second + "\n";

        offset += dimO[axis];
    }
}

void CodeEngine::genConcatDesc(const ConcatOp &op) {
    // Empty
}

void CodeEngine::genConcatCompute(const ConcatOp &op) {
    if (op.getDim() == 0) {
        return;
    }

    const Dim &dimO = getDim(*op.getOutput());
    auto axis = op.getDim();
    std::string lambda = "lambda ";
    for (size_t i = 0, iEnd = dimO.size(); i < iEnd; i++) {
        lambda += std::string(1, 'a' + i) + (i < iEnd - 1 ? ", " : "");
    }
    lambda += ": ";
    std::function<std::string(int, int)> f = [&](int inId,
                                                 int offset) -> std::string {
        std::string str;
        auto &&in = op.getInputs()[inId];
        const Dim &dimA = getDim(*in);
        if (inId < (int)op.getInputs().size() - 1) {
            str += "tvm.tir.if_then_else(";
            str += std::string(1, 'a' + axis) + " < " +
                   std::to_string(offset + dimA[axis]) + ", ";
            str += "I[" + std::to_string(inId) + "][";
            for (size_t i = 0, iEnd = dimO.size(); i < iEnd; i++) {
                if ((int)i == axis) {
                    str += std::string(1, 'a' + i) + " - " +
                           std::to_string(offset) + (i < iEnd - 1 ? ", " : "");
                } else {
                    str += std::string(1, 'a' + i) + (i < iEnd - 1 ? ", " : "");
                }
            }
            str += "], ";
            str += f(inId + 1, offset + dimA[axis]);
            str += ")";
        } else {
            str += "I[" + std::to_string(inId) + "][";
            for (size_t i = 0, iEnd = dimO.size(); i < iEnd; i++) {
                if ((int)i == axis) {
                    str += std::string(1, 'a' + i) + " - " +
                           std::to_string(offset) + (i < iEnd - 1 ? ", " : "");
                } else {
                    str += std::string(1, 'a' + i) + (i < iEnd - 1 ? ", " : "");
                }
            }
            str += "]";
        }
        return str;
    };
    lambda += f(0, 0);

    std::vector<Dim> inDims;
    std::vector<std::string> inDTypes, inNames;
    for (auto &&in : op.getInputs()) {
        inDims.emplace_back(getDim(*in));
        inDTypes.emplace_back("float32");
        inNames.emplace_back(getVarName(*in));
    }
    std::string func = "concat_" + std::to_string(op.getGuid());
    std::string output = getVarName(*op.getOutputs()[0]);

    auto res =
        getTVMCode(inDims, inDTypes, dimO, lambda, func, inNames, output);

    head += "\n" + res.first + "\n";
    main += "\n" + res.second + "\n";
}

void CodeEngine::genExtendDesc(const ExtendOp &op) {
    // Empty
}

void CodeEngine::genExtendCompute(const ExtendOp &op) {
    const Dim &dimA = getDim(*op.getInputs()[0]);
    const Dim &dimO = getDim(*op.getOutput());
    auto axis = op.getDim();
    auto nCopy = op.getNum() + 1;
    std::string lambda = "lambda ";
    for (size_t i = 0, iEnd = dimO.size(); i < iEnd; i++) {
        lambda += std::string(1, 'a' + i) + (i < iEnd - 1 ? ", " : "");
    }
    lambda += ": ";
    std::function<std::string(int, int)> f = [&](int inId,
                                                 int offset) -> std::string {
        std::string str;
        if (inId < nCopy - 1) {
            str += "tvm.tir.if_then_else(";
            str += std::string(1, 'a' + axis) + " < " +
                   std::to_string(offset + dimA[axis]) + ", ";
            str += "I[0][";
            for (size_t i = 0, iEnd = dimO.size(); i < iEnd; i++) {
                if ((int)i == axis) {
                    str += std::string(1, 'a' + i) + " - " +
                           std::to_string(offset) + (i < iEnd - 1 ? ", " : "");
                } else {
                    str += std::string(1, 'a' + i) + (i < iEnd - 1 ? ", " : "");
                }
            }
            str += "], ";
            str += f(inId + 1, offset + dimA[axis]);
            str += ")";
        } else {
            str += "I[0][";
            for (size_t i = 0, iEnd = dimO.size(); i < iEnd; i++) {
                if ((int)i == axis) {
                    str += std::string(1, 'a' + i) + " - " +
                           std::to_string(offset) + (i < iEnd - 1 ? ", " : "");
                } else {
                    str += std::string(1, 'a' + i) + (i < iEnd - 1 ? ", " : "");
                }
            }
            str += "]";
        }
        return str;
    };
    lambda += f(0, 0);

    std::string func = "extend_" + std::to_string(op.getGuid());
    std::string input = getVarName(*op.getInputs()[0]);
    std::string output = getVarName(*op.getOutputs()[0]);

    auto res =
        getTVMCode({dimA}, {"float32"}, dimO, lambda, func, {input}, output);

    head += "\n" + res.first + "\n";
    main += "\n" + res.second + "\n";
}

void CodeEngine::genReshapeDesc(const ReshapeOp &op) {
    // Empty
}

void CodeEngine::genReshapeCompute(const ReshapeOp &op) {
    /*std::string line = "checkCudaError(cudaMemcpyAsync(";
    line += getVarName(*op.getOutputs()[0]) + ", ";
    line += getVarName(*op.getInputs()[0]) + ", ";
    line += std::to_string(getTensorSize(*op.getInputs()[0])) + ", ";
    line += "cudaMemcpyDefault));";
    emit(line);*/
    emit(getVarName(*op.getOutput()) + " = " + getVarName(*op.getInputs()[0]) +
         ";");
}

void CodeEngine::genSoftmaxDesc(const SoftmaxOp &op) {
    // Empty
    /*auto dim = getDim(*op.getInputs()[0]);
    assert(op.getAxis() == (int)dim.size() - 1);
    emit("cudnnTensorDescriptor_t " + getDescName(op) + "_in_desc;");
    emit("checkCudnnError(cudnnCreateTensorDescriptor(&" + getDescName(op) +
         "_in_desc));");
    std::string line = "checkCudnnError(cudnnSetTensor4dDescriptor(";
    line += getDescName(op) + "_in_desc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, ";
    line +=
        std::to_string(getTensorNElem(*op.getInputs()[0]) / dim.back()) + ", ";
    line += std::to_string(dim.back()) + ", 1, 1));";
    emit(line);*/
}

void CodeEngine::genSoftmaxCompute(const SoftmaxOp &op) {
    emit("{"); shiftTab(1);
    std::string x = getVarName(*op.getInputs()[0]); // input
    std::string y = getVarName(*op.getOutputs()[0]); // output
    auto _dims = (*op.getInputs()[0]).getDims();
    int batch_size = 1, V;
    for (size_t i = 0, iEnd = _dims.size(); i < iEnd - 1; ++i) 
        batch_size *= _dims[i];
    V = _dims[_dims.size() - 1];
    emit("int batch_size = " + std::to_string(batch_size) + ", V = " + std::to_string(V) + ";");
    emit("int max_threadblock_size = " + std::to_string(V / 8) + ";");
    emit("if (max_threadblock_size >= 256)");
    emit("    online_softmax<256><<<batch_size,256>>>(" + x + ", " + y + ", V);");
    emit("else if (max_threadblock_size >= 128)");
    emit("    online_softmax<128><<<batch_size,128>>>(" + x + ", " + y + ", V);");
    emit("else if (max_threadblock_size >= 64)");
    emit("    online_softmax<64><<<batch_size,64>>>(" + x + ", " + y + ", V);");
    emit("else");
    emit("    online_softmax<32><<<batch_size,32>>>(" + x + ", " + y + ", V);");
    shiftTab(-1); emit("}");
    // This code causes runtime error
    // std::string alpha = "alpha_" + std::to_string(op.getGuid());
    // std::string beta = "beta_" + std::to_string(op.getGuid());
    // emit("float " + alpha + " = 1.0f, " + beta + " = 0.0f;");
    // std::string line = "";
    // line += "checkCudnnError(cudnnSoftmaxForward(cudnn, ";
    // line += "CUDNN_SOFTMAX_FAST, CUDNN_SOFTMAX_MODE_INSTANCE, ";
    // line += "&" + alpha + ", ";
    // line += getDescName(op) + "_in_desc, ";
    // line += getVarName(*op.getInputs()[0]) + ", ";
    // line += "&" + beta + ", ";
    // line += getTensorDescName(*op.getOutputs()[0]) + ", ";
    // line += getVarName(*op.getOutputs()[0]) + "));";
    // emit(line);
}

std::pair<std::string, std::string> CodeEngine::getTVMCode(
    const std::vector<std::vector<int>> &inDims,
    const std::vector<std::string> &inDTypes, const std::vector<int> &outDims,
    const std::string &lambda, const std::string &funcName,
    const std::vector<std::string> &inputNames, const std::string &outputName) {
    std::string funcCode, invokeCode;
    start_interpreter();
    try {
        auto func = py::module::import("cpp_plugin").attr("gen_simple_op");
        py::tuple code = func(inDims, inDTypes, outDims, lambda, funcName,
                              inputNames, outputName);
        funcCode = py::str(code[0]), invokeCode = py::str(code[1]);
    } catch (py::error_already_set &e) {
        if (e.matches(PyExc_ImportError)) {
            std::cerr << "Import Error. Don't forget to set environment "
                         "variable PYTHONPATH to contain "
                         "<repo-root>/python"
                      << std::endl;
        }
        throw;
    }
    return std::make_pair(funcCode, invokeCode);
}

void CodeEngine::importPerfEngine(std::shared_ptr<PerfEngine> perfEngine_) {
    perfEngine = perfEngine_;
}

} // namespace tpm
