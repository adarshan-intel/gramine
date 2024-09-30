sudo apt install clang-18

sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100
sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100
export CC="clang-18"
export CXX="clang-18++"
export AS="clang-18"

# The time for which the fuzzer should run
TIME=$1
if [ -z "$TIME" ]; then
    TIME=1
fi

# Run any of the following commands to fuzz the code

# 1. Fuzz the code using libFuzzer
# clang-18 -fsanitize=fuzzer sanitize_cputopology.c -o executable-name

# 2. Fuzz the code using AddressSanitizer
# clang-18 -fsanitize=fuzzer,address sanitize_cputopology.c -o executable-name

# 3. Fuzz the code using MemorySanitizer
# clang-18 -fsanitize=fuzzer,memory sanitize_cputopology.c -o executable-name

./executable-name values_corpus -max_total_time=$TIME