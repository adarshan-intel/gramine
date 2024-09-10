#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "assert.h"
#include "pal_topology.h"
#include "sanitize_cputopology.h"

#define IS_IN_RANGE_INCL(x, min, max) ((x) >= (min) && (x) <= (max) ? 1 : 0)
#define READ_ONCE(x)                                      \
    ({                                                    \
        __typeof__(x) y = *(volatile __typeof__(x)*)&(x); \
        y;                                                \
    })

#define PAL_ERROR_INVAL 4

#define MAX_CACHES     4
#define MAX_THREADS    16
#define MAX_CORES      16
#define MAX_SOCKETS    16
#define MAX_NUMA_NODES 16

size_t random_size_t(size_t min, size_t max) {
    return rand() % (max - min + 1) + min;
}

static void coerce_untrusted_bool(bool* ptr) {
    static_assert(sizeof(bool) == sizeof(unsigned char), "Unsupported compiler");
    *ptr = !!READ_ONCE(*(unsigned char*)ptr);
}

static int sanitize_topo_info(struct pal_topo_info* topo_info) {
    for (size_t i = 0; i < topo_info->caches_cnt; i++) {
        struct pal_cache_info* cache = &topo_info->caches[i];
        if (cache->type != CACHE_TYPE_DATA && cache->type != CACHE_TYPE_INSTRUCTION &&
            cache->type != CACHE_TYPE_UNIFIED) {
            printf("ERROR: Invalid cache type\n");
            return -PAL_ERROR_INVAL;
        }

        if (!IS_IN_RANGE_INCL(cache->level, 1, 3) || !IS_IN_RANGE_INCL(cache->size, 1, 1 << 30) ||
            !IS_IN_RANGE_INCL(cache->coherency_line_size, 1, 1 << 16) ||
            !IS_IN_RANGE_INCL(cache->number_of_sets, 1, 1 << 30) ||
            !IS_IN_RANGE_INCL(cache->physical_line_partition, 1, 1 << 16)) {
            printf("ERROR: Invalid cache parameters\n");
            return -PAL_ERROR_INVAL;
        }
    }

    if (topo_info->threads_cnt == 0 || !topo_info->threads[0].is_online) {
        printf("ERROR: First thread must be online\n");
        return -PAL_ERROR_INVAL;
    }

    for (size_t i = 0; i < topo_info->threads_cnt; i++) {
        struct pal_cpu_thread_info* thread = &topo_info->threads[i];
        coerce_untrusted_bool(&thread->is_online);
        if (thread->is_online) {
            if (thread->core_id >= topo_info->cores_cnt) {
                printf("ERROR: Invalid core ID\n");
                return -PAL_ERROR_INVAL;
            }
            /* Verify that the cache array has no holes... */
            for (size_t j = 0; j < MAX_CACHES - 1; j++)
                if (thread->ids_of_caches[j] == (size_t)-1 &&
                    thread->ids_of_caches[j + 1] != (size_t)-1) {
                    printf("ERROR: Cache array has holes\n");
                    return -PAL_ERROR_INVAL;
                }
            /* ...and valid indices. */
            for (size_t j = 0; j < MAX_CACHES; j++) {
                if (thread->ids_of_caches[j] != (size_t)-1 &&
                    thread->ids_of_caches[j] >= topo_info->caches_cnt) {
                    printf("ERROR: Invalid cache ID\n");
                    return -PAL_ERROR_INVAL;
                }
            }
        } else {
            thread->core_id = 0;
            for (size_t j = 0; j < MAX_CACHES; j++) thread->ids_of_caches[j] = 0;
        }
    }

    for (size_t i = 0; i < topo_info->cores_cnt; i++) {
        if (topo_info->cores[i].socket_id >= topo_info->sockets_cnt) {
            printf("ERROR: Invalid socket ID\n");
            return -PAL_ERROR_INVAL;
        }
        if (topo_info->cores[i].node_id >= topo_info->numa_nodes_cnt) {
            printf("ERROR: Invalid node ID\n");
            return -PAL_ERROR_INVAL;
        }
    }

    if (!topo_info->numa_nodes[0].is_online) {
        printf("ERROR: First NUMA node must be online\n");
        return -PAL_ERROR_INVAL;
    }

    for (size_t i = 0; i < topo_info->numa_nodes_cnt; i++) {
        struct pal_numa_node_info* node = &topo_info->numa_nodes[i];
        coerce_untrusted_bool(&node->is_online);
        if (node->is_online) {
            for (size_t j = 0; j < HUGEPAGES_MAX; j++) {
                size_t unused;
                if (__builtin_mul_overflow(node->nr_hugepages[j], hugepage_size[j], &unused)) {
                    printf("ERROR: Hugepage size overflow\n");
                    return -PAL_ERROR_INVAL;
                }
            }
        } else {
            /* Not required, just a hardening in case we accidentally accessed offline node's
             * fields. */
            for (size_t j = 0; j < HUGEPAGES_MAX; j++) node->nr_hugepages[j] = 0;
        }
    }

    for (size_t i = 0; i < topo_info->numa_nodes_cnt; i++) {
        /* Note: distance i -> i is 10 according to the ACPI 2.0 SLIT spec, but to accomodate for
         * weird BIOS settings we aren't checking this. */
        for (size_t j = 0; j < topo_info->numa_nodes_cnt; j++) {
            if ((!topo_info->numa_nodes[i].is_online || !topo_info->numa_nodes[j].is_online) &&
                topo_info->numa_distance_matrix[i * topo_info->numa_nodes_cnt + j] != 0) {
                printf("ERROR: Invalid distance matrix\n");
                return -PAL_ERROR_INVAL;
            }
            if (topo_info->numa_distance_matrix[i * topo_info->numa_nodes_cnt + j] !=
                topo_info->numa_distance_matrix[j * topo_info->numa_nodes_cnt + i]) {
                printf("ERROR: Distance matrix is not symmetric\n");
                return -PAL_ERROR_INVAL;
            }
        }
    }

    /* Verify that online threads belong to online NUMA nodes (at this point, all indices are
     * verified to be in-bounds and we can safely use them) */
    for (size_t i = 0; i < topo_info->threads_cnt; i++) {
        struct pal_cpu_thread_info* thread = &topo_info->threads[i];
        if (!thread->is_online)
            continue;
        size_t node_id = topo_info->cores[thread->core_id].node_id;
        if (!topo_info->numa_nodes[node_id].is_online) {
            printf("ERROR: Online thread belongs to offline NUMA node\n");
            return -PAL_ERROR_INVAL;
        }
    }

    return 0;
}

