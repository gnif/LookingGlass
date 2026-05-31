#pragma once

#include <Windows.h>
#include <wdf.h>
#include <wrl.h>
#include <memory>

#include "CD3D11Device.h"
#include "CD3D12Device.h"
#include "CD3D12CommandQueue.h"

using namespace Microsoft::WRL;

#define LG_MAX_DIRTY_RECTS 256

class CInteropResource
{
  private:
    std::shared_ptr<CD3D11Device> m_dx11Device;
    std::shared_ptr<CD3D12Device> m_dx12Device;

    /* this value is likely released, it is only used to check if the texture supplied
    is different, do not rely on it pointing to valid memory */
    void * m_srcTex;

    ComPtr<ID3D12Resource > m_d12Res;
    D3D11_TEXTURE2D_DESC    m_format;
    ComPtr<ID3D11Fence    > m_d11Fence;
    ComPtr<ID3D12Fence    > m_d12Fence;
    UINT64                  m_fenceValue;
    bool                    m_ready;

    RECT     m_dirtyRects[LG_MAX_DIRTY_RECTS];
    unsigned m_nbDirtyRects;

  public:
    bool Init(std::shared_ptr<CD3D11Device> dx11Device, std::shared_ptr<CD3D12Device> dx12Device, ComPtr<ID3D11Texture2D> srcTex);
    void Reset();

    bool IsReady() { return m_ready; }
    bool Compare(const ComPtr<ID3D11Texture2D>& srcTex);
    void Signal();
    void Sync(CD3D12CommandQueue& queue);
    void SetFullDamage();

    const ComPtr<ID3D12Resource>& GetRes() { return m_d12Res; }
    const D3D11_TEXTURE2D_DESC& GetFormat() { return m_format; }
};