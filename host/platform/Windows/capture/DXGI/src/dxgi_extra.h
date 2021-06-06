/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include <dxgi.h>
#include <d3d11.h>
#include <d3dcommon.h>

// missing declarations in dxgi.h
HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **factory);
#define D3D_FEATURE_LEVEL_12_0 0xc000
#define D3D_FEATURE_LEVEL_12_1 0xc100

#ifndef DXGI_ERROR_ACCESS_LOST
#define DXGI_ERROR_ACCESS_LOST           _HRESULT_TYPEDEF_(0x887A0026L)
#endif

#ifndef DXGI_ERROR_WAIT_TIMEOUT
#define DXGI_ERROR_WAIT_TIMEOUT          _HRESULT_TYPEDEF_(0x887A0027L)
#endif

enum DXGI_OUTDUPL_POINTER_SHAPE_TYPE {
  DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME   = 0x1,
  DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR        = 0x2,
  DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR = 0x4
};

typedef struct DXGI_OUTDUPL_DESC {
    DXGI_MODE_DESC ModeDesc;
    DXGI_MODE_ROTATION Rotation;
    BOOL DesktopImageInSystemMemory;
}
DXGI_OUTDUPL_DESC;

typedef struct DXGI_OUTDUPL_POINTER_POSITION {
    POINT Position;
    BOOL Visible;
}
DXGI_OUTDUPL_POINTER_POSITION;

typedef struct DXGI_OUTDUPL_FRAME_INFO {
    LARGE_INTEGER LastPresentTime;
    LARGE_INTEGER LastMouseUpdateTime;
    UINT AccumulatedFrames;
    BOOL RectsCoalesced;
    BOOL ProtectedContentMaskedOut;
    DXGI_OUTDUPL_POINTER_POSITION PointerPosition;
    UINT TotalMetadataBufferSize;
    UINT PointerShapeBufferSize;
}
DXGI_OUTDUPL_FRAME_INFO;

typedef struct DXGI_OUTDUPL_MOVE_RECT {
    POINT SourcePoint;
    RECT DestinationRect;
}
DXGI_OUTDUPL_MOVE_RECT;

typedef struct DXGI_OUTDUPL_POINTER_SHAPE_INFO {
    UINT Type;
    UINT Width;
    UINT Height;
    UINT Pitch;
    POINT HotSpot;
}
DXGI_OUTDUPL_POINTER_SHAPE_INFO;

DEFINE_GUID(IID_IDXGIOutputDuplication, 0x191cfac3, 0xa341, 0x470d, 0xb2,0x6e,0xa8,0x64,0xf4,0x28,0x31,0x9c);

typedef interface IDXGIOutputDuplication IDXGIOutputDuplication;

typedef struct IDXGIOutputDuplicationVtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        IDXGIOutputDuplication* This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        IDXGIOutputDuplication* This);

    ULONG (STDMETHODCALLTYPE *Release)(
        IDXGIOutputDuplication* This);

    /*** IDXGIObject methods ***/
    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        IDXGIOutputDuplication* This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        IDXGIOutputDuplication* This,
        REFGUID guid,
        const IUnknown *object);

    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        IDXGIOutputDuplication* This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *GetParent)(
        IDXGIOutputDuplication* This,
        REFIID riid,
        void **parent);

    /*** IDXGIOutputDuplication methods ***/

    void (STDMETHODCALLTYPE *GetDesc)(
        IDXGIOutputDuplication* This,
        DXGI_OUTDUPL_DESC *pDesc);

    HRESULT (STDMETHODCALLTYPE *AcquireNextFrame)(
        IDXGIOutputDuplication* This,
        UINT TimeoutInMilliseconds,
        DXGI_OUTDUPL_FRAME_INFO *pFrameInfo,
        IDXGIResource **ppDesktopResource);

    HRESULT (STDMETHODCALLTYPE *GetFrameDirtyRects)(
        IDXGIOutputDuplication* This,
        UINT DirtyRectsBufferSize,
        RECT *pDirtyRectsBuffer,
        UINT *pDirtyRectsBufferSizeRequired);

    HRESULT (STDMETHODCALLTYPE *GetFrameMoveRects)(
        IDXGIOutputDuplication* This,
        UINT MoveRectsBufferSize,
        DXGI_OUTDUPL_MOVE_RECT *pMoveRectBuffer,
        UINT *pMoveRectsBufferSizeRequired);

    HRESULT (STDMETHODCALLTYPE *GetFramePointerShape)(
        IDXGIOutputDuplication* This,
        UINT PointerShapeBufferSize,
        void *pPointerShapeBuffer,
        UINT *pPointerShapeBufferSizeRequired,
        DXGI_OUTDUPL_POINTER_SHAPE_INFO *pPointerShapeInfo);

    HRESULT (STDMETHODCALLTYPE *MapDesktopSurface)(
        IDXGIOutputDuplication* This,
        DXGI_MAPPED_RECT *pLockedRect);

    HRESULT (STDMETHODCALLTYPE *UnMapDesktopSurface)(
        IDXGIOutputDuplication* This);

    HRESULT (STDMETHODCALLTYPE *ReleaseFrame)(
        IDXGIOutputDuplication* This);

    END_INTERFACE
}
IDXGIOutputDuplicationVtbl;

