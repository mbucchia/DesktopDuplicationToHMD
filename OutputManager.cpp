// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#include "OutputManager.h"
using namespace DirectX;
using namespace winrt;

// How early do we request wake up prior to VBlank. This must be sufficient to run our copy shader.
static constexpr uint64_t OffsetFromVBlank = 5'000'000;

//
// Constructor NULLs out all pointers & sets appropriate var vals
//
OUTPUTMANAGER::OUTPUTMANAGER() : m_Device(nullptr),
                                 m_DeviceContext(nullptr),
                                 m_RTV(nullptr),
                                 m_SamplerLinear(nullptr),
                                 m_BlendState(nullptr),
                                 m_VertexShader(nullptr),
                                 m_PixelShader(nullptr),
                                 m_InputLayout(nullptr),
                                 m_SharedSurf(nullptr),
                                 m_KeyMutex(nullptr)
{
}

//
// Destructor which calls CleanRefs to release all references and memory.
//
OUTPUTMANAGER::~OUTPUTMANAGER()
{
    WaitNextVBlank();
    CleanRefs();
}

//
// Initialize all state
//
DUPL_RETURN OUTPUTMANAGER::InitOutput(INT SingleOutput, _Out_ UINT* OutCount, _Out_ RECT* DeskBounds)
{
    HRESULT hr;

    // Open the output device and create the backbuffers;
    OpenOutput(0xd94d, 0xc207, 90.f);

    // Create shared texture
    DUPL_RETURN Return = CreateSharedSurf(SingleOutput, OutCount, DeskBounds);
    if (Return != DUPL_RETURN_SUCCESS)
    {
        return Return;
    }

    // Make new render target view
    Return = MakeRTV();
    if (Return != DUPL_RETURN_SUCCESS)
    {
        return Return;
    }

    // Set view port
    SetViewPort(m_DisplayWidth, m_DisplayHeight);

    // Create the sample state
    D3D11_SAMPLER_DESC SampDesc;
    RtlZeroMemory(&SampDesc, sizeof(SampDesc));
    SampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    SampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    SampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    SampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    SampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    SampDesc.MinLOD = 0;
    SampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m_Device->CreateSamplerState(&SampDesc, &m_SamplerLinear);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create sampler state in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Create the blend state
    D3D11_BLEND_DESC BlendStateDesc;
    BlendStateDesc.AlphaToCoverageEnable = FALSE;
    BlendStateDesc.IndependentBlendEnable = FALSE;
    BlendStateDesc.RenderTarget[0].BlendEnable = TRUE;
    BlendStateDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    BlendStateDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    BlendStateDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    BlendStateDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    BlendStateDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    BlendStateDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    BlendStateDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = m_Device->CreateBlendState(&BlendStateDesc, &m_BlendState);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create blend state in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Initialize shaders
    Return = InitShaders();
    if (Return != DUPL_RETURN_SUCCESS)
    {
        return Return;
    }

    return Return;
}

