#include "features.h"
#include "../config.h"
#include "../logger.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstring>

// Real post-process anti-aliasing for The Run's D3D11 renderer.
//
// Frostbite exposes WorldRenderSettings::MultisampleCount, but the retail game
// ignores it (the upstream DE project already wired and tested that field). Do
// not ship a placebo "MSAA" knob. Instead hook IDXGISwapChain::Present and run a
// compact FXAA pass over the final back buffer.
//
// The hook is installed through the DXGI swap-chain vtable obtained from a tiny
// dummy D3D11 device. This works even though the game creates its D3D device
// before the ASI is loaded. D3D11 and D3DCompiler are resolved dynamically so
// the ASI does not gain a hard import dependency on either library.

namespace {
    typedef HRESULT (WINAPI *D3D11CreateDeviceAndSwapChainFn)(
        IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL*, UINT, UINT,
        const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
        ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

    typedef HRESULT (WINAPI *D3DCompileFn)(
        LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

    typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain*, UINT, UINT);

    PresentFn g_OriginalPresent = nullptr;
    bool g_HookInstalled = false;
    bool g_DisabledAfterError = false;
    bool g_LoggedActive = false;

    IDXGISwapChain* g_ActiveSwap = nullptr; // identity only; not AddRef'd
    ID3D11Device* g_Device = nullptr;
    ID3D11DeviceContext* g_Context = nullptr;
    ID3D11Texture2D* g_Scratch = nullptr;
    ID3D11ShaderResourceView* g_ScratchSrv = nullptr;
    ID3D11RenderTargetView* g_BackBufferRtv = nullptr;
    ID3D11VertexShader* g_Vs = nullptr;
    ID3D11PixelShader* g_Ps = nullptr;
    ID3D11SamplerState* g_Sampler = nullptr;
    ID3D11Buffer* g_ConstantBuffer = nullptr;
    ID3D11BlendState* g_BlendState = nullptr;
    ID3D11DepthStencilState* g_DepthState = nullptr;
    ID3D11RasterizerState* g_RasterState = nullptr;
    UINT g_Width = 0;
    UINT g_Height = 0;
    DXGI_FORMAT g_Format = DXGI_FORMAT_UNKNOWN;

    template <typename T>
    void SafeRelease(T*& p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }

    void ReleaseRenderResources() {
        SafeRelease(g_ScratchSrv);
        SafeRelease(g_Scratch);
        SafeRelease(g_BackBufferRtv);
        SafeRelease(g_Vs);
        SafeRelease(g_Ps);
        SafeRelease(g_Sampler);
        SafeRelease(g_ConstantBuffer);
        SafeRelease(g_BlendState);
        SafeRelease(g_DepthState);
        SafeRelease(g_RasterState);
        SafeRelease(g_Context);
        SafeRelease(g_Device);
        g_ActiveSwap = nullptr;
        g_Width = g_Height = 0;
        g_Format = DXGI_FORMAT_UNKNOWN;
    }

    D3DCompileFn ResolveCompiler() {
        const char* dlls[] = {
            "D3DCompiler_47.dll", "D3DCompiler_46.dll",
            "D3DCompiler_43.dll"
        };
        for (const char* name : dlls) {
            HMODULE m = GetModuleHandleA(name);
            if (!m) m = LoadLibraryA(name);
            if (!m) continue;
            auto fn = reinterpret_cast<D3DCompileFn>(GetProcAddress(m, "D3DCompile"));
            if (fn) return fn;
        }
        return nullptr;
    }

    bool CompileShader(D3DCompileFn compile, const char* source, const char* entry,
                       const char* profile, ID3DBlob** outBlob) {
        ID3DBlob* errors = nullptr;
        HRESULT hr = compile(source, std::strlen(source), "NFSTR-DE FXAA", nullptr,
                             nullptr, entry, profile,
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, outBlob, &errors);
        if (FAILED(hr)) {
            if (errors && errors->GetBufferPointer()) {
                Logger::Log("Anti-aliasing shader compile failed: %s",
                            static_cast<const char*>(errors->GetBufferPointer()));
            } else {
                Logger::Log("Anti-aliasing shader compile failed: HRESULT 0x%08X", static_cast<unsigned>(hr));
            }
        }
        SafeRelease(errors);
        return SUCCEEDED(hr);
    }

    const char* kShaderSource = R"HLSL(
Texture2D SceneTex : register(t0);
SamplerState LinearClamp : register(s0);

cbuffer FxaaConstants : register(b0)
{
    float2 InvSize;
    float SpanMax;
    float _Padding;
};

struct VSOut
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.UV = p;
    o.Position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float Luma(float3 rgb)
{
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float2 uv = i.UV;
    float3 rgbNW = SceneTex.SampleLevel(LinearClamp, uv + float2(-1.0, -1.0) * InvSize, 0).rgb;
    float3 rgbNE = SceneTex.SampleLevel(LinearClamp, uv + float2( 1.0, -1.0) * InvSize, 0).rgb;
    float3 rgbSW = SceneTex.SampleLevel(LinearClamp, uv + float2(-1.0,  1.0) * InvSize, 0).rgb;
    float3 rgbSE = SceneTex.SampleLevel(LinearClamp, uv + float2( 1.0,  1.0) * InvSize, 0).rgb;
    float4 center = SceneTex.SampleLevel(LinearClamp, uv, 0);

    float lumaNW = Luma(rgbNW);
    float lumaNE = Luma(rgbNE);
    float lumaSW = Luma(rgbSW);
    float lumaSE = Luma(rgbSE);
    float lumaM  = Luma(center.rgb);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    const float reduceMul = 1.0 / 8.0;
    const float reduceMin = 1.0 / 128.0;
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * reduceMul), reduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -SpanMax.xx, SpanMax.xx) * InvSize;

