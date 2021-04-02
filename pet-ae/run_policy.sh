if [ ! -n "$PET_HOME" ]; then  
    echo "PET_HOME is not set"  
    exit
fi    

PET_BUILD=${PET_HOME}/build/
PET_MODEL=${PET_HOME}/models-ae/


echo resnet
time ${PET_BUILD}/onnx ${PET_MODEL}/resnet18_bs1.onnx | grep -i "best perf"
echo CSRNet
time ${PET_BUILD}/dilation 1 1  | grep -i "best perf"
echo Bert
time ${PET_BUILD}/onnx ${PET_MODEL}/mybert-new.1.onnx | grep -i "best perf"
echo Inception
time ${PET_BUILD}/onnx ${PET_MODEL}/inception_origin.infered.onnx | grep -i "best perf"






export PET_DISABLE_NO_NEQ_OPT=1

echo resnet
time ${PET_BUILD}/onnx ${PET_MODEL}/resnet18_bs1.onnx | grep -i "best perf"
echo CSRNet
time ${PET_BUILD}/dilation 1 1  | grep -i "best perf"
echo Bert
time ${PET_BUILD}/onnx ${PET_MODEL}/mybert-new.1.onnx | grep -i "best perf"
echo Inception
time ${PET_BUILD}/onnx ${PET_MODEL}/inception_origin.infered.onnx | grep -i "best perf"

unset PET_DISABLE_NO_NEQ_OPT












export PET_DISABLE_EQ_OPT=1

echo resnet
time ${PET_BUILD}/onnx ${PET_MODEL}/resnet18_bs1.onnx | grep -i "best perf"
echo CSRNet
time ${PET_BUILD}/dilation 1 1  | grep -i "best perf"
echo Bert
time ${PET_BUILD}/onnx ${PET_MODEL}/mybert-new.1.onnx | grep -i "best perf"
echo Inception
time ${PET_BUILD}/onnx ${PET_MODEL}/inception_origin.infered.onnx | grep -i "best perf"

unset PET_DISABLE_EQ_OPT














export PET_DISABLE_NEQ_OPT=1
export PET_DISABLE_NO_NEQ_OPT=1

echo resnet
time ${PET_BUILD}/onnx ${PET_MODEL}/resnet18_bs1.onnx | grep -i "best perf"
echo CSRNet
time ${PET_BUILD}/dilation 1 1  | grep -i "best perf"
echo Bert
time ${PET_BUILD}/onnx ${PET_MODEL}/mybert-new.1.onnx | grep -i "best perf"
echo Inception
time ${PET_BUILD}/onnx ${PET_MODEL}/inception_origin.infered.onnx | grep -i "best perf"

unset PET_DISABLE_NEQ_OPT
unset PET_DISABLE_NO_NEQ_OPT
