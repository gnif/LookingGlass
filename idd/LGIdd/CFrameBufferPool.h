#pragma once

#include "CFrameBufferResource.h"
#include "CIndirectDeviceContext.h"
#include "common/KVMFR.h"

//class CSwapChainProcessor;

class CFrameBufferPool
{
  CSwapChainProcessor * m_swapChain;

  CFrameBufferResource m_buffers[LGMP_Q_FRAME_LEN];

  public:
    void Init(CSwapChainProcessor * swapChain);
    void Reset();

    CFrameBufferResource* CFrameBufferPool::Get(
      const CIndirectDeviceContext::PreparedFrameBuffer& buffer,
      size_t minSize);
};