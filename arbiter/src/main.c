#include "common/debug.h"
#include "common/option.h"
#include "porthole/client.h"

static struct Option options[] =
{
  // app options
  {
    .module         = "host",
    .name           = "socket",
    .description    = "The porthole host socket",
    .type           = OPTION_TYPE_STRING,
    .value.x_string = "/var/tmp/porthole",
  },
  {0}
};

static void map_event(uint32_t type, PortholeMap * map)
{
  DEBUG_INFO("map_event: %u, %u, %u", type, map->id, map->size);
}

static void unmap_event(uint32_t id)
{
  DEBUG_INFO("unmap_event: %u", id);
}

static void discon_event()
{
  DEBUG_INFO("discon_event");
}

int main(int argc, char * argv[])
{
  option_register(options);
  if (!option_parse(argc, argv))
    return -1;

  if (!option_validate())
    return -1;

  PortholeClient phc;
  if (!porthole_client_open(
        &phc,
        option_get_string("host", "socket"),
        map_event,
        unmap_event,
        discon_event))
  {
    return -1;
  }

  porthole_client_close(&phc);
  return 0;
}