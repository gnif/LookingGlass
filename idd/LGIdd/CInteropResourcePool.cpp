#include "CInteropResourcePool.h"
#include "CDebug.h"

void CInteropResourcePool::Init(std::shared_ptr<CD3D11Device> dx11Device, std::shared_ptr<CD3D12Device> dx12Device)
{
  Reset();
  m_dx11Device = dx11Device;
  m_dx12Device = dx12Device;
}

void CInteropResourcePool::Reset()
{
  for (unsigned i = 0; i < POOL_SIZE; ++i)
    m_pool[i].Reset();
  m_dx11Device.reset();
  m_dx12Device.reset();
}

CInteropResource* CInteropResourcePool::Get(ComPtr<ID3D11Texture2D> srcTex)
{
  CInteropResource * res;
  unsigned freeSlot = POOL_SIZE;
  for (unsigned i = 0; i < POOL_SIZE; ++i)
  {
    res = &m_pool[i];
    if (!res->IsReady())
    {
      freeSlot = min(freeSlot, i);
      continue;
    }

    if (res->Compare(srcTex))
      return res;
  }

  if (freeSlot == POOL_SIZE)
  {
    DEBUG_ERROR("Interop Resouce Pool Full");
    return nullptr;
  }

  res = &m_pool[freeSlot];
  if (!res->Init(m_dx11Device, m_dx12Device, srcTex))
    return nullptr;

  return res;
}