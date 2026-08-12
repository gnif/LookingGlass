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

#include "transport/CTransportManager.h"
#include "transport/lgmp/CLGMPTransport.h"

#include <new>

static std::unique_ptr<ITransport> CreateLGMP()
{
  return std::unique_ptr<ITransport>(new (std::nothrow) CLGMPTransport());
}

std::unique_ptr<CTransportManager> CreateTransport()
{
  std::unique_ptr<CTransportManager> manager(
    new (std::nothrow) CTransportManager());
  if (!manager || !manager->Add(1, "LGMP", true, true, CreateLGMP))
    return std::unique_ptr<CTransportManager>();
  return manager;
}
