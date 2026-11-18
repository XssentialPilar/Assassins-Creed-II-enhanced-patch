#include "d3d9_video_fit.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>

namespace {

template <typename T>
struct Release {
    void operator()(T* value) const noexcept { value->Release(); }
};

template <typename T>
using D3DPtr = std::unique_ptr<T, Release<T>>;

struct Size { UINT width, height; };

constexpr Size formats[] = {{640,480}, {1920,1200}, {720,480}, {640,512}, {512,640},
    {960,480}, {1008,432}, {950,400}, {740,400}, {940,400}, {956,400}, {960,400}, {1104,400},
    {450,800}, {800,400}, {1950,100}, {1000,450}};
constexpr Size sources[] = {{1280,720}, {640,480}, {512,256}};

bool test_layout() {
    for (Size mode : formats) {
        for (Size source : sources) {
            const auto layout = fit_video_viewport(mode.width, mode.height, source.width, source.height, 0, 1);
            const auto& p = layout.picture;
            const double source_ratio = static_cast<double>(source.width) / source.height;
            if (!p.Width || !p.Height || p.X + p.Width > mode.width || p.Y + p.Height > mode.height ||
                std::abs(p.Width - p.Height * source_ratio) > source_ratio * .501 + .501) {
                return false;
            }
            if (layout.vertical_bands ?
                layout.first_band.Width + p.Width + layout.second_band.Width != mode.width :
                layout.first_band.Height + p.Height + layout.second_band.Height != mode.height) {
                return false;
            }
        }
    }
    return true;
}

bool ok(HRESULT result, const char* step) {
    if (FAILED(result)) {
        std::fprintf(stderr, "%s: 0x%08lx\n", step, static_cast<unsigned long>(result));
        return false;
    }
    return true;
}

bool verify_pixels(IDirect3DDevice9* device, Size output) {
    IDirect3DSurface9* raw = nullptr;
    if (!ok(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &raw), "backbuffer")) {
        return false;
    }
    D3DPtr<IDirect3DSurface9> backbuffer(raw);
    if (!ok(device->CreateOffscreenPlainSurface(output.width, output.height, D3DFMT_X8R8G8B8,
        D3DPOOL_SYSTEMMEM, &raw, nullptr), "readback buffer")) {
        return false;
    }
    D3DPtr<IDirect3DSurface9> pixels(raw);
    if (!ok(device->GetRenderTargetData(backbuffer.get(), pixels.get()), "readback")) {
        return false;
    }
    D3DLOCKED_RECT locked{};
    if (!ok(pixels->LockRect(&locked, nullptr, D3DLOCK_READONLY), "lock readback")) {
        return false;
    }
    const auto rgb = [&locked](UINT x, UINT y) {
        const auto* row = static_cast<const BYTE*>(locked.pBits) + y * locked.Pitch;
        return reinterpret_cast<const DWORD*>(row)[x] & 0x00ffffffu;
    };
    const UINT cx = output.width / 2, cy = output.height / 2;
    const auto layout = fit_video_viewport(output.width, output.height, 1280, 720, 0, 1);
    const auto& p = layout.picture;
    const DWORD first = layout.vertical_bands ? rgb(p.X - 2, cy) : rgb(cx, p.Y - 2);
    const DWORD second = layout.vertical_bands ? rgb(p.X + p.Width + 1, cy) : rgb(cx, p.Y + p.Height + 1);
    const DWORD first_far = layout.vertical_bands ? rgb(1, cy) : rgb(cx, 1);
    const DWORD second_far = layout.vertical_bands ? rgb(output.width - 2, cy) : rgb(cx, output.height - 2);
    unsigned square_width = 0, square_height = 0;
    for (UINT x = 0; x < output.width; ++x) {
        square_width += rgb(x, cy) == 0xff0000;
    }
    for (UINT y = 0; y < output.height; ++y) {
        square_height += rgb(cx, y) == 0xff0000;
    }
    pixels->UnlockRect();
    const bool square = square_width > 0 && std::abs(static_cast<int>(square_width) -
        static_cast<int>(square_height)) <= 1;
    const bool bands = first == 0x008800 && second == 0x0088ff &&
                       first_far && second_far && first_far != first && second_far != second;
    if (!square || !bands) {
        std::fprintf(stderr, "unexpected render: %06lx %06lx, outer %06lx %06lx, square %u x %u\n",
            first, second, first_far, second_far, square_width, square_height);
    }
    return square && bands;
}