//
// Open Direct Display Output.
//
DUPL_RETURN OUTPUTMANAGER::OpenOutput(uint16_t vendorId, uint16_t productId, float refreshRate) {
    // First, we disable Nvidia's protection of direct display mode devices. This is an undocumented API,
    // but the function ID is available online.
    {
        typedef PVOID(*pfnQueryInterface)(DWORD);
        typedef DWORD(*pfnNvAPI_Initialize)();
        typedef DWORD(*pfnNvAPI_DISP_DisableDirectMode)(DWORD, DWORD);
        HMODULE nvapi;
        nvapi = LoadLibraryA("nvapi64.dll");
        if (nvapi)
        {
            pfnQueryInterface QueryInterface = (pfnQueryInterface)GetProcAddress(nvapi, "nvapi_QueryInterface");
            if (QueryInterface)
            {
                pfnNvAPI_Initialize NvAPI_Initialize = (pfnNvAPI_Initialize)QueryInterface(0x150E828UL);
                // https://www.cnblogs.com/zzz3265/p/16517057.html
                pfnNvAPI_DISP_DisableDirectMode NvAPI_DISP_DisableDirectMode = (pfnNvAPI_DISP_DisableDirectMode)QueryInterface(0x7951E57CUL);
                if (NvAPI_Initialize && NvAPI_DISP_DisableDirectMode)
                {
                    DWORD retInitialize = NvAPI_Initialize();
                    DWORD retDisableDirectMode = NvAPI_DISP_DisableDirectMode(vendorId, 0);
                }
            }
            FreeLibrary(nvapi);
        }
    }

    // Find the output.
    m_DisplayManager = DisplayManager::Create(winrt::DisplayManagerOptions::None);
    IVectorView<winrt::DisplayTarget> targets = m_DisplayManager.GetCurrentTargets();
    auto myTargets = winrt::single_threaded_vector<winrt::DisplayTarget>();
    for (auto&& target : targets)
    {
        winrt::DisplayMonitor monitor = target.TryGetMonitor();
        if (!monitor)
        {
            continue;
        }
        winrt::com_array<uint8_t> edidBuffer = monitor.GetDescriptor(winrt::DisplayMonitorDescriptorKind::Edid);
        const uint16_t deviceVendorId = *(uint16_t*)(edidBuffer.data() + 8);
        const uint16_t deviceProductId = *(uint16_t*)(edidBuffer.data() + 10);

        if (deviceVendorId == vendorId && deviceProductId == productId)
        {
            myTargets.Append(target);
            break;
        }
    }

    if (myTargets.Size() == 0)
    {
        return ProcessFailure(m_Device, L"No device found in OUTPUTMANAGER", L"Error", E_UNEXPECTED);
    }

    HRESULT hr;

    auto stateResult = m_DisplayManager.TryAcquireTargetsAndCreateEmptyState(myTargets);
    hr = stateResult.ExtendedErrorCode();
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to acquire target in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }
    auto state = stateResult.State();

    auto target = *myTargets.First();
    winrt::DisplayPath path = state.ConnectTarget(target);

    // Configure the device.
    path.IsInterlaced(false);
    path.Scaling(winrt::DisplayPathScaling::Identity);
    // Output format cannot be sRGB, but our RTVs will be.
    path.SourcePixelFormat(winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized);

    // Pick the desired refresh rate.
    winrt::IVectorView<winrt::DisplayModeInfo> modes = path.FindModes(winrt::DisplayModeQueryOptions::OnlyPreferredResolution);
    winrt::DisplayModeInfo bestMode{ nullptr };
    double bestModeDiff = INFINITY;
    for (auto&& mode : modes)
    {
        auto vSync = mode.PresentationRate().VerticalSyncRate;
        double vSyncDouble = (double)vSync.Numerator / vSync.Denominator;

        double modeDiff = abs(vSyncDouble - refreshRate);
        if (modeDiff < bestModeDiff)
        {
            bestMode = mode;
            bestModeDiff = modeDiff;
        }
    }

    if (bestMode)
    {
        path.ApplyPropertiesFromMode(bestMode);
    }
    else
    {
        return ProcessFailure(m_Device, L"Failed to set refresh rate in OUTPUTMANAGER", L"Error", E_UNEXPECTED);
    }

    auto applyResult = state.TryApply(winrt::DisplayStateApplyOptions::None);
    hr = applyResult.ExtendedErrorCode();
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to apply mode in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Re-read the current state to see the final state that was applied (with all properties).
    stateResult = m_DisplayManager.TryAcquireTargetsAndReadCurrentState(myTargets);
    hr = stateResult.ExtendedErrorCode();
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to acquire target in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }
    state = stateResult.State();

    m_DisplayTarget = std::move(target);
    m_DisplayDevice = std::move(m_DisplayManager.CreateDisplayDevice(m_DisplayTarget.Adapter()));
    m_DisplayTaskPool = m_DisplayDevice.CreateTaskPool();

    // Create the D3D device we will use for this output.
    auto adapterLuid = m_DisplayTarget.Adapter().Id();

    {
        com_ptr<IDXGIFactory6> dxgiFactory;
        hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory6), (void**)dxgiFactory.put());
        if (FAILED(hr))
        {
            return ProcessFailure(m_Device, L"Failed to create DXGI Factory in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
        }
        com_ptr<IDXGIAdapter> dxgiAdapter;
        hr = dxgiFactory->EnumAdapterByLuid({ adapterLuid.LowPart, adapterLuid.HighPart }, __uuidof(IDXGIAdapter), (void**)dxgiAdapter.put());
        if (FAILED(hr))
        {
            return ProcessFailure(m_Device, L"Failed to find adapter in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
        }
        com_ptr<ID3D11Device> device;
        com_ptr<ID3D11DeviceContext> deviceContext;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        hr = D3D11CreateDevice(dxgiAdapter.get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            creationFlags,
            &featureLevel,
            1,
            D3D11_SDK_VERSION,
            device.put(),
            nullptr,
            deviceContext.put());
        if (FAILED(hr))
        {
            return ProcessFailure(m_Device, L"Failed to create D3D Device in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
        }

        // Boost the device's priority.
        auto dxgiDevice = device.as<IDXGIDevice>();
        {
            dxgiDevice->SetGPUThreadPriority(7);
        }

        device->QueryInterface(__uuidof(ID3D11Device5), (void**)&m_Device);
        deviceContext->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&m_DeviceContext);
    }

    auto deviceInterop = m_DisplayDevice.as<IDisplayDeviceInterop>();

    // Create the output surfaces.
    {
        auto path = state.GetPathForTarget(m_DisplayTarget);
        m_DisplaySource = m_DisplayDevice.CreateScanoutSource(m_DisplayTarget);

        const winrt::SizeInt32 sourceResolution = path.SourceResolution().Value();
        m_DisplayWidth = sourceResolution.Width;
        m_DisplayHeight = sourceResolution.Height;
        winrt::Direct3D11::Direct3DMultisampleDescription multisampleDesc = {};
        multisampleDesc.Count = 1;

        winrt::DisplayPrimaryDescription primaryDesc{
            static_cast<uint32_t>(sourceResolution.Width),
            static_cast<uint32_t>(sourceResolution.Height),
            winrt::DirectXPixelFormat::B8G8R8A8UIntNormalizedSrgb,
            winrt::DirectXColorSpace::RgbFullG22NoneP709,
            false,
            multisampleDesc
        };

        for (uint32_t i = 0; i < 2; i++)
        {
            OutputSurface surface{};

            surface.primary = m_DisplayDevice.CreatePrimary(m_DisplayTarget, primaryDesc);

            // Immediately after chaging the refresh rate, we must retry until success.
            if (i == 0)
            {
                for (uint32_t retry = 0; retry < 2; retry++) {
                    try
                    {
                        surface.scanout = m_DisplayDevice.CreateSimpleScanout(m_DisplaySource, surface.primary, 0, 1);
                        break;
                    }
                    catch (winrt::hresult_error&)
                    {
                        Sleep(500);
                    }
                }
            }
            if (!surface.scanout)
            {
                surface.scanout = m_DisplayDevice.CreateSimpleScanout(m_DisplaySource, surface.primary, 0, 1);
            }

            // Open on the presentation device.
            auto surfaceRaw = surface.primary.as<::IInspectable>();
            winrt::handle handle;
            hr = deviceInterop->CreateSharedHandle(surfaceRaw.get(), nullptr, GENERIC_ALL, nullptr, handle.put());
            if (FAILED(hr))
            {
                return ProcessFailure(m_Device, L"Failed to create shared surface handle in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
            }
            hr = m_Device->OpenSharedResource1(handle.get(), __uuidof(ID3D11Texture2D), (void**)surface.surface.put());
            if (FAILED(hr))
            {
                return ProcessFailure(m_Device, L"Failed to open shared surface handle in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
            }

            m_OutputSurfaces.push_back(std::move(surface));
        }
    }

    // Create a fence to signal completion of LSR before we can trigger scanout.
    {
        hr = m_Device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)m_DisplayFenceOnPresentationDevice.put());
        if (FAILED(hr))
        {
            return ProcessFailure(m_Device, L"Failed to create shared fence in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
        }

        winrt::handle handle;
        hr = m_DisplayFenceOnPresentationDevice->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, handle.put());
        if (FAILED(hr))
        {
            return ProcessFailure(m_Device, L"Failed to create shared fence handle in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
        }

        winrt::com_ptr<::IInspectable> displayFence;
        hr = deviceInterop->OpenSharedHandle(handle.get(), __uuidof(::IInspectable), (void**)displayFence.put());
        if (FAILED(hr))
        {
            return ProcessFailure(m_Device, L"Failed to open shared fence handle in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
        }

        m_DisplayFenceOnDisplayDevice = displayFence.as<winrt::DisplayFence>();
    }

    // Create a fence to wake up the presentation thread to perform LSR just before scanout.
    {
        m_VBlankFenceOnDisplayDevice = m_DisplayDevice.CreatePeriodicFence(m_DisplayTarget, std::chrono::milliseconds(OffsetFromVBlank / 1'000'000));

        winrt::handle handle;
        hr = deviceInterop->CreateSharedHandle(m_VBlankFenceOnDisplayDevice.as<::IInspectable>().get(), nullptr, GENERIC_ALL,nullptr, handle.put());
        if (FAILED(hr))
        {
            return ProcessFailure(m_Device, L"Failed to create shared fence handle in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
        }

        hr = m_Device->OpenSharedFence(handle.get(), __uuidof(ID3D11Fence), (void**)m_VBlankFenceOnPresentationDevice.put());
        if (FAILED(hr))
        {
            return ProcessFailure(m_Device, L"Failed to open shared fence handle in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
        }

        m_VBlankEvent.attach(CreateEventEx(nullptr, L"VBlank Fence", 0, EVENT_ALL_ACCESS));
    }

    return DUPL_RETURN_SUCCESS;
}

//
// Recreate shared texture
//
DUPL_RETURN OUTPUTMANAGER::CreateSharedSurf(INT SingleOutput, _Out_ UINT* OutCount, _Out_ RECT* DeskBounds)
{
    HRESULT hr;

    // Get DXGI resources
    IDXGIDevice* DxgiDevice = nullptr;
    hr = m_Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&DxgiDevice));
    if (FAILED(hr))
    {
        return ProcessFailure(nullptr, L"Failed to QI for DXGI Device", L"Error", hr);
    }

    IDXGIAdapter* DxgiAdapter = nullptr;
    hr = DxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&DxgiAdapter));
    DxgiDevice->Release();
    DxgiDevice = nullptr;
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to get parent DXGI Adapter", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Set initial values so that we always catch the right coordinates
    DeskBounds->left = INT_MAX;
    DeskBounds->right = INT_MIN;
    DeskBounds->top = INT_MAX;
    DeskBounds->bottom = INT_MIN;

    IDXGIOutput* DxgiOutput = nullptr;

    // Figure out right dimensions for full size desktop texture and # of outputs to duplicate
    UINT OutputCount;
    if (SingleOutput < 0)
    {
        hr = S_OK;
        for (OutputCount = 0; SUCCEEDED(hr); ++OutputCount)
        {
            if (DxgiOutput)
            {
                DxgiOutput->Release();
                DxgiOutput = nullptr;
            }
            hr = DxgiAdapter->EnumOutputs(OutputCount, &DxgiOutput);
            if (DxgiOutput && (hr != DXGI_ERROR_NOT_FOUND))
            {
                DXGI_OUTPUT_DESC DesktopDesc;
                DxgiOutput->GetDesc(&DesktopDesc);

                DeskBounds->left = min(DesktopDesc.DesktopCoordinates.left, DeskBounds->left);
                DeskBounds->top = min(DesktopDesc.DesktopCoordinates.top, DeskBounds->top);
                DeskBounds->right = max(DesktopDesc.DesktopCoordinates.right, DeskBounds->right);
                DeskBounds->bottom = max(DesktopDesc.DesktopCoordinates.bottom, DeskBounds->bottom);
            }
        }

        --OutputCount;
    }
    else
    {
        hr = DxgiAdapter->EnumOutputs(SingleOutput, &DxgiOutput);
        if (FAILED(hr))
        {
            DxgiAdapter->Release();
            DxgiAdapter = nullptr;
            return ProcessFailure(m_Device, L"Output specified to be duplicated does not exist", L"Error", hr);
        }
        DXGI_OUTPUT_DESC DesktopDesc;
        DxgiOutput->GetDesc(&DesktopDesc);
        *DeskBounds = DesktopDesc.DesktopCoordinates;

        DxgiOutput->Release();
        DxgiOutput = nullptr;

        OutputCount = 1;
    }

    DxgiAdapter->Release();
    DxgiAdapter = nullptr;

    // Set passed in output count variable
    *OutCount = OutputCount;

    if (OutputCount == 0)
    {
        // We could not find any outputs, the system must be in a transition so return expected error
        // so we will attempt to recreate
        return DUPL_RETURN_ERROR_EXPECTED;
    }

    // Create shared texture for all duplication threads to draw into
    D3D11_TEXTURE2D_DESC DeskTexD;
    RtlZeroMemory(&DeskTexD, sizeof(D3D11_TEXTURE2D_DESC));
    DeskTexD.Width = DeskBounds->right - DeskBounds->left;
    DeskTexD.Height = DeskBounds->bottom - DeskBounds->top;
    DeskTexD.MipLevels = 1;
    DeskTexD.ArraySize = 1;
    DeskTexD.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    DeskTexD.SampleDesc.Count = 1;
    DeskTexD.Usage = D3D11_USAGE_DEFAULT;
    DeskTexD.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    DeskTexD.CPUAccessFlags = 0;
    DeskTexD.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    hr = m_Device->CreateTexture2D(&DeskTexD, nullptr, &m_SharedSurf);
    if (FAILED(hr))
    {
        if (OutputCount != 1)
        {
            // If we are duplicating the complete desktop we try to create a single texture to hold the
            // complete desktop image and blit updates from the per output DDA interface.  The GPU can
            // always support a texture size of the maximum resolution of any single output but there is no
            // guarantee that it can support a texture size of the desktop.
            // The sample only use this large texture to display the desktop image in a single window using DX
            // we could revert back to using GDI to update the window in this failure case.
            return ProcessFailure(m_Device, L"Failed to create DirectX shared texture - we are attempting to create a texture the size of the complete desktop and this may be larger than the maximum texture size of your GPU.  Please try again using the -output command line parameter to duplicate only 1 monitor or configure your computer to a single monitor configuration", L"Error", hr, SystemTransitionsExpectedErrors);
        }
        else
        {
            return ProcessFailure(m_Device, L"Failed to create shared texture", L"Error", hr, SystemTransitionsExpectedErrors);
        }
    }

    // Get keyed mutex
    hr = m_SharedSurf->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&m_KeyMutex));
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to query for keyed mutex in OUTPUTMANAGER", L"Error", hr);
    }

    return DUPL_RETURN_SUCCESS;
}

