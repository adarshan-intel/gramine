# sudo apt install clang-18

# sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100
# sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100
# export CC="clang-18"
# export CXX="clang-18++"
# export AS="clang-18"

TIME=$1
if [ -z "$TIME" ]; then
    TIME=1
fi

# clang-18 -fsanitize=fuzzer sanitize_cputopology.c -o executable-name

# clang-18 -fsanitize=fuzzer,address sanitize_cputopology.c -o executable-name

clang-18 -fsanitize=fuzzer,memory sanitize_cputopology.c -o executable-name

./executable-name values_corpus -max_total_time=$TIME