#!/bin/bash

# Do git clone of gramine and run the script from root directory, output will be in CI-Examples/cpuid_fuzzing/out.c

source_file="./pal/src/host/linux-sgx/pal_main.c"
function_list+=("sanitize_topo_info")
output_file="./CI-Examples/cpu_topology/out.c"

./script.sh "$source_file" "${function_list[@]}" "$output_file"