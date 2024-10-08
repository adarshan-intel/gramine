#include "api.h"
#include "pal.h"
#include "pal_error.h"
#include "pal_regression.h"

int main(int argc, char** argv, char** envp) {
    uint64_t time1 = 0;
    if (PalSystemTimeQuery(&time1) < 0) {
        pal_printf("PalSystemTimeQuery failed\n");
        return 1;
    }
    uint64_t time2 = 0;
    if (PalSystemTimeQuery(&time2) < 0) {
        pal_printf("PalSystemTimeQuery failed\n");
        return 1;
    }

    pal_printf("Time Query 1: %ld\n", time1);
    pal_printf("Time Query 2: %ld\n", time2);

    if (time1 <= time2)
        pal_printf("Query System Time OK\n");

    PAL_HANDLE sleep_handle = NULL;
    if (PalEventCreate(&sleep_handle, /*init_signaled=*/false, /*auto_clear=*/false) < 0) {
        pal_printf("PalEventCreate failed\n");
        return 1;
    }

    uint64_t time3 = 0;
    if (PalSystemTimeQuery(&time3) < 0) {
        pal_printf("PalSystemTimeQuery failed\n");
        return 1;
    }

    uint64_t timeout = 10000;
    int ret = PalEventWait(sleep_handle, &timeout);
    if (ret != PAL_ERROR_TRYAGAIN) {
        pal_printf("PalEventWait failed\n");
        return 1;
    }

    uint64_t time4 = 0;
    if (PalSystemTimeQuery(&time4) < 0) {
        pal_printf("PalSystemTimeQuery failed\n");
        return 1;
    }

    pal_printf("Sleeped %ld Microseconds\n", time4 - time3);

    if (time3 < time4 && time4 - time3 > 10000)
        pal_printf("Delay Execution for 10000 Microseconds OK\n");

    uint64_t time5 = 0;
    if (PalSystemTimeQuery(&time5) < 0) {
        pal_printf("PalSystemTimeQuery failed\n");
        return 1;
    }

    timeout = 3000000;
    ret = PalEventWait(sleep_handle, &timeout);
    if (ret != PAL_ERROR_TRYAGAIN) {
        pal_printf("PalEventWait failed\n");
        return 1;
    }

    uint64_t time6 = 0;
    if (PalSystemTimeQuery(&time6) < 0) {
        pal_printf("PalSystemTimeQuery failed\n");
        return 1;
    }

    pal_printf("Sleeped %ld Microseconds\n", time6 - time5);

    if (time5 < time6 && time6 - time5 > 3000000)
        pal_printf("Delay Execution for 3 Seconds OK\n");

    unsigned long data[100];
    memset(data, 0, sizeof(data));

    for (int i = 0; i < 100; i++) {
        ret = PalRandomBitsRead(&data[i], sizeof(unsigned long));
        if (ret < 0) {
            pal_printf("PalRandomBitsRead() failed!\n");
            return 1;
        }
    }

    bool same = false;
    for (int i = 1; i < 100; i++)
        for (int j = 0; j < i; j++)
            if (data[i] == data[j])
                same = true;

    if (!same)
        pal_printf("Generate Random Bits OK\n");

    return 0;
}
