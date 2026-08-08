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

#pragma once

#include "CPipeEndpoint.h"

#include <stddef.h>
#include <stdint.h>

class CInputPipeClient : private IPipeEndpointHandler
{
public:
  ~CInputPipeClient() { Stop(); }

  bool Start();
  void Stop();
  bool IsConnected() const { return m_endpoint.IsConnected(); }

private:
  void OnPipeConnected() override;
  void OnPipeDisconnected() override;
  bool OnPipeMessage(const void * message, size_t size) override;

  CPipeEndpoint m_endpoint;
  uint64_t m_lastSequence = 0;
};
