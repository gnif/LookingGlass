/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#ifndef _H_D12_COMMANDGROUP_
#define _H_D12_COMMANDGROUP_

#include <stdbool.h>
#include <d3d12.h>

typedef struct D12CommandGroup
{
  ID3D12CommandAllocator    ** allocator;
  ID3D12GraphicsCommandList ** gfxList;
  ID3D12CommandList         ** cmdList;
  ID3D12Fence               ** fence;
  HANDLE                       event;
  UINT64                       fenceValue;
}
D12CommandGroup;

bool d12_commandGroupCreate(ID3D12Device3 * device, D3D12_COMMAND_LIST_TYPE type,
  D12CommandGroup * dst, PCWSTR name);

void d12_commandGroupFree(D12CommandGroup * grp);

bool d12_commandGroupExecute(ID3D12CommandQueue * queue, D12CommandGroup * grp);

void d12_commandGroupWait(D12CommandGroup * grp);

bool d12_commandGroupReset(D12CommandGroup * grp);

#endif
