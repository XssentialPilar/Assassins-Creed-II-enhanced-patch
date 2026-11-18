#include <windows.h>
#include <d3d9.h>

#include <cstdio>

namespace {

constexpr UINT Width = 640;
constexpr UINT Height = 400;
constexpr D3DVIEWPORT9 Full{0, 0, Width, Height, 0, 1};
constexpr D3DVIEWPORT9 Letterbox{0, 20, Width, 360, 0, 1};

bool check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::fprintf(stderr, "%s failed (0x%08lx)\n", operation, static_cast<unsigned long>(result));
        return false;
    }
    return true;
}

bool test_frame(IDirect3DDevice9* device) {
    const RECT bounds{0, 20, Width, 380};
    D3DMATRIX projection{};
    projection._11 = 0.5625f;
    projection._22 = projection._33 = projection._44 = 1.0f;
    const float shader_projection[16] = {
        0.5625f, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 1,
        0, 0, -0.1f, 0,
    };
    const float shader_view_projection[16] = {
        0.45f, 0.6f, 0, 0,
        -0.3375f, 0.8f, 0, 0,
        0, 0, 1, 1,
        0.1f, 0.2f, 1.9f, 2,
    };
    const float shader_flat_projection[16] = {
        2.0f / 1280, 0, 0, 0,
        0, -2.0f / 720, 0, 0,
        0, 0, 1, 0,
        -1, 1, 0, 1,
    };
    return check(device->SetViewport(&Full), "SetViewport full") &&
           check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0), "Clear background") &&
           check(device->SetViewport(&Letterbox), "SetViewport letterbox") &&
           check(device->SetScissorRect(&bounds), "SetScissorRect") &&
           check(device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE), "Enable scissor") &&
           check(device->SetTransform(D3DTS_PROJECTION, &projection), "SetTransform projection") &&
           check(device->SetVertexShaderConstantF(8, shader_projection, 4), "SetVertexShaderConstantF") &&
           check(device->SetVertexShaderConstantF(12, shader_view_projection, 4), "Set combined projection") &&
           check(device->SetVertexShaderConstantF(16, shader_flat_projection, 4), "Set flat projection") &&
           check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff336699, 1, 0), "Clear scene");
}

bool check_pixels(IDirect3DDevice9* device) {
    IDirect3DSurface9* backbuffer = nullptr;
    IDirect3DSurface9* readback = nullptr;
    D3DSURFACE_DESC description{};
    D3DLOCKED_RECT locked{};
    bool passed = false;
    if (check(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer), "GetBackBuffer") &&
        check(backbuffer->GetDesc(&description), "GetDesc") &&
        check(device->CreateOffscreenPlainSurface(Width, Height, description.Format,
                                                 D3DPOOL_SYSTEMMEM, &readback, nullptr), "Readback surface") &&
        check(device->GetRenderTargetData(backbuffer, readback), "GetRenderTargetData") &&
        check(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "LockRect")) {
        const auto rgb = [&locked](UINT row) {
            const auto* pixels = static_cast<const BYTE*>(locked.pBits) + row * locked.Pitch;
            return reinterpret_cast<const DWORD*>(pixels)[Width / 2] & 0xffffff;
        };
        passed = rgb(0) == 0 && rgb(19) == 0 && rgb(20) == 0x336699 &&
                 rgb(379) == 0x336699 && rgb(380) == 0 && rgb(399) == 0;
        readback->UnlockRect();
        const RECT bounds{0, 20, Width, 380};
        passed = passed && check(device->SetRenderTarget(0, backbuffer), "Rebind backbuffer") &&
                 check(device->SetViewport(&Letterbox), "Restore letterbox viewport") &&
                 check(device->SetScissorRect(&bounds), "Restore scissor rectangle");
    }
    if (readback) {
        readback->Release();
    }
    if (backbuffer) {
        backbuffer->Release();
    }
    return passed;
}

bool run(HWND window) {
    IDirect3D9* factory = Direct3DCreate9(D3D_SDK_VERSION);
    if (!factory) {
        std::fprintf(stderr, "Direct3DCreate9 failed\n");
        return false;
    }
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.BackBufferWidth = Width;
    parameters.BackBufferHeight = Height;
    parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
    parameters.hDeviceWindow = window;
    parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = nullptr;
    if (!check(factory->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                      D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device), "CreateDevice")) {
        factory->Release();
        return false;
    }
    IDirect3D9* parent = nullptr;
    IUnknown* identity = nullptr;
    bool passed = check(device->GetDirect3D(&parent), "GetDirect3D") && parent == factory &&
                  check(device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity)), "QueryInterface") &&
                  identity == static_cast<IUnknown*>(device);
    if (parent) {
        parent->Release();
    }
    if (identity) {
        identity->Release();
    }
    factory->Release();
    for (unsigned frame = 0; passed && frame <= 60; ++frame) {
        passed = test_frame(device) && (frame > 0 || check_pixels(device)) &&
                 check(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
    }
    parameters.BackBufferHeight = 360;
    passed = passed && check(device->Reset(&parameters), "Reset");
    device->Release();
    return passed;
}

}  // namespace

int main() {
    std::printf("pid=%lu\n", GetCurrentProcessId());
    HWND window = CreateWindowExW(0, L"STATIC", L"AC II Direct3D 9 smoke test", WS_POPUP,
                                  0, 0, Width, Height, nullptr, nullptr, nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "CreateWindowExW failed\n");
        return 1;
    }
    const bool passed = run(window);
    DestroyWindow(window);
    std::puts(passed ? "D3D9 smoke test passed" : "D3D9 smoke test failed");
    return passed ? 0 : 1;
}