//
// Present to the application window
//
DUPL_RETURN OUTPUTMANAGER::UpdateApplicationWindow(_In_ PTR_INFO* PointerInfo)
{
    // In a typical desktop duplication application there would be an application running on one system collecting the desktop images
    // and another application running on a different system that receives the desktop images via a network and display the image. This
    // sample contains both these aspects into a single application.
    // This routine is the part of the sample that displays the desktop image onto the display

    // Try and acquire sync on common display buffer
    HRESULT hr = m_KeyMutex->AcquireSync(1, 100);
    if (hr == static_cast<HRESULT>(WAIT_TIMEOUT))
    {
        // Another thread has the keyed mutex so try again later
        return DUPL_RETURN_SUCCESS;
    }
    else if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to acquire Keyed mutex in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Got mutex, so draw
    DUPL_RETURN Ret = DrawFrame();
    if (Ret == DUPL_RETURN_SUCCESS)
    {
        // We have keyed mutex so we can access the mouse info
        if (PointerInfo->Visible)
        {
            // Draw mouse into texture
            Ret = DrawMouse(PointerInfo);
        }
    }

    // Release keyed mutex
    hr = m_KeyMutex->ReleaseSync(0);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to Release Keyed mutex in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Present to window if all worked
    if (Ret == DUPL_RETURN_SUCCESS)
    {
        Ret = Present();
    }

    return Ret;
}

