#include <stdio.h>

#ifdef DEBUG
  #define DEBUG_PRINT(type, fmt, args...) do {fprintf(stderr, type " %20s:%-5u | %-20s | " fmt "\n", __FILE__, __LINE__, __FUNCTION__, ##args);} while (0)
#else
  #define DEBUG_PRINT(type, fmt, args...) do {} while(0)
#endif

#define DEBUG_INFO(fmt, args...) DEBUG_PRINT("[I]", fmt, ##args)
#define DEBUG_WARN(fmt, args...) DEBUG_PRINT("[W]", fmt, ##args)
#define DEBUG_ERROR(fmt, args...) DEBUG_PRINT("[E]", fmt, ##args)
#define DEBUG_FIXME(fmt, args...) DEBUG_PRINT("[F]", fmt, ##args)

#if defined(DEBUG_SPICE) | defined(DEBUG_IVSHMEM)
  #define DEBUG_PROTO(fmt, args...) DEBUG_PRINT("[P]", fmt, ##args)
#else
  #define DEBUG_PROTO(fmt, args...) do {} while(0)
#endif