interface IDXGIOutputDuplication {
    CONST_VTBL IDXGIOutputDuplicationVtbl* lpVtbl;
};

#define IDXGIOutputDuplication_Release(This) (This)->lpVtbl->Release(This)
#define IDXGIOutputDuplication_GetDesc(This, pDesc) (This)->lpVtbl->GetDesc(This, pDesc)
#define IDXGIOutputDuplication_AcquireNextFrame(This, TimeoutInMilliseconds, pFrameInfo, ppDesktopResource) (This)->lpVtbl->AcquireNextFrame(This, TimeoutInMilliseconds, pFrameInfo, ppDesktopResource)
#define IDXGIOutputDuplication_GetFrameDirtyRects(This, DirtyRectsBufferSize, pDirectyRectsBuffer, pDirtyRectsBufferSizeRequired) (This)->lpVtbl->GetFrameDirtyRects(This, DirtyRectsBufferSize, pDirectyRectsBuffer, pDirtyRectsBufferSizeRequired)
#define IDXGIOutputDuplication_GetFrameMoveRects(This, MoveRectsBufferSize, pDirtyRectsBuffer, pDirtyRectsBufferSizeRequired) (This)->lpVtbl->GetFrameMoveRects(This, MoveRectsBufferSize, pDirtyRectsBuffer, pDirtyRectsBufferSizeRequired)
#define IDXGIOutputDuplication_GetFramePointerShape(This, PointerShapeBufferSize, pPointerShapeBuffer, pPointerShapeBufferSizeRequired, pPointerShapeInfo) (This)->lpVtbl->GetFramePointerShape(This, PointerShapeBufferSize, pPointerShapeBuffer, pPointerShapeBufferSizeRequired, pPointerShapeInfo)
#define IDXGIOutputDuplication_MapDesktopSurface(This, pLockedRect) (This)->lpVtbl->MapDesktopSurface(This, pLockedRect)
#define IDXGIOutputDuplication_UnMapDesktopSurface(This) (This)->lpVtbl->UnMapDesktopSurface(This)
#define IDXGIOutputDuplication_ReleaseFrame(This) (This)->lpVtbl->ReleaseFrame(This)

typedef struct DXGI_MODE_DESC1
{
  UINT Width;
  UINT Height;
  DXGI_RATIONAL RefreshRate;
  DXGI_FORMAT Format;
  DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
  DXGI_MODE_SCALING Scaling;
  BOOL Stereo;
}
DXGI_MODE_DESC1;

#ifndef __dxgicommon_h__
typedef enum DXGI_COLOR_SPACE_TYPE {
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709             = 0,
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709             = 1,
    DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709           = 2,
    DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020          = 3,
    DXGI_COLOR_SPACE_RESERVED                           = 4,
    DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601      = 5,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601         = 6,
    DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601           = 7,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709         = 8,
    DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709           = 9,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020        = 10,
    DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020          = 11,
    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020          = 12,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020      = 13,
    DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020        = 14,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020     = 15,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020   = 16,
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020            = 17,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020    = 18,
    DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020      = 19,
    DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709           = 20,
    DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P2020          = 21,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P709         = 22,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P2020        = 23,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020     = 24,
    DXGI_COLOR_SPACE_CUSTOM                             = 0xFFFFFFFF
} DXGI_COLOR_SPACE_TYPE;
#endif

