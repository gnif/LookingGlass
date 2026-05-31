#include "CFrameBufferPool.h"
#include "CSwapChainProcessor.h"

#include <stdint.h>

void CFrameBufferPool::Init(CSwapChainProcessor * swapChain)
{
  m_swapChain = swapChain;
}

void CFrameBufferPool::Reset()
{
  for (int i = 0; i < ARRAYSIZE(m_buffers); ++i)
    m_buffers[i].Reset();
}

CFrameBufferResource * CFrameBufferPool::Get(
  const CIndirectDeviceContext::PreparedFrameBuffer& buffer,
  size_t minSize)
{
  if (buffer.frameIndex > ARRAYSIZE(m_buffers) - 1)
    return nullptr;

  CFrameBufferResource* fbr = &m_buffers[buffer.frameIndex];
  if (!fbr->IsValid() || fbr->GetBase() != buffer.mem || fbr->GetSize() < minSize)
  {
    fbr->Reset();
    if (!fbr->Init(m_swapChain, buffer.frameIndex, buffer.mem, minSize))
      return nullptr;
  }

  return fbr;
}