void print_data(struct pal_topo_info* topo_info) {
    printf("Caches: %zu\n", topo_info->caches_cnt);
    for (size_t i = 0; i < topo_info->caches_cnt; i++) {
        printf(
            "Type: %d, Level: %zu, Size: %zu, Coherency Line Size: %zu, Number of Sets: %zu, "
            "Physical Line Partition: %zu\n",
            topo_info->caches[i].type, topo_info->caches[i].level, topo_info->caches[i].size,
            topo_info->caches[i].coherency_line_size, topo_info->caches[i].number_of_sets,
            topo_info->caches[i].physical_line_partition);
    }
    printf("\n");
    printf("Threads: %zu\n", topo_info->threads_cnt);
    for (size_t i = 0; i < topo_info->threads_cnt; i++) {
        printf("Is Online: %d, Core ID: %zu, Cache IDs: ", topo_info->threads[i].is_online,
               topo_info->threads[i].core_id);
        for (size_t j = 0; j < MAX_CACHES; j++) {
            printf("%zu ", topo_info->threads[i].ids_of_caches[j]);
        }
        printf("\n");
    }
    printf("\n");
    printf("Cores: %zu\n", topo_info->cores_cnt);
    for (size_t i = 0; i < topo_info->cores_cnt; i++) {
        printf("Socket ID: %zu, Node ID: %zu\n", topo_info->cores[i].socket_id,
               topo_info->cores[i].node_id);
    }
    printf("\n");
    printf("Sockets: %zu\n", topo_info->sockets_cnt);
    for (size_t i = 0; i < topo_info->sockets_cnt; i++) {
        printf("Unused: %c\n", topo_info->sockets[i].unused);
    }
    printf("\n");
    printf("NUMA Nodes: %zu\n", topo_info->numa_nodes_cnt);
    for (size_t i = 0; i < topo_info->numa_nodes_cnt; i++) {
        printf("Is Online: %d, Hugepages: ", topo_info->numa_nodes[i].is_online);
        for (size_t j = 0; j < HUGEPAGES_MAX; j++) {
            printf("%zu ", topo_info->numa_nodes[i].nr_hugepages[j]);
        }
        printf("\n");
    }
    printf("\n");
    printf("NUMA Distance Matrix:\n");
    for (size_t i = 0; i < topo_info->numa_nodes_cnt; i++) {
        for (size_t j = 0; j < topo_info->numa_nodes_cnt; j++) {
            printf("%zu ", topo_info->numa_distance_matrix[i * topo_info->numa_nodes_cnt + j]);
        }
        printf("\n");
    }
    printf("\n");
}