DEFINE_GUID(IID_IDXGIOutput1, 0x00cddea8, 0x939b, 0x4b83, 0xa3,0x40,0xa6,0x85,0x22,0x66,0x66,0xcc);

typedef struct IDXGIOutput1 IDXGIOutput1;

typedef struct IDXGIOutput1Vtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        IDXGIOutput1* This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        IDXGIOutput1* This);

    ULONG (STDMETHODCALLTYPE *Release)(
        IDXGIOutput1* This);

    /*** IDXGIObject methods ***/
    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        IDXGIOutput1* This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        IDXGIOutput1* This,
        REFGUID guid,
        const IUnknown *object);

    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        IDXGIOutput1* This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *GetParent)(
        IDXGIOutput1* This,
        REFIID riid,
        void **parent);

    /*** IDXGIOutput methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDesc)(
        IDXGIOutput1* This,
        DXGI_OUTPUT_DESC *desc);

    HRESULT (STDMETHODCALLTYPE *GetDisplayModeList)(
        IDXGIOutput1* This,
        DXGI_FORMAT format,
        UINT flags,
        UINT *mode_count,
        DXGI_MODE_DESC *desc);

    HRESULT (STDMETHODCALLTYPE *FindClosestMatchingMode)(
        IDXGIOutput1* This,
        const DXGI_MODE_DESC *mode,
        DXGI_MODE_DESC *closest_match,
        IUnknown *device);

    HRESULT (STDMETHODCALLTYPE *WaitForVBlank)(
        IDXGIOutput1* This);

    HRESULT (STDMETHODCALLTYPE *TakeOwnership)(
        IDXGIOutput1* This,
        IUnknown *device,
        WINBOOL exclusive);

    void (STDMETHODCALLTYPE *ReleaseOwnership)(
        IDXGIOutput1* This);

    HRESULT (STDMETHODCALLTYPE *GetGammaControlCapabilities)(
        IDXGIOutput1* This,
        DXGI_GAMMA_CONTROL_CAPABILITIES *gamma_caps);

    HRESULT (STDMETHODCALLTYPE *SetGammaControl)(
        IDXGIOutput1* This,
        const DXGI_GAMMA_CONTROL *gamma_control);

    HRESULT (STDMETHODCALLTYPE *GetGammaControl)(
        IDXGIOutput1* This,
        DXGI_GAMMA_CONTROL *gamma_control);

    HRESULT (STDMETHODCALLTYPE *SetDisplaySurface)(
        IDXGIOutput1* This,
        IDXGISurface *surface);

    HRESULT (STDMETHODCALLTYPE *GetDisplaySurfaceData)(
        IDXGIOutput1* This,
        IDXGISurface *surface);

    HRESULT (STDMETHODCALLTYPE *GetFrameStatistics)(
        IDXGIOutput1* This,
        DXGI_FRAME_STATISTICS *stats);

    /*** IDXGIOutput1 methods ***/

    HRESULT (STDMETHODCALLTYPE *GetDisplayModeList1)(
        IDXGIOutput1* This,
        DXGI_FORMAT EnumFormat,
        UINT Flags,
        UINT *pNumModes,
        DXGI_MODE_DESC1 *pDesc);

    HRESULT (STDMETHODCALLTYPE *FindClosestMatchingMode1)(
        IDXGIOutput1* This,
        const DXGI_MODE_DESC1 *pModeToMatch,
        DXGI_MODE_DESC1 *pClosestMatch,
        IUnknown *pConcernedDevice);

    HRESULT (STDMETHODCALLTYPE *GetDisplaySurfaceData1)(
        IDXGIOutput1* This,
        IDXGIResource *pDestination);

    HRESULT (STDMETHODCALLTYPE *DuplicateOutput)(
        IDXGIOutput1* This,
        IUnknown *pDevice,
        IDXGIOutputDuplication **ppOutputDuplication);

    END_INTERFACE
}
IDXGIOutput1Vtbl;
interface IDXGIOutput1 {
    CONST_VTBL IDXGIOutput1Vtbl* lpVtbl;
};

#define IDXGIOutput1_DuplicateOutput(This,pDevice,ppOutputDuplication) (This)->lpVtbl->DuplicateOutput(This,pDevice,ppOutputDuplication)
#define IDXGIOutput1_Release(This) (This)->lpVtbl->Release(This);