    float3 rgbA = 0.5 * (
        SceneTex.SampleLevel(LinearClamp, uv + dir * (1.0 / 3.0 - 0.5), 0).rgb +
        SceneTex.SampleLevel(LinearClamp, uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);

    float3 rgbB = rgbA * 0.5 + 0.25 * (
        SceneTex.SampleLevel(LinearClamp, uv + dir * -0.5, 0).rgb +
        SceneTex.SampleLevel(LinearClamp, uv + dir *  0.5, 0).rgb);

    float lumaB = Luma(rgbB);
    float3 result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    return float4(result, center.a);
}
)HLSL";

    bool CreatePipelineObjects() {
        D3DCompileFn compile = ResolveCompiler();
        if (!compile) {
            Logger::Log("Anti-aliasing disabled: no compatible D3DCompiler DLL was found.");
            return false;
        }

        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        if (!CompileShader(compile, kShaderSource, "VSMain", "vs_4_0", &vsBlob) ||
            !CompileShader(compile, kShaderSource, "PSMain", "ps_4_0", &psBlob)) {
            SafeRelease(vsBlob);
            SafeRelease(psBlob);
            return false;
        }

        HRESULT hr = g_Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_Vs);
        if (SUCCEEDED(hr)) {
            hr = g_Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_Ps);
        }
        SafeRelease(vsBlob);
        SafeRelease(psBlob);
        if (FAILED(hr)) {
            Logger::Log("Anti-aliasing disabled: shader creation failed (0x%08X).", static_cast<unsigned>(hr));
            return false;
        }

        D3D11_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MinLOD = 0;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        hr = g_Device->CreateSamplerState(&sampler, &g_Sampler);
        if (FAILED(hr)) return false;

        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = 16;
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = g_Device->CreateBuffer(&cb, nullptr, &g_ConstantBuffer);
        if (FAILED(hr)) return false;

        D3D11_BLEND_DESC blend = {};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = g_Device->CreateBlendState(&blend, &g_BlendState);
        if (FAILED(hr)) return false;

        D3D11_DEPTH_STENCIL_DESC depth = {};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        depth.StencilEnable = FALSE;
        hr = g_Device->CreateDepthStencilState(&depth, &g_DepthState);
        if (FAILED(hr)) return false;

        D3D11_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        hr = g_Device->CreateRasterizerState(&raster, &g_RasterState);
        return SUCCEEDED(hr);
    }

    bool RecreateBackBufferResources(IDXGISwapChain* swap) {
        ReleaseRenderResources();
        g_ActiveSwap = swap;

        HRESULT hr = swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_Device));
        if (FAILED(hr) || !g_Device) {
            Logger::Log("Anti-aliasing: IDXGISwapChain::GetDevice failed (0x%08X).", static_cast<unsigned>(hr));
            return false;
        }
        g_Device->GetImmediateContext(&g_Context);
        if (!g_Context) return false;

        ID3D11Texture2D* backBuffer = nullptr;
        hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
        if (FAILED(hr) || !backBuffer) return false;

        D3D11_TEXTURE2D_DESC desc = {};
        backBuffer->GetDesc(&desc);
        g_Width = desc.Width;
        g_Height = desc.Height;
        g_Format = desc.Format;

        hr = g_Device->CreateRenderTargetView(backBuffer, nullptr, &g_BackBufferRtv);
        if (FAILED(hr)) {
            SafeRelease(backBuffer);
            return false;
        }

        D3D11_TEXTURE2D_DESC scratchDesc = desc;
        scratchDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        scratchDesc.CPUAccessFlags = 0;
        scratchDesc.MiscFlags = 0;
        scratchDesc.Usage = D3D11_USAGE_DEFAULT;
        scratchDesc.SampleDesc.Count = 1;
        scratchDesc.SampleDesc.Quality = 0;
        scratchDesc.ArraySize = 1;
        scratchDesc.MipLevels = 1;

        // The final swap-chain buffer should already be single-sampled. If it is
        // not, CopyResource to a single-sampled texture is invalid; fail safely.
        if (desc.SampleDesc.Count != 1) {
            Logger::Log("Anti-aliasing: unexpected multisampled swap-chain buffer (%u samples); FXAA pass skipped.",
                        desc.SampleDesc.Count);
            SafeRelease(backBuffer);
            return false;
        }

        hr = g_Device->CreateTexture2D(&scratchDesc, nullptr, &g_Scratch);
        if (SUCCEEDED(hr)) hr = g_Device->CreateShaderResourceView(g_Scratch, nullptr, &g_ScratchSrv);
        SafeRelease(backBuffer);
        if (FAILED(hr)) {
            Logger::Log("Anti-aliasing: scratch texture/SRV creation failed (0x%08X, format %u).",
                        static_cast<unsigned>(hr), static_cast<unsigned>(desc.Format));
            return false;
        }

        if (!CreatePipelineObjects()) return false;

        Logger::Log("Anti-aliasing: D3D11 FXAA resources ready at %ux%u, format %u.",
                    g_Width, g_Height, static_cast<unsigned>(g_Format));
        return true;
    }

    bool BackBufferChanged(IDXGISwapChain* swap) {
        if (swap != g_ActiveSwap || !g_Device || !g_Context || !g_Scratch || !g_BackBufferRtv) return true;

        ID3D11Texture2D* backBuffer = nullptr;
        HRESULT hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
        if (FAILED(hr) || !backBuffer) return true;
        D3D11_TEXTURE2D_DESC desc = {};
        backBuffer->GetDesc(&desc);
        SafeRelease(backBuffer);
        return desc.Width != g_Width || desc.Height != g_Height || desc.Format != g_Format;
    }

    struct FxaaConstants {
        float invWidth;
        float invHeight;
        float spanMax;
        float padding;
    };

    void ApplyFxaa(IDXGISwapChain* swap) {
        if (g_DisabledAfterError || g_Config.AntiAliasing <= 0) return;

        if (BackBufferChanged(swap)) {
            if (!RecreateBackBufferResources(swap)) {
                ReleaseRenderResources();
                g_DisabledAfterError = true;
                Logger::Log("Anti-aliasing disabled for this run after D3D11 resource setup failure.");
                return;
            }
        }

        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || !backBuffer) {
            return;
        }

        // Preserve the pipeline state touched by the fullscreen pass. Present is
        // normally called after the frame is complete, but restoring it keeps the
        // hook friendly to overlays and to any post-present work the game performs.
        ID3D11RenderTargetView* oldRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* oldDsv = nullptr;
        g_Context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, &oldDsv);

        ID3D11BlendState* oldBlend = nullptr;
        FLOAT oldBlendFactor[4] = {};
        UINT oldSampleMask = 0;
        g_Context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);

        ID3D11DepthStencilState* oldDepth = nullptr;
        UINT oldStencilRef = 0;
        g_Context->OMGetDepthStencilState(&oldDepth, &oldStencilRef);

        ID3D11RasterizerState* oldRaster = nullptr;
        g_Context->RSGetState(&oldRaster);

        UINT oldViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        g_Context->RSGetViewports(&oldViewportCount, oldViewports);

        ID3D11InputLayout* oldLayout = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY oldTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        g_Context->IAGetInputLayout(&oldLayout);
        g_Context->IAGetPrimitiveTopology(&oldTopology);

        ID3D11VertexShader* oldVs = nullptr;
        ID3D11PixelShader* oldPs = nullptr;
        g_Context->VSGetShader(&oldVs, nullptr, nullptr);
        g_Context->PSGetShader(&oldPs, nullptr, nullptr);

        ID3D11ShaderResourceView* oldSrv = nullptr;
        ID3D11SamplerState* oldSampler = nullptr;
        ID3D11Buffer* oldCb = nullptr;
        g_Context->PSGetShaderResources(0, 1, &oldSrv);
        g_Context->PSGetSamplers(0, 1, &oldSampler);
        g_Context->PSGetConstantBuffers(0, 1, &oldCb);

        // Back buffer cannot be a CopyResource source while still bound as RTV.
        g_Context->OMSetRenderTargets(0, nullptr, nullptr);
        g_Context->CopyResource(g_Scratch, backBuffer);

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(g_Context->Map(g_ConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            FxaaConstants constants = {
                1.0f / static_cast<float>(g_Width),
                1.0f / static_cast<float>(g_Height),
                g_Config.AntiAliasing >= 2 ? 12.0f : 6.0f,
                0.0f
            };
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            g_Context->Unmap(g_ConstantBuffer, 0);
        }

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(g_Width);
        vp.Height = static_cast<float>(g_Height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;

        FLOAT blendFactor[4] = { 0, 0, 0, 0 };
        g_Context->OMSetRenderTargets(1, &g_BackBufferRtv, nullptr);
        g_Context->OMSetBlendState(g_BlendState, blendFactor, 0xFFFFFFFFu);
        g_Context->OMSetDepthStencilState(g_DepthState, 0);
        g_Context->RSSetState(g_RasterState);
        g_Context->RSSetViewports(1, &vp);
        g_Context->IASetInputLayout(nullptr);
        g_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_Context->VSSetShader(g_Vs, nullptr, 0);
        g_Context->PSSetShader(g_Ps, nullptr, 0);
        g_Context->PSSetShaderResources(0, 1, &g_ScratchSrv);
        g_Context->PSSetSamplers(0, 1, &g_Sampler);
        g_Context->PSSetConstantBuffers(0, 1, &g_ConstantBuffer);
        g_Context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrv = nullptr;
        g_Context->PSSetShaderResources(0, 1, &nullSrv);

        // Restore everything touched above.
        g_Context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, oldDsv);
        g_Context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
        g_Context->OMSetDepthStencilState(oldDepth, oldStencilRef);
        g_Context->RSSetState(oldRaster);
        if (oldViewportCount) g_Context->RSSetViewports(oldViewportCount, oldViewports);
        g_Context->IASetInputLayout(oldLayout);
        g_Context->IASetPrimitiveTopology(oldTopology);
        g_Context->VSSetShader(oldVs, nullptr, 0);
        g_Context->PSSetShader(oldPs, nullptr, 0);
        g_Context->PSSetShaderResources(0, 1, &oldSrv);
        g_Context->PSSetSamplers(0, 1, &oldSampler);
        g_Context->PSSetConstantBuffers(0, 1, &oldCb);

        for (auto*& rtv : oldRtvs) SafeRelease(rtv);
        SafeRelease(oldDsv);
        SafeRelease(oldBlend);
        SafeRelease(oldDepth);
        SafeRelease(oldRaster);
        SafeRelease(oldLayout);
        SafeRelease(oldVs);
        SafeRelease(oldPs);
        SafeRelease(oldSrv);
        SafeRelease(oldSampler);
        SafeRelease(oldCb);
        SafeRelease(backBuffer);

        if (!g_LoggedActive) {
            Logger::Log("Anti-aliasing active: post-process FXAA mode %d (native Frostbite MultisampleCount is inert in retail).",
                        g_Config.AntiAliasing);
            g_LoggedActive = true;
        }
    }

    HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swap, UINT syncInterval, UINT flags) {
        ApplyFxaa(swap);
        return g_OriginalPresent(swap, syncInterval, flags);
    }

    LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    bool InstallPresentHook() {
        HMODULE d3d11 = GetModuleHandleA("d3d11.dll");
        if (!d3d11) d3d11 = LoadLibraryA("d3d11.dll");
        if (!d3d11) return false;

        auto create = reinterpret_cast<D3D11CreateDeviceAndSwapChainFn>(
            GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
        if (!create) return false;

        const char* className = "NFSTRDE_FXAA_DummyWindow";
        WNDCLASSA wc = {};
        wc.lpfnWndProc = DummyWndProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = className;
        RegisterClassA(&wc);

        HWND hwnd = CreateWindowExA(0, className, className, WS_OVERLAPPED,
                                    0, 0, 32, 32, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return false;

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferDesc.Width = 32;
        sd.BufferDesc.Height = 32;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.OutputWindow = hwnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL requested[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_10_0;
        IDXGISwapChain* dummySwap = nullptr;
        ID3D11Device* dummyDevice = nullptr;
        ID3D11DeviceContext* dummyContext = nullptr;

        HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                            requested, sizeof(requested) / sizeof(requested[0]),
                            D3D11_SDK_VERSION, &sd, &dummySwap, &dummyDevice,
                            &obtained, &dummyContext);
        if (FAILED(hr)) {
            hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                        requested, sizeof(requested) / sizeof(requested[0]),
                        D3D11_SDK_VERSION, &sd, &dummySwap, &dummyDevice,
                        &obtained, &dummyContext);
        }

        if (FAILED(hr) || !dummySwap) {
            SafeRelease(dummyContext);
            SafeRelease(dummyDevice);
            SafeRelease(dummySwap);
            DestroyWindow(hwnd);
            UnregisterClassA(className, wc.hInstance);
            Logger::Log("Anti-aliasing: could not create the dummy D3D11 swap chain (0x%08X).", static_cast<unsigned>(hr));
            return false;
        }

        void** vtable = *reinterpret_cast<void***>(dummySwap);
        constexpr size_t kPresentIndex = 8;
        g_OriginalPresent = reinterpret_cast<PresentFn>(vtable[kPresentIndex]);

        DWORD oldProtect = 0;
        bool ok = VirtualProtect(&vtable[kPresentIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect) != FALSE;
        if (ok) {
            *reinterpret_cast<void**>(&vtable[kPresentIndex]) = reinterpret_cast<void*>(&HookPresent);
            DWORD ignored = 0;
            VirtualProtect(&vtable[kPresentIndex], sizeof(void*), oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), &vtable[kPresentIndex], sizeof(void*));
        }

        SafeRelease(dummyContext);
        SafeRelease(dummyDevice);
        SafeRelease(dummySwap);
        DestroyWindow(hwnd);
        UnregisterClassA(className, wc.hInstance);

        return ok && g_OriginalPresent != nullptr;
    }
}

namespace Features {
    void InitAntiAliasing() {
        if (g_Config.AntiAliasing <= 0) {
            Logger::Log("Anti-aliasing disabled in INI.");
            return;
        }

        if (g_Config.AntiAliasing > 2) g_Config.AntiAliasing = 2;

        if (!InstallPresentHook()) {
            g_DisabledAfterError = true;
            Logger::Log("Anti-aliasing disabled: IDXGISwapChain::Present hook installation failed.");
            return;
        }

        g_HookInstalled = true;
        Logger::Log("Anti-aliasing: D3D11 Present hook installed; FXAA mode %d will initialize on the game's next frame.",
                    g_Config.AntiAliasing);
    }
}
