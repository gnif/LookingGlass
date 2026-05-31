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

#include "command_group.h"
#include "d12.h"

#include "com_ref.h"
#include "common/debug.h"
#include "common/windebug.h"

bool d12_commandGroupCreate(ID3D12Device3 * device, D3D12_COMMAND_LIST_TYPE type,
  D12CommandGroup * dst, LPCWSTR name)
{
  bool result = false;
  HRESULT hr;
  comRef_scopePush(10);

  comRef_defineLocal(ID3D12CommandAllocator, allocator);
  hr = ID3D12Device3_CreateCommandAllocator(
    device,
    type,
    &IID_ID3D12CommandAllocator,
    (void **)allocator);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Failed to create the ID3D12CommandAllocator");
    goto exit;
  }
  ID3D12CommandAllocator_SetName(*allocator, name);

  comRef_defineLocal(ID3D12GraphicsCommandList, gfxList);
  hr = ID3D12Device3_CreateCommandList(
    device,
    0,
    type,
    *allocator,
    NULL,
    &IID_ID3D12GraphicsCommandList,
    (void **)gfxList);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create ID3D12GraphicsCommandList", hr);
    goto exit;
  }
  ID3D12GraphicsCommandList_SetName(*gfxList, name);

  comRef_defineLocal(ID3D12CommandList, cmdList);
  hr = ID3D12GraphicsCommandList_QueryInterface(
    *gfxList, &IID_ID3D12CommandList, (void **)cmdList);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to query the ID3D12CommandList interface", hr);
    goto exit;
  }

  comRef_defineLocal(ID3D12Fence, fence);
  hr = ID3D12Device3_CreateFence(
    device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)fence);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create ID3D12Fence", hr);
    goto exit;
  }

  // Create the completion event for the fence
  HANDLE event = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (!event)
  {
    DEBUG_WINERROR("Failed to create the completion event", GetLastError());
    goto exit;
  }

  comRef_toGlobal(dst->allocator, allocator);
  comRef_toGlobal(dst->gfxList  , gfxList  );
  comRef_toGlobal(dst->cmdList  , cmdList  );
  comRef_toGlobal(dst->fence    , fence    );
  dst->event      = event;
  dst->fenceValue = 0;

  result = true;

exit:
  comRef_scopePop();
  return result;
}

void d12_commandGroupFree(D12CommandGroup * grp)
{
  // only the handle needs to be free'd, the rest is handled by comRef
  if (grp->event)
  {
    CloseHandle(grp->event);
    grp->event = NULL;
  }
}

bool d12_commandGroupExecute(ID3D12CommandQueue * queue, D12CommandGroup * grp)
{
  HRESULT hr = ID3D12GraphicsCommandList_Close(*grp->gfxList);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to close the command list", hr);
    return false;
  }

  ID3D12CommandQueue_ExecuteCommandLists(queue, 1, grp->cmdList);

  hr = ID3D12CommandQueue_Signal(queue, *grp->fence, ++grp->fenceValue);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to set the fence signal", hr);
    return false;
  }

  return true;
}

void d12_commandGroupWait(D12CommandGroup * grp)
{
  if (ID3D12Fence_GetCompletedValue(*grp->fence) >= grp->fenceValue)
    return;

  ID3D12Fence_SetEventOnCompletion(*grp->fence, grp->fenceValue, grp->event);
  WaitForSingleObject(grp->event, INFINITE);
}

bool d12_commandGroupReset(D12CommandGroup * grp)
{
  HRESULT hr = ID3D12CommandAllocator_Reset(*grp->allocator);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to reset the command allocator", hr);
    return false;
  }

  hr = ID3D12GraphicsCommandList_Reset(*grp->gfxList, *grp->allocator, NULL);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to reset the graphics command list", hr);
    return false;
  }

  return true;
}
