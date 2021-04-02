if [ ! -n "$PET_HOME" ]; then  
    echo "PET_HOME is not set"  
    exit
fi    

PET_BUILD=${PET_HOME}/build/
PET_MODEL=${PET_HOME}/models-ae/

for PET_MUTATION_SIZE in $(seq 1 5); do
    for PET_MUTATION_DEPTH in $(seq 2 4); do
        echo $PET_MUTATION_SIZE $PET_MUTATION_DEPTH
        PET_MUTATION_SIZE=$PET_MUTATION_SIZE PET_MUTATION_DEPTH=$PET_MUTATION_DEPTH ${PET_BUILD}/onnx ${PET_MODEL}/resnet18_bs1.onnx | grep -i "best perf"
    done
done