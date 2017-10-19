#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool ivshmem_connect(const char * unix_socket);
void ivshmem_disconnect();
bool ivshmem_process();

uint16_t ivshmem_get_id();
void *   ivshmem_get_map();
size_t   ivshmem_get_map_size();