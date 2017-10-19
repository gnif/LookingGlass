#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool ivshmem_connect(const char * unix_socket);
void ivshmem_close();

void * ivshmem_get_map();
size_t ivshmem_get_map_size();