DEFINE_GUID(IID_IDXGIOutput5, 0x80a07424, 0xab52, 0x42eb, 0x83,0x3c,0x0c,0x42,0xfd,0x28,0x2d,0x98);

typedef struct IDXGIOutput5 IDXGIOutput5;

typedef struct IDXGIOutput5Vtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        IDXGIOutput5* This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        IDXGIOutput5* This);

    ULONG (STDMETHODCALLTYPE *Release)(
        IDXGIOutput5* This);

    /*** IDXGIObject methods ***/
    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        IDXGIOutput5* This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        IDXGIOutput5* This,
        REFGUID guid,
        const IUnknown *object);

    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        IDXGIOutput5* This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *GetParent)(
        IDXGIOutput5* This,
        REFIID riid,
        void **parent);

    /*** IDXGIOutput methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDesc)(
        IDXGIOutput5* This,
        DXGI_OUTPUT_DESC *desc);

    HRESULT (STDMETHODCALLTYPE *GetDisplayModeList)(
        IDXGIOutput5* This,
        DXGI_FORMAT format,
        UINT flags,
        UINT *mode_count,
        DXGI_MODE_DESC *desc);

    HRESULT (STDMETHODCALLTYPE *FindClosestMatchingMode)(
        IDXGIOutput5* This,
        const DXGI_MODE_DESC *mode,
        DXGI_MODE_DESC *closest_match,
        IUnknown *device);

    HRESULT (STDMETHODCALLTYPE *WaitForVBlank)(
        IDXGIOutput5* This);

    HRESULT (STDMETHODCALLTYPE *TakeOwnership)(
        IDXGIOutput5* This,
        IUnknown *device,
        WINBOOL exclusive);

    void (STDMETHODCALLTYPE *ReleaseOwnership)(
        IDXGIOutput5* This);

    HRESULT (STDMETHODCALLTYPE *GetGammaControlCapabilities)(
        IDXGIOutput5* This,
        DXGI_GAMMA_CONTROL_CAPABILITIES *gamma_caps);

    HRESULT (STDMETHODCALLTYPE *SetGammaControl)(
        IDXGIOutput5* This,
        const DXGI_GAMMA_CONTROL *gamma_control);

    HRESULT (STDMETHODCALLTYPE *GetGammaControl)(
        IDXGIOutput5* This,
        DXGI_GAMMA_CONTROL *gamma_control);

    HRESULT (STDMETHODCALLTYPE *SetDisplaySurface)(
        IDXGIOutput5* This,
        IDXGISurface *surface);

    HRESULT (STDMETHODCALLTYPE *GetDisplaySurfaceData)(
        IDXGIOutput5* This,
        IDXGISurface *surface);

    HRESULT (STDMETHODCALLTYPE *GetFrameStatistics)(
        IDXGIOutput5* This,
        DXGI_FRAME_STATISTICS *stats);

    /*** IDXGIOutput1 methods ***/

    HRESULT (STDMETHODCALLTYPE *GetDisplayModeList1)(
        IDXGIOutput5* This,
        DXGI_FORMAT EnumFormat,
        UINT Flags,
        UINT *pNumModes,
        DXGI_MODE_DESC1 *pDesc);

    HRESULT (STDMETHODCALLTYPE *FindClosestMatchingMode1)(
        IDXGIOutput5* This,
        const DXGI_MODE_DESC1 *pModeToMatch,
        DXGI_MODE_DESC1 *pClosestMatch,
        IUnknown *pConcernedDevice);

    HRESULT (STDMETHODCALLTYPE *GetDisplaySurfaceData1)(
        IDXGIOutput5* This,
        IDXGIResource *pDestination);

    HRESULT (STDMETHODCALLTYPE *DuplicateOutput)(
        IDXGIOutput5* This,
        IUnknown *pDevice,
        IDXGIOutputDuplication **ppOutputDuplication);

    /*** IDXGIOutput2 methods ***/

    BOOL (STDMETHODCALLTYPE *SupportsOverlays)(
        IDXGIOutput5* This);

    /*** IDXGIOutput3 methods ***/

    HRESULT (STDMETHODCALLTYPE *CheckOverlaySupport)(
        IDXGIOutput5* This,
        DXGI_FORMAT EnumFormat,
        IUnknown *pConcernedDevice,
        UINT *pFlags);

    /*** IDXGIOutput4 methods ***/

    HRESULT (STDMETHODCALLTYPE *CheckOverlayColorSpaceSupport)(
        IDXGIOutput5* This,
        DXGI_FORMAT Format,
        DXGI_COLOR_SPACE_TYPE ColorSpace,
        IUnknown *pConcernedDevice,
        UINT *pFlags);

    /*** IDXGIOutput5 methods ***/

    HRESULT (STDMETHODCALLTYPE *DuplicateOutput1)(
        IDXGIOutput5* This,
        IUnknown *pDevice,
        UINT Flags,
        UINT SupportedFormatsCount,
        const DXGI_FORMAT *pSupportedFormats,
        IDXGIOutputDuplication **ppOutputDuplication);

    END_INTERFACE
}
IDXGIOutput5Vtbl;
interface IDXGIOutput5 {
    CONST_VTBL IDXGIOutput5Vtbl* lpVtbl;
};

