#include <stdbool.h>

// exit code for user opted to exit looking-glass-host
#define LG_HOST_EXIT_USER    0xfee1dead
// exit code for capture errors that should result in a restart, e.g. UAC
#define LG_HOST_EXIT_CAPTURE 0xdead0000
// exit code for terminated
#define LG_HOST_EXIT_KILLED  0xdeadbeef
// exit code for failed to start
#define LG_HOST_EXIT_FAILED  0xdeadbaad

bool HandleService(int argc, char * argv[]);
