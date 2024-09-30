# Steps to generate coverage report

TIME=$1

if [ -z "$TIME" ]; then
    TIME=1
fi

INPUT_FILE="sanitize_cputopology.c"
EXECUTABLE_NAME="executable-name"

clang-18 -fsanitize=fuzzer -fprofile-instr-generate -fcoverage-mapping -mllvm -runtime-counter-relocation "$INPUT_FILE" -o "$EXECUTABLE_NAME"

LLVM_PROFILE_FILE="${EXECUTABLE_NAME}.profraw" ./"$EXECUTABLE_NAME" values_corpus -max_total_time=$TIME

llvm-profdata merge -sparse "${EXECUTABLE_NAME}.profraw" -o "${EXECUTABLE_NAME}.profdata"

llvm-cov report ./"$EXECUTABLE_NAME" -instr-profile="${EXECUTABLE_NAME}.profdata"
