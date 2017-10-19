#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool ivshmem_connect(const char * unix_socket);
void ivshmem_disconnect();
bool ivshmem_process();

uint16_t ivshmem_get_id();
void *   ivshmem_get_map();
size_t   ivshmem_get_map_size();

enum IVSHMEMWaitResult
{
  IVSHMEM_WAIT_RESULT_OK,
  IVSHMEM_WAIT_RESULT_TIMEOUT,
  IVSHMEM_WAIT_RESULT_ERROR
};

enum IVSHMEMWaitResult ivshmem_wait_irq(uint16_t vector);
bool ivshmem_kick_irq(uint16_t clientID, uint16_t vector);