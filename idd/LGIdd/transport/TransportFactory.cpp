/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "transport/TransportFactory.h"

#include "CDebug.h"
#include "config/CSettings.h"
#include "transport/CTransportManager.h"
#include "transport/lgmp/CLGMPTransport.h"

#include <new>

namespace
{
  struct Provider
  {
    CTransportManager::CreateFn create;
  };

  std::unique_ptr<ITransport> CreateLGMP(
    const TransportInstance& config)
  {
    return std::unique_ptr<ITransport>(
      new (std::nothrow) CLGMPTransport(config));
  }

  const TransportKind KINDS[] =
  {
    { L"LGMP", 1 },
  };

  const Provider PROVIDERS[] =
  {
    { CreateLGMP },
  };

  static_assert(ARRAYSIZE(KINDS) == ARRAYSIZE(PROVIDERS),
    "Every transport kind must have a provider");

  std::unique_ptr<CTransportManager> Build(
    const ResolvedTransportInstances& instances)
  {
    std::unique_ptr<CTransportManager> manager(
      new (std::nothrow) CTransportManager());
    if (!manager)
      return std::unique_ptr<CTransportManager>();

    for (const ResolvedTransportInstance& instance : instances)
      if (!manager->Add(instance.config, instance.primary,
          PROVIDERS[instance.kindIndex].create))
        return std::unique_ptr<CTransportManager>();
    return manager;
  }
}

std::unique_ptr<CTransportManager> CreateTransport()
{
  TransportInstances instances = g_settings.LoadTransportInstances();
  ResolvedTransportInstances resolved;
  bool usedDefaults = false;
  if (!ResolveTransportInstances(instances, KINDS, ARRAYSIZE(KINDS),
      resolved, usedDefaults))
    return std::unique_ptr<CTransportManager>();
  if (usedDefaults)
    DEBUG_WARN("Unsupported transport instance configuration; using defaults");

  return Build(resolved);
}