//
// Schedule scanout of the frame.
//
DUPL_RETURN OUTPUTMANAGER::Present() {
    ++m_DisplayFenceValue;
    HRESULT hr = m_DeviceContext->Signal(m_DisplayFenceOnPresentationDevice.get(), m_DisplayFenceValue);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to signal fence in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    m_DeviceContext->Flush();

    winrt::DisplayTask task = m_DisplayTaskPool.CreateTask();
    task.SetScanout(m_OutputSurfaces[m_OutputSurfaceIndex].scanout);
    task.SetWait(m_DisplayFenceOnDisplayDevice, m_DisplayFenceValue);

    m_DisplayTaskPool.ExecuteTask(task);

    // Switch backbuffer.
    m_OutputSurfaceIndex ^= 1;

    return DUPL_RETURN_SUCCESS;
}

//
// Wait for the next v-blank event.
//
DUPL_RETURN OUTPUTMANAGER::WaitNextVBlank() {
    HRESULT hr = m_VBlankFenceOnPresentationDevice->SetEventOnCompletion(m_VBlankFenceValue, m_VBlankEvent.get());
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to set fence event in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    DWORD waited = WaitForSingleObject(m_VBlankEvent.get(), 200);
    if (waited != WAIT_OBJECT_0)
    {
        return ProcessFailure(m_Device, L"Failed to wait for fence in OUTPUTMANAGER", L"Error", E_UNEXPECTED);
    }

    // When using a real display output, assume the fence increases monotonically (per
    // CreatePeriodicFence()).
    m_VBlankFenceValue = m_VBlankFenceOnPresentationDevice->GetCompletedValue() + 1;

    return DUPL_RETURN_SUCCESS;
}

//
// Returns shared handle
//
HANDLE OUTPUTMANAGER::GetSharedHandle()
{
    HANDLE Hnd = nullptr;

    // QI IDXGIResource interface to synchronized shared surface.
    IDXGIResource* DXGIResource = nullptr;
    HRESULT hr = m_SharedSurf->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&DXGIResource));
    if (SUCCEEDED(hr))
    {
        // Obtain handle to IDXGIResource object.
        DXGIResource->GetSharedHandle(&Hnd);
        DXGIResource->Release();
        DXGIResource = nullptr;
    }

    return Hnd;
}

