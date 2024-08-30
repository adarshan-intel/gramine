# Steps to generate coverage report

TIME=$1

if [ -z "$TIME" ]; then
    TIME=1
fi

# clang-18 -fsanitize=fuzzer -fprofile-instr-generate -fcoverage-mapping -mllvm -runtime-counter-relocation sanitize_cputopology.c -o executable-name
# LLVM_PROFILE_FILE="executable-name.profraw" ./executable-name values_corpus -max_total_time=$TIME
# llvm-profdata merge -sparse executable-name.profraw -o executable-name.profdata
# llvm-cov report ./executable-name -instr-profile=executable-name.profdata


clang-18 -fsanitize=fuzzer -fprofile-instr-generate -fcoverage-mapping -mllvm -runtime-counter-relocation out.c -o executable-name
LLVM_PROFILE_FILE="executable-name.profraw" ./executable-name values_corpus -max_total_time=$TIME
llvm-profdata merge -sparse executable-name.profraw -o executable-name.profdata
llvm-cov report ./executable-name -instr-profile=executable-name.profdata
