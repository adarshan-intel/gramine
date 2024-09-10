// void fix_topo_info(struct pal_topo_info * topo_info)
// {
// // Fix First thread must be online
//     if (topo_info->threads_cnt == 0 || !topo_info->threads[0].is_online) {
//         topo_info->threads[0].is_online = 1;
//     }

//     // Fix Invalid socket ID and node ID
//     for (size_t i = 0; i < topo_info->cores_cnt; i++) {
//         topo_info->cores[i].socket_id %= topo_info->sockets_cnt;
//         topo_info->cores[i].node_id %= topo_info->numa_nodes_cnt;
//     }

//     // Fix Invalid core ID
//     for (size_t i = 0; i < topo_info->threads_cnt; i++) {
//         if (topo_info->threads[i].is_online) {
//             topo_info->threads[i].core_id %= topo_info->cores_cnt;
//             for (size_t j = 0; j < MAX_CACHES; j++) {
//                 topo_info->threads[i].ids_of_caches[j] %= topo_info->caches_cnt;
//             }
//         }
//     }

//     // Fix Distance matrix is not symmetric and Invalid distance matrix
//     for (size_t i = 0; i < topo_info->numa_nodes_cnt; i++) {
//         for (size_t j = 0; j < topo_info->numa_nodes_cnt; j++) {
//             if (!topo_info->numa_nodes[i].is_online || !topo_info->numa_nodes[j].is_online) {
//                 topo_info->numa_distance_matrix[i * topo_info->numa_nodes_cnt + j] = 0;
//             } else {
//                 topo_info->numa_distance_matrix[i * topo_info->numa_nodes_cnt + j] =
//                     topo_info->numa_distance_matrix[j * topo_info->numa_nodes_cnt + i];
//             }
//         }
//     }
// }