//
// Draw frame into backbuffer
//
DUPL_RETURN OUTPUTMANAGER::DrawFrame()
{
    HRESULT hr;

    // Vertices for drawing whole texture
    VERTEX Vertices[NUMVERTICES] =
    {
        {XMFLOAT3(-1.0f, -1.0f, 0), XMFLOAT2(0.0f, 1.0f)},
        {XMFLOAT3(-1.0f, 1.0f, 0), XMFLOAT2(0.0f, 0.0f)},
        {XMFLOAT3(1.0f, -1.0f, 0), XMFLOAT2(1.0f, 1.0f)},
        {XMFLOAT3(1.0f, -1.0f, 0), XMFLOAT2(1.0f, 1.0f)},
        {XMFLOAT3(-1.0f, 1.0f, 0), XMFLOAT2(0.0f, 0.0f)},
        {XMFLOAT3(1.0f, 1.0f, 0), XMFLOAT2(1.0f, 0.0f)},
    };

    D3D11_TEXTURE2D_DESC FrameDesc;
    m_SharedSurf->GetDesc(&FrameDesc);

    D3D11_SHADER_RESOURCE_VIEW_DESC ShaderDesc;
    ShaderDesc.Format = FrameDesc.Format;
    ShaderDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    ShaderDesc.Texture2D.MostDetailedMip = FrameDesc.MipLevels - 1;
    ShaderDesc.Texture2D.MipLevels = FrameDesc.MipLevels;

    // Create new shader resource view
    ID3D11ShaderResourceView* ShaderResource = nullptr;
    hr = m_Device->CreateShaderResourceView(m_SharedSurf, &ShaderDesc, &ShaderResource);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create shader resource when drawing a frame", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Set resources
    UINT Stride = sizeof(VERTEX);
    UINT Offset = 0;
    FLOAT blendFactor[4] = {0.f, 0.f, 0.f, 0.f};
    m_DeviceContext->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    m_DeviceContext->OMSetRenderTargets(1, &m_RTV, nullptr);
    m_DeviceContext->VSSetShader(m_VertexShader, nullptr, 0);
    m_DeviceContext->PSSetShader(m_PixelShader, nullptr, 0);
    m_DeviceContext->PSSetShaderResources(0, 1, &ShaderResource);
    m_DeviceContext->PSSetSamplers(0, 1, &m_SamplerLinear);
    m_DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D11_BUFFER_DESC BufferDesc;
    RtlZeroMemory(&BufferDesc, sizeof(BufferDesc));
    BufferDesc.Usage = D3D11_USAGE_DEFAULT;
    BufferDesc.ByteWidth = sizeof(VERTEX) * NUMVERTICES;
    BufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    BufferDesc.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA InitData;
    RtlZeroMemory(&InitData, sizeof(InitData));
    InitData.pSysMem = Vertices;

    ID3D11Buffer* VertexBuffer = nullptr;

    // Create vertex buffer
    hr = m_Device->CreateBuffer(&BufferDesc, &InitData, &VertexBuffer);
    if (FAILED(hr))
    {
        ShaderResource->Release();
        ShaderResource = nullptr;
        return ProcessFailure(m_Device, L"Failed to create vertex buffer when drawing a frame", L"Error", hr, SystemTransitionsExpectedErrors);
    }
    m_DeviceContext->IASetVertexBuffers(0, 1, &VertexBuffer, &Stride, &Offset);

    // Draw textured quad onto render target
    m_DeviceContext->Draw(NUMVERTICES, 0);

    VertexBuffer->Release();
    VertexBuffer = nullptr;

    // Release shader resource
    ShaderResource->Release();
    ShaderResource = nullptr;

    return DUPL_RETURN_SUCCESS;
}

