#pragma once

#include <Windows.h>
#include <wdf.h>
#include <wrl.h>
#include <d3d11_4.h>
#include "CInteropResource.h"

using namespace Microsoft::WRL;

#define POOL_SIZE 10

class CInteropResourcePool
{
  private:
    CInteropResource m_pool[POOL_SIZE];
    
    std::shared_ptr<CD3D11Device> m_dx11Device;
    std::shared_ptr<CD3D12Device> m_dx12Device;
   
  public:
    void Init(std::shared_ptr<CD3D11Device> dx11Device, std::shared_ptr<CD3D12Device> dx12Device);
    void Reset();

    CInteropResource* Get(ComPtr<ID3D11Texture2D> srcTex);
};