// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#ifndef _OUTPUTMANAGER_H_
#define _OUTPUTMANAGER_H_

#include <stdio.h>

#include "CommonTypes.h"
#include "warning.h"

//
// Handles the task of drawing into a window.
// Has the functionality to draw the mouse given a mouse shape buffer and position
//
class OUTPUTMANAGER
{
    public:
        OUTPUTMANAGER();
        ~OUTPUTMANAGER();
        DUPL_RETURN InitOutput(INT SingleOutput, _Out_ UINT* OutCount, _Out_ RECT* DeskBounds);
        DUPL_RETURN UpdateApplicationWindow(_In_ PTR_INFO* PointerInfo);
        DUPL_RETURN WaitNextVBlank();
        void CleanRefs();
        HANDLE GetSharedHandle();

    private:
    // Methods
        DUPL_RETURN OpenOutput(uint16_t vendorId, uint16_t productId, float refreshRate);
        DUPL_RETURN ProcessMonoMask(bool IsMono, _Inout_ PTR_INFO* PtrInfo, _Out_ INT* PtrWidth, _Out_ INT* PtrHeight, _Out_ INT* PtrLeft, _Out_ INT* PtrTop, _Outptr_result_bytebuffer_(*PtrHeight * *PtrWidth * BPP) BYTE** InitBuffer, _Out_ D3D11_BOX* Box);
        DUPL_RETURN MakeRTV();
        void SetViewPort(UINT Width, UINT Height);
        DUPL_RETURN InitShaders();
        DUPL_RETURN InitGeometry();
        DUPL_RETURN CreateSharedSurf(INT SingleOutput, _Out_ UINT* OutCount, _Out_ RECT* DeskBounds);
        DUPL_RETURN DrawFrame();
        DUPL_RETURN DrawMouse(_In_ PTR_INFO* PtrInfo);
        DUPL_RETURN Present();

    // Vars
        winrt::DisplayManager m_DisplayManager = nullptr;
        winrt::DisplayTarget m_DisplayTarget = nullptr;
        winrt::DisplaySource m_DisplaySource = nullptr;
        winrt::DisplayDevice m_DisplayDevice = nullptr;
        winrt::DisplayTaskPool m_DisplayTaskPool = nullptr;
        uint32_t m_DisplayWidth = 0;
        uint32_t m_DisplayHeight = 0;

        ID3D11Device5* m_Device;
        ID3D11DeviceContext4* m_DeviceContext;
        ID3D11RenderTargetView* m_RTV;
        ID3D11SamplerState* m_SamplerLinear;
        ID3D11BlendState* m_BlendState;
        ID3D11VertexShader* m_VertexShader;
        ID3D11PixelShader* m_PixelShader;
        ID3D11InputLayout* m_InputLayout;
        ID3D11Texture2D* m_SharedSurf;
        IDXGIKeyedMutex* m_KeyMutex;

        struct OutputSurface {
            winrt::DisplaySurface primary = nullptr;
            winrt::DisplayScanout scanout = nullptr;
            winrt::com_ptr<ID3D11Texture2D> surface;
        };
        std::vector<OutputSurface> m_OutputSurfaces;
        uint32_t m_OutputSurfaceIndex = 0;

        winrt::DisplayFence m_VBlankFenceOnDisplayDevice = nullptr;
        winrt::com_ptr<ID3D11Fence> m_VBlankFenceOnPresentationDevice;
        uint64_t m_VBlankFenceValue = 0;
        winrt::handle m_VBlankEvent;

        winrt::DisplayFence m_DisplayFenceOnDisplayDevice = nullptr;
        winrt::com_ptr<ID3D11Fence> m_DisplayFenceOnPresentationDevice;
        uint64_t m_DisplayFenceValue = 0;
        winrt::handle m_DisplayEvent;
};

#endif