//
// Process both masked and monochrome pointers
//
DUPL_RETURN OUTPUTMANAGER::ProcessMonoMask(bool IsMono, _Inout_ PTR_INFO* PtrInfo, _Out_ INT* PtrWidth, _Out_ INT* PtrHeight, _Out_ INT* PtrLeft, _Out_ INT* PtrTop, _Outptr_result_bytebuffer_(*PtrHeight * *PtrWidth * BPP) BYTE** InitBuffer, _Out_ D3D11_BOX* Box)
{
    // Desktop dimensions
    D3D11_TEXTURE2D_DESC FullDesc;
    m_SharedSurf->GetDesc(&FullDesc);
    INT DesktopWidth = FullDesc.Width;
    INT DesktopHeight = FullDesc.Height;

    // Pointer position
    INT GivenLeft = PtrInfo->Position.x;
    INT GivenTop = PtrInfo->Position.y;

    // Figure out if any adjustment is needed for out of bound positions
    if (GivenLeft < 0)
    {
        *PtrWidth = GivenLeft + static_cast<INT>(PtrInfo->ShapeInfo.Width);
    }
    else if ((GivenLeft + static_cast<INT>(PtrInfo->ShapeInfo.Width)) > DesktopWidth)
    {
        *PtrWidth = DesktopWidth - GivenLeft;
    }
    else
    {
        *PtrWidth = static_cast<INT>(PtrInfo->ShapeInfo.Width);
    }

    if (IsMono)
    {
        PtrInfo->ShapeInfo.Height = PtrInfo->ShapeInfo.Height / 2;
    }

    if (GivenTop < 0)
    {
        *PtrHeight = GivenTop + static_cast<INT>(PtrInfo->ShapeInfo.Height);
    }
    else if ((GivenTop + static_cast<INT>(PtrInfo->ShapeInfo.Height)) > DesktopHeight)
    {
        *PtrHeight = DesktopHeight - GivenTop;
    }
    else
    {
        *PtrHeight = static_cast<INT>(PtrInfo->ShapeInfo.Height);
    }

    if (IsMono)
    {
        PtrInfo->ShapeInfo.Height = PtrInfo->ShapeInfo.Height * 2;
    }

    *PtrLeft = (GivenLeft < 0) ? 0 : GivenLeft;
    *PtrTop = (GivenTop < 0) ? 0 : GivenTop;

    // Staging buffer/texture
    D3D11_TEXTURE2D_DESC CopyBufferDesc;
    CopyBufferDesc.Width = *PtrWidth;
    CopyBufferDesc.Height = *PtrHeight;
    CopyBufferDesc.MipLevels = 1;
    CopyBufferDesc.ArraySize = 1;
    CopyBufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    CopyBufferDesc.SampleDesc.Count = 1;
    CopyBufferDesc.SampleDesc.Quality = 0;
    CopyBufferDesc.Usage = D3D11_USAGE_STAGING;
    CopyBufferDesc.BindFlags = 0;
    CopyBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CopyBufferDesc.MiscFlags = 0;

    ID3D11Texture2D* CopyBuffer = nullptr;
    HRESULT hr = m_Device->CreateTexture2D(&CopyBufferDesc, nullptr, &CopyBuffer);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed creating staging texture for pointer", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Copy needed part of desktop image
    Box->left = *PtrLeft;
    Box->top = *PtrTop;
    Box->right = *PtrLeft + *PtrWidth;
    Box->bottom = *PtrTop + *PtrHeight;
    m_DeviceContext->CopySubresourceRegion(CopyBuffer, 0, 0, 0, 0, m_SharedSurf, 0, Box);

    // QI for IDXGISurface
    IDXGISurface* CopySurface = nullptr;
    hr = CopyBuffer->QueryInterface(__uuidof(IDXGISurface), (void **)&CopySurface);
    CopyBuffer->Release();
    CopyBuffer = nullptr;
    if (FAILED(hr))
    {
        return ProcessFailure(nullptr, L"Failed to QI staging texture into IDXGISurface for pointer", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Map pixels
    DXGI_MAPPED_RECT MappedSurface;
    hr = CopySurface->Map(&MappedSurface, DXGI_MAP_READ);
    if (FAILED(hr))
    {
        CopySurface->Release();
        CopySurface = nullptr;
        return ProcessFailure(m_Device, L"Failed to map surface for pointer", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // New mouseshape buffer
    *InitBuffer = new (std::nothrow) BYTE[*PtrWidth * *PtrHeight * BPP];
    if (!(*InitBuffer))
    {
        return ProcessFailure(nullptr, L"Failed to allocate memory for new mouse shape buffer.", L"Error", E_OUTOFMEMORY);
    }

    UINT* InitBuffer32 = reinterpret_cast<UINT*>(*InitBuffer);
    UINT* Desktop32 = reinterpret_cast<UINT*>(MappedSurface.pBits);
    UINT  DesktopPitchInPixels = MappedSurface.Pitch / sizeof(UINT);

    // What to skip (pixel offset)
    UINT SkipX = (GivenLeft < 0) ? (-1 * GivenLeft) : (0);
    UINT SkipY = (GivenTop < 0) ? (-1 * GivenTop) : (0);

    if (IsMono)
    {
        for (INT Row = 0; Row < *PtrHeight; ++Row)
        {
            // Set mask
            BYTE Mask = 0x80;
            Mask = Mask >> (SkipX % 8);
            for (INT Col = 0; Col < *PtrWidth; ++Col)
            {
                // Get masks using appropriate offsets
                BYTE AndMask = PtrInfo->PtrShapeBuffer[((Col + SkipX) / 8) + ((Row + SkipY) * (PtrInfo->ShapeInfo.Pitch))] & Mask;
                BYTE XorMask = PtrInfo->PtrShapeBuffer[((Col + SkipX) / 8) + ((Row + SkipY + (PtrInfo->ShapeInfo.Height / 2)) * (PtrInfo->ShapeInfo.Pitch))] & Mask;
                UINT AndMask32 = (AndMask) ? 0xFFFFFFFF : 0xFF000000;
                UINT XorMask32 = (XorMask) ? 0x00FFFFFF : 0x00000000;

                // Set new pixel
                InitBuffer32[(Row * *PtrWidth) + Col] = (Desktop32[(Row * DesktopPitchInPixels) + Col] & AndMask32) ^ XorMask32;

                // Adjust mask
                if (Mask == 0x01)
                {
                    Mask = 0x80;
                }
                else
                {
                    Mask = Mask >> 1;
                }
            }
        }
    }
    else
    {
        UINT* Buffer32 = reinterpret_cast<UINT*>(PtrInfo->PtrShapeBuffer);

        // Iterate through pixels
        for (INT Row = 0; Row < *PtrHeight; ++Row)
        {
            for (INT Col = 0; Col < *PtrWidth; ++Col)
            {
                // Set up mask
                UINT MaskVal = 0xFF000000 & Buffer32[(Col + SkipX) + ((Row + SkipY) * (PtrInfo->ShapeInfo.Pitch / sizeof(UINT)))];
                if (MaskVal)
                {
                    // Mask was 0xFF
                    InitBuffer32[(Row * *PtrWidth) + Col] = (Desktop32[(Row * DesktopPitchInPixels) + Col] ^ Buffer32[(Col + SkipX) + ((Row + SkipY) * (PtrInfo->ShapeInfo.Pitch / sizeof(UINT)))]) | 0xFF000000;
                }
                else
                {
                    // Mask was 0x00
                    InitBuffer32[(Row * *PtrWidth) + Col] = Buffer32[(Col + SkipX) + ((Row + SkipY) * (PtrInfo->ShapeInfo.Pitch / sizeof(UINT)))] | 0xFF000000;
                }
            }
        }
    }

    // Done with resource
    hr = CopySurface->Unmap();
    CopySurface->Release();
    CopySurface = nullptr;
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to unmap surface for pointer", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    return DUPL_RETURN_SUCCESS;
}

//
// Draw mouse provided in buffer to backbuffer
//
DUPL_RETURN OUTPUTMANAGER::DrawMouse(_In_ PTR_INFO* PtrInfo)
{
    // Vars to be used
    ID3D11Texture2D* MouseTex = nullptr;
    ID3D11ShaderResourceView* ShaderRes = nullptr;
    ID3D11Buffer* VertexBufferMouse = nullptr;
    D3D11_SUBRESOURCE_DATA InitData;
    D3D11_TEXTURE2D_DESC Desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC SDesc;

    // Position will be changed based on mouse position
    VERTEX Vertices[NUMVERTICES] =
    {
        {XMFLOAT3(-1.0f, -1.0f, 0), XMFLOAT2(0.0f, 1.0f)},
        {XMFLOAT3(-1.0f, 1.0f, 0), XMFLOAT2(0.0f, 0.0f)},
        {XMFLOAT3(1.0f, -1.0f, 0), XMFLOAT2(1.0f, 1.0f)},
        {XMFLOAT3(1.0f, -1.0f, 0), XMFLOAT2(1.0f, 1.0f)},
        {XMFLOAT3(-1.0f, 1.0f, 0), XMFLOAT2(0.0f, 0.0f)},
        {XMFLOAT3(1.0f, 1.0f, 0), XMFLOAT2(1.0f, 0.0f)},
    };

    D3D11_TEXTURE2D_DESC FullDesc;
    m_SharedSurf->GetDesc(&FullDesc);
    INT DesktopWidth = FullDesc.Width;
    INT DesktopHeight = FullDesc.Height;

    // Center of desktop dimensions
    INT CenterX = (DesktopWidth / 2);
    INT CenterY = (DesktopHeight / 2);

    // Clipping adjusted coordinates / dimensions
    INT PtrWidth = 0;
    INT PtrHeight = 0;
    INT PtrLeft = 0;
    INT PtrTop = 0;

    // Buffer used if necessary (in case of monochrome or masked pointer)
    BYTE* InitBuffer = nullptr;

    // Used for copying pixels
    D3D11_BOX Box;
    Box.front = 0;
    Box.back = 1;

    Desc.MipLevels = 1;
    Desc.ArraySize = 1;
    Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Desc.SampleDesc.Count = 1;
    Desc.SampleDesc.Quality = 0;
    Desc.Usage = D3D11_USAGE_DEFAULT;
    Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Desc.CPUAccessFlags = 0;
    Desc.MiscFlags = 0;

    // Set shader resource properties
    SDesc.Format = Desc.Format;
    SDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    SDesc.Texture2D.MostDetailedMip = Desc.MipLevels - 1;
    SDesc.Texture2D.MipLevels = Desc.MipLevels;

    switch (PtrInfo->ShapeInfo.Type)
    {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        {
            PtrLeft = PtrInfo->Position.x;
            PtrTop = PtrInfo->Position.y;

            PtrWidth = static_cast<INT>(PtrInfo->ShapeInfo.Width);
            PtrHeight = static_cast<INT>(PtrInfo->ShapeInfo.Height);

            break;
        }

        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        {
            ProcessMonoMask(true, PtrInfo, &PtrWidth, &PtrHeight, &PtrLeft, &PtrTop, &InitBuffer, &Box);
            break;
        }

        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        {
            ProcessMonoMask(false, PtrInfo, &PtrWidth, &PtrHeight, &PtrLeft, &PtrTop, &InitBuffer, &Box);
            break;
        }

        default:
            break;
    }

    // VERTEX creation
    Vertices[0].Pos.x = (PtrLeft - CenterX) / (FLOAT)CenterX;
    Vertices[0].Pos.y = -1 * ((PtrTop + PtrHeight) - CenterY) / (FLOAT)CenterY;
    Vertices[1].Pos.x = (PtrLeft - CenterX) / (FLOAT)CenterX;
    Vertices[1].Pos.y = -1 * (PtrTop - CenterY) / (FLOAT)CenterY;
    Vertices[2].Pos.x = ((PtrLeft + PtrWidth) - CenterX) / (FLOAT)CenterX;
    Vertices[2].Pos.y = -1 * ((PtrTop + PtrHeight) - CenterY) / (FLOAT)CenterY;
    Vertices[3].Pos.x = Vertices[2].Pos.x;
    Vertices[3].Pos.y = Vertices[2].Pos.y;
    Vertices[4].Pos.x = Vertices[1].Pos.x;
    Vertices[4].Pos.y = Vertices[1].Pos.y;
    Vertices[5].Pos.x = ((PtrLeft + PtrWidth) - CenterX) / (FLOAT)CenterX;
    Vertices[5].Pos.y = -1 * (PtrTop - CenterY) / (FLOAT)CenterY;

    // Set texture properties
    Desc.Width = PtrWidth;
    Desc.Height = PtrHeight;

    // Set up init data
    InitData.pSysMem = (PtrInfo->ShapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) ? PtrInfo->PtrShapeBuffer : InitBuffer;
    InitData.SysMemPitch = (PtrInfo->ShapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) ? PtrInfo->ShapeInfo.Pitch : PtrWidth * BPP;
    InitData.SysMemSlicePitch = 0;

    // Create mouseshape as texture
    HRESULT hr = m_Device->CreateTexture2D(&Desc, &InitData, &MouseTex);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create mouse pointer texture", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Create shader resource from texture
    hr = m_Device->CreateShaderResourceView(MouseTex, &SDesc, &ShaderRes);
    if (FAILED(hr))
    {
        MouseTex->Release();
        MouseTex = nullptr;
        return ProcessFailure(m_Device, L"Failed to create shader resource from mouse pointer texture", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    D3D11_BUFFER_DESC BDesc;
    ZeroMemory(&BDesc, sizeof(D3D11_BUFFER_DESC));
    BDesc.Usage = D3D11_USAGE_DEFAULT;
    BDesc.ByteWidth = sizeof(VERTEX) * NUMVERTICES;
    BDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    BDesc.CPUAccessFlags = 0;

    ZeroMemory(&InitData, sizeof(D3D11_SUBRESOURCE_DATA));
    InitData.pSysMem = Vertices;

    // Create vertex buffer
    hr = m_Device->CreateBuffer(&BDesc, &InitData, &VertexBufferMouse);
    if (FAILED(hr))
    {
        ShaderRes->Release();
        ShaderRes = nullptr;
        MouseTex->Release();
        MouseTex = nullptr;
        return ProcessFailure(m_Device, L"Failed to create mouse pointer vertex buffer in OutputManager", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Set resources
    FLOAT BlendFactor[4] = {0.f, 0.f, 0.f, 0.f};
    UINT Stride = sizeof(VERTEX);
    UINT Offset = 0;
    m_DeviceContext->IASetVertexBuffers(0, 1, &VertexBufferMouse, &Stride, &Offset);
    m_DeviceContext->OMSetBlendState(m_BlendState, BlendFactor, 0xFFFFFFFF);
    m_DeviceContext->OMSetRenderTargets(1, &m_RTV, nullptr);
    m_DeviceContext->VSSetShader(m_VertexShader, nullptr, 0);
    m_DeviceContext->PSSetShader(m_PixelShader, nullptr, 0);
    m_DeviceContext->PSSetShaderResources(0, 1, &ShaderRes);
    m_DeviceContext->PSSetSamplers(0, 1, &m_SamplerLinear);

    // Draw
    m_DeviceContext->Draw(NUMVERTICES, 0);

    // Clean
    if (VertexBufferMouse)
    {
        VertexBufferMouse->Release();
        VertexBufferMouse = nullptr;
    }
    if (ShaderRes)
    {
        ShaderRes->Release();
        ShaderRes = nullptr;
    }
    if (MouseTex)
    {
        MouseTex->Release();
        MouseTex = nullptr;
    }
    if (InitBuffer)
    {
        delete [] InitBuffer;
        InitBuffer = nullptr;
    }

    return DUPL_RETURN_SUCCESS;
}

//
// Initialize shaders for drawing to screen
//
DUPL_RETURN OUTPUTMANAGER::InitShaders()
{
    HRESULT hr;

    UINT Size = ARRAYSIZE(g_VS);
    hr = m_Device->CreateVertexShader(g_VS, Size, nullptr, &m_VertexShader);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create vertex shader in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    D3D11_INPUT_ELEMENT_DESC Layout[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    UINT NumElements = ARRAYSIZE(Layout);
    hr = m_Device->CreateInputLayout(Layout, NumElements, g_VS, Size, &m_InputLayout);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create input layout in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }
    m_DeviceContext->IASetInputLayout(m_InputLayout);

    Size = ARRAYSIZE(g_PS);
    hr = m_Device->CreatePixelShader(g_PS, Size, nullptr, &m_PixelShader);
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create pixel shader in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    return DUPL_RETURN_SUCCESS;
}

//
// Reset render target view
//
DUPL_RETURN OUTPUTMANAGER::MakeRTV()
{
    // Get backbuffer
    ID3D11Texture2D* BackBuffer = m_OutputSurfaces[m_OutputSurfaceIndex].surface.get();

    // Create a render target view
    HRESULT hr = m_Device->CreateRenderTargetView(BackBuffer, nullptr, &m_RTV);
    BackBuffer->Release();
    if (FAILED(hr))
    {
        return ProcessFailure(m_Device, L"Failed to create render target view in OUTPUTMANAGER", L"Error", hr, SystemTransitionsExpectedErrors);
    }

    // Set new render target
    m_DeviceContext->OMSetRenderTargets(1, &m_RTV, nullptr);

    return DUPL_RETURN_SUCCESS;
}

//
// Set new viewport
//
void OUTPUTMANAGER::SetViewPort(UINT Width, UINT Height)
{
    D3D11_VIEWPORT VP;
    VP.Width = static_cast<FLOAT>(Width);
    VP.Height = static_cast<FLOAT>(Height);
    VP.MinDepth = 0.0f;
    VP.MaxDepth = 1.0f;
    VP.TopLeftX = 0;
    VP.TopLeftY = 0;
    m_DeviceContext->RSSetViewports(1, &VP);
}

//
// Releases all references
//
void OUTPUTMANAGER::CleanRefs()
{
    if (m_VertexShader)
    {
        m_VertexShader->Release();
        m_VertexShader = nullptr;
    }

    if (m_PixelShader)
    {
        m_PixelShader->Release();
        m_PixelShader = nullptr;
    }

    if (m_InputLayout)
    {
        m_InputLayout->Release();
        m_InputLayout = nullptr;
    }

    if (m_RTV)
    {
        m_RTV->Release();
        m_RTV = nullptr;
    }

    if (m_SamplerLinear)
    {
        m_SamplerLinear->Release();
        m_SamplerLinear = nullptr;
    }

    if (m_BlendState)
    {
        m_BlendState->Release();
        m_BlendState = nullptr;
    }

    if (m_DeviceContext)
    {
        m_DeviceContext->Release();
        m_DeviceContext = nullptr;
    }

    if (m_Device)
    {
        m_Device->Release();
        m_Device = nullptr;
    }

    if (m_SharedSurf)
    {
        m_SharedSurf->Release();
        m_SharedSurf = nullptr;
    }

    if (m_KeyMutex)
    {
        m_KeyMutex->Release();
        m_KeyMutex = nullptr;
    }
}