int LLVMFuzzerTestOneInput(const uint32_t* Data, size_t Size) {
    struct pal_topo_info* topo_info = malloc(sizeof(struct pal_topo_info));
    memset(topo_info, 0, sizeof(struct pal_topo_info));

    printf("-----------------------\n");
    size_t index = 0;

    topo_info->caches_cnt = (Data[index++] % MAX_CACHES) + 1;
    topo_info->caches     = malloc(topo_info->caches_cnt * sizeof(struct pal_cache_info));
    for (size_t i = 0; i < topo_info->caches_cnt; i++) {
        topo_info->caches[i].type                    = Data[index++] % 3;
        topo_info->caches[i].level                   = Data[index++] % 3 + 1;
        topo_info->caches[i].size                    = Data[index++] % (1 << 30) + 1;
        topo_info->caches[i].coherency_line_size     = Data[index++] % (1 << 16) + 1;
        topo_info->caches[i].number_of_sets          = Data[index++] % (1 << 30) + 1;
        topo_info->caches[i].physical_line_partition = Data[index++] % (1 << 16) + 1;
    }

    topo_info->threads_cnt = (Data[index++] % MAX_THREADS) + 1;
    topo_info->threads     = malloc(topo_info->threads_cnt * sizeof(struct pal_cpu_thread_info));
    for (size_t i = 0; i < topo_info->threads_cnt; i++) {
        topo_info->threads[i].is_online = Data[index++] % 2;
        if (topo_info->threads[i].is_online) {
            topo_info->threads[i].core_id = Data[index++];
            for (size_t j = 0; j < MAX_CACHES; j++) {
                topo_info->threads[i].ids_of_caches[j] = Data[index++];
            }
        }
    }

    topo_info->cores_cnt = (Data[index++] % MAX_CORES) + 1;
    topo_info->cores     = malloc(topo_info->cores_cnt * sizeof(struct pal_cpu_core_info));
    for (size_t i = 0; i < topo_info->cores_cnt; i++) {
        topo_info->cores[i].socket_id = Data[index++];
        topo_info->cores[i].node_id   = Data[index++];
    }

    topo_info->sockets_cnt = (Data[index++] % MAX_SOCKETS) + 1;
    topo_info->sockets     = malloc(topo_info->sockets_cnt * sizeof(struct pal_socket_info));
    for (size_t i = 0; i < topo_info->sockets_cnt; i++) {
        topo_info->sockets[i].unused = Data[index++] % 26 + 'a';
    }

    topo_info->numa_nodes_cnt = (Data[index++] % MAX_NUMA_NODES) + 1;
    topo_info->numa_nodes = malloc(topo_info->numa_nodes_cnt * sizeof(struct pal_numa_node_info));
    for (size_t i = 0; i < topo_info->numa_nodes_cnt; i++) {
        topo_info->numa_nodes[i].is_online = Data[index++] % 2;
        if (topo_info->numa_nodes[i].is_online) {
            for (size_t j = 0; j < HUGEPAGES_MAX; j++) {
                topo_info->numa_nodes[i].nr_hugepages[j] = Data[index++];
            }
        }
    }

    topo_info->numa_distance_matrix =
        malloc(topo_info->numa_nodes_cnt * topo_info->numa_nodes_cnt * sizeof(size_t));
    for (size_t i = 0; i < topo_info->numa_nodes_cnt; i++) {
        for (size_t j = 0; j < topo_info->numa_nodes_cnt; j++) {
            topo_info->numa_distance_matrix[i * topo_info->numa_nodes_cnt + j] = Data[index++];
        }
    }

    int ret = sanitize_topo_info(topo_info);
    if (ret == 0) {
        print_data(topo_info);
        printf("Sanitize successful\n");
    } else {
        printf("Sanitize failed\n");
    }

    return 0;
}