#define IDXGIOutput5_DuplicateOutput1(This,pDevice,Flags,SupportedForamtsCount,pSupportedFormats,ppOutputDuplication) (This)->lpVtbl->DuplicateOutput1(This,pDevice,Flags,SupportedForamtsCount,pSupportedFormats,ppOutputDuplication)
#define IDXGIOutput5_Release(This) (This)->lpVtbl->Release(This);


static const char * DXGI_FORMAT_STR[] = {
  "DXGI_FORMAT_UNKNOWN",
  "DXGI_FORMAT_R32G32B32A32_TYPELESS",
  "DXGI_FORMAT_R32G32B32A32_FLOAT",
  "DXGI_FORMAT_R32G32B32A32_UINT",
  "DXGI_FORMAT_R32G32B32A32_SINT",
  "DXGI_FORMAT_R32G32B32_TYPELESS",
  "DXGI_FORMAT_R32G32B32_FLOAT",
  "DXGI_FORMAT_R32G32B32_UINT",
  "DXGI_FORMAT_R32G32B32_SINT",
  "DXGI_FORMAT_R16G16B16A16_TYPELESS",
  "DXGI_FORMAT_R16G16B16A16_FLOAT",
  "DXGI_FORMAT_R16G16B16A16_UNORM",
  "DXGI_FORMAT_R16G16B16A16_UINT",
  "DXGI_FORMAT_R16G16B16A16_SNORM",
  "DXGI_FORMAT_R16G16B16A16_SINT",
  "DXGI_FORMAT_R32G32_TYPELESS",
  "DXGI_FORMAT_R32G32_FLOAT",
  "DXGI_FORMAT_R32G32_UINT",
  "DXGI_FORMAT_R32G32_SINT",
  "DXGI_FORMAT_R32G8X24_TYPELESS",
  "DXGI_FORMAT_D32_FLOAT_S8X24_UINT",
  "DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS",
  "DXGI_FORMAT_X32_TYPELESS_G8X24_UINT",
  "DXGI_FORMAT_R10G10B10A2_TYPELESS",
  "DXGI_FORMAT_R10G10B10A2_UNORM",
  "DXGI_FORMAT_R10G10B10A2_UINT",
  "DXGI_FORMAT_R11G11B10_FLOAT",
  "DXGI_FORMAT_R8G8B8A8_TYPELESS",
  "DXGI_FORMAT_R8G8B8A8_UNORM",
  "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB",
  "DXGI_FORMAT_R8G8B8A8_UINT",
  "DXGI_FORMAT_R8G8B8A8_SNORM",
  "DXGI_FORMAT_R8G8B8A8_SINT",
  "DXGI_FORMAT_R16G16_TYPELESS",
  "DXGI_FORMAT_R16G16_FLOAT",
  "DXGI_FORMAT_R16G16_UNORM",
  "DXGI_FORMAT_R16G16_UINT",
  "DXGI_FORMAT_R16G16_SNORM",
  "DXGI_FORMAT_R16G16_SINT",
  "DXGI_FORMAT_R32_TYPELESS",
  "DXGI_FORMAT_D32_FLOAT",
  "DXGI_FORMAT_R32_FLOAT",
  "DXGI_FORMAT_R32_UINT",
  "DXGI_FORMAT_R32_SINT",
  "DXGI_FORMAT_R24G8_TYPELESS",
  "DXGI_FORMAT_D24_UNORM_S8_UINT",
  "DXGI_FORMAT_R24_UNORM_X8_TYPELESS",
  "DXGI_FORMAT_X24_TYPELESS_G8_UINT",
  "DXGI_FORMAT_R8G8_TYPELESS",
  "DXGI_FORMAT_R8G8_UNORM",
  "DXGI_FORMAT_R8G8_UINT",
  "DXGI_FORMAT_R8G8_SNORM",
  "DXGI_FORMAT_R8G8_SINT",
  "DXGI_FORMAT_R16_TYPELESS",
  "DXGI_FORMAT_R16_FLOAT",
  "DXGI_FORMAT_D16_UNORM",
  "DXGI_FORMAT_R16_UNORM",
  "DXGI_FORMAT_R16_UINT",
  "DXGI_FORMAT_R16_SNORM",
  "DXGI_FORMAT_R16_SINT",
  "DXGI_FORMAT_R8_TYPELESS",
  "DXGI_FORMAT_R8_UNORM",
  "DXGI_FORMAT_R8_UINT",
  "DXGI_FORMAT_R8_SNORM",
  "DXGI_FORMAT_R8_SINT",
  "DXGI_FORMAT_A8_UNORM",
  "DXGI_FORMAT_R1_UNORM",
  "DXGI_FORMAT_R9G9B9E5_SHAREDEXP",
  "DXGI_FORMAT_R8G8_B8G8_UNORM",
  "DXGI_FORMAT_G8R8_G8B8_UNORM",
  "DXGI_FORMAT_BC1_TYPELESS",
  "DXGI_FORMAT_BC1_UNORM",
  "DXGI_FORMAT_BC1_UNORM_SRGB",
  "DXGI_FORMAT_BC2_TYPELESS",
  "DXGI_FORMAT_BC2_UNORM",
  "DXGI_FORMAT_BC2_UNORM_SRGB",
  "DXGI_FORMAT_BC3_TYPELESS",
  "DXGI_FORMAT_BC3_UNORM",
  "DXGI_FORMAT_BC3_UNORM_SRGB",
  "DXGI_FORMAT_BC4_TYPELESS",
  "DXGI_FORMAT_BC4_UNORM",
  "DXGI_FORMAT_BC4_SNORM",
  "DXGI_FORMAT_BC5_TYPELESS",
  "DXGI_FORMAT_BC5_UNORM",
  "DXGI_FORMAT_BC5_SNORM",
  "DXGI_FORMAT_B5G6R5_UNORM",
  "DXGI_FORMAT_B5G5R5A1_UNORM",
  "DXGI_FORMAT_B8G8R8A8_UNORM",
  "DXGI_FORMAT_B8G8R8X8_UNORM",
  "DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM",
  "DXGI_FORMAT_B8G8R8A8_TYPELESS",
  "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB",
  "DXGI_FORMAT_B8G8R8X8_TYPELESS",
  "DXGI_FORMAT_B8G8R8X8_UNORM_SRGB",
  "DXGI_FORMAT_BC6H_TYPELESS",
  "DXGI_FORMAT_BC6H_UF16",
  "DXGI_FORMAT_BC6H_SF16",
  "DXGI_FORMAT_BC7_TYPELESS",
  "DXGI_FORMAT_BC7_UNORM",
  "DXGI_FORMAT_BC7_UNORM_SRGB",
  "DXGI_FORMAT_AYUV",
  "DXGI_FORMAT_Y410",
  "DXGI_FORMAT_Y416",
  "DXGI_FORMAT_NV12",
  "DXGI_FORMAT_P010",
  "DXGI_FORMAT_P016",
  "DXGI_FORMAT_420_OPAQUE",
  "DXGI_FORMAT_YUY2",
  "DXGI_FORMAT_Y210",
  "DXGI_FORMAT_Y216",
  "DXGI_FORMAT_NV11",
  "DXGI_FORMAT_AI44",
  "DXGI_FORMAT_IA44",
  "DXGI_FORMAT_P8",
  "DXGI_FORMAT_A8P8",
  "DXGI_FORMAT_B4G4R4A4_UNORM",

  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,

  "DXGI_FORMAT_P208",
  "DXGI_FORMAT_V208",
  "DXGI_FORMAT_V408"
};

static const char * GetDXGIFormatStr(DXGI_FORMAT format)
{
  if (format > sizeof(DXGI_FORMAT_STR) / sizeof(const char *))
    return DXGI_FORMAT_STR[0];
  return DXGI_FORMAT_STR[format];
}