bool test_gpu(HWND window, Size output) {
    D3DPtr<IDirect3D9> factory(Direct3DCreate9(D3D_SDK_VERSION));
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.hDeviceWindow = window;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
    parameters.BackBufferWidth = output.width;
    parameters.BackBufferHeight = output.height;
    IDirect3DDevice9* raw = nullptr;
    if (!factory || !ok(factory->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &raw), "create device")) {
        return false;
    }
    D3DPtr<IDirect3DDevice9> device(raw);
    IDirect3DTexture9* raw_texture = nullptr;
    if (!ok(device->CreateTexture(1280, 720, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &raw_texture, nullptr),
        "create test movie texture")) {
        return false;
    }
    D3DPtr<IDirect3DTexture9> texture(raw_texture);
    D3DLOCKED_RECT locked{};
    if (!ok(texture->LockRect(0, &locked, nullptr, 0), "lock texture")) {
        return false;
    }
    const auto layout = fit_video_viewport(output.width, output.height, 1280, 720, 0, 1);
    for (UINT y = 0; y < 720; ++y) {
        auto* row = reinterpret_cast<DWORD*>(static_cast<BYTE*>(locked.pBits) + y * locked.Pitch);
        for (UINT x = 0; x < 1280; ++x) {
            const UINT edge = layout.vertical_bands ? x : y;
            const UINT extent = layout.vertical_bands ? 1280 : 720;
            row[x] = x >= 576 && x < 704 && y >= 296 && y < 424 ? 0xffff0000 :
                (edge < 16 ? 0xff008800 : (edge >= extent - 16 ? 0xff0088ff : 0xff002244));
        }
    }
    texture->UnlockRect(0);

    const D3DVIEWPORT9 previous = layout.picture;
    const float quad[4][5] = {{-1,1,.5f,0,0}, {1,1,.5f,1,0}, {-1,-1,.5f,0,1}, {1,-1,.5f,1,1}};
    if (!ok(device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1), "set vertex format") ||
        !ok(device->SetTexture(0, texture.get()), "bind test movie texture") ||
        !ok(device->SetRenderState(D3DRS_LIGHTING, FALSE), "disable lighting") ||
        !ok(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "texture color operation") ||
        !ok(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE), "texture color source") ||
        !ok(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "disable culling") ||
        !ok(device->SetViewport(&previous), "initial viewport") ||
        !ok(device->BeginScene(), "begin movie draw")) {
        return false;
    }
    HRESULT result = E_FAIL;
    UINT bands_drawn = 0;
    MovieSurround surround;
    const bool handled = draw_fitted_movie(device.get(), output.width, output.height, quad, surround, result, bands_drawn);
    const HRESULT end = device->EndScene();
    D3DVIEWPORT9 restored{};
    IDirect3DPixelShader9* active_shader = nullptr;
    const HRESULT active_result = device->GetPixelShader(&active_shader);
    D3DPtr<IDirect3DPixelShader9> active(active_shader);
    return handled && bands_drawn == 2 && surround.vertex && surround.pixel && ok(result, "movie frame") &&
        ok(end, "end movie draw") &&
        ok(active_result, "restored pixel shader") && !active &&
        ok(device->GetViewport(&restored), "restored viewport") &&
        restored.X == previous.X && restored.Y == previous.Y &&
        restored.Width == previous.Width && restored.Height == previous.Height && verify_pixels(device.get(), output);
}

}  // namespace

int main() {
    HWND window = CreateWindowExW(0, L"STATIC", L"AC II video fit test", WS_POPUP, 0, 0, 640, 400,
                                  nullptr, nullptr, nullptr, nullptr);
    if (!window) {
        return 1;
    }
    const bool passed = test_layout() && test_gpu(window, {640, 400}) && test_gpu(window, {960, 400});
    DestroyWindow(window);
    if (passed) {
        std::puts("17 formats and 3 video sizes; GPU kept the square proportional and filled both sets of margins");
    }
    return passed ? 0 : 1;
}
