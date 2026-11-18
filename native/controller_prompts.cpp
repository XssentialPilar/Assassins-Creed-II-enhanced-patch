#include <cstdint>

#include "controller_prompts.h"

#include <cmath>
#include <cstring>

PromptButton prompt_button(std::uint64_t fingerprint) {
    switch (fingerprint) {
        case 0x718b3bfd74451ca5ull: // Apply
        case 0x1468983cac3f0596ull: return PromptButton::South;
        case 0x28085c42e2f616beull: // Cancel
        case 0xfe6268926eb63a5full: return PromptButton::East;
        case 0xbfb7cb1e4c0d1af3ull: return PromptButton::West;
        case 0xf7920ee604fd2a7full: return PromptButton::North;
        case 0xac93225b6073fbd3ull: return PromptButton::LeftTrigger;
        case 0x95f7f56474be3b54ull: return PromptButton::RightTrigger;
        case 0x3b80cf527982dbe1ull: return PromptButton::LeftShoulder;
        case 0xd3d3338d9ae2a061ull: return PromptButton::RightShoulder;
        case 0x1d29716299064f8bull: return PromptButton::Start;
        case 0xc4d7b26ce6a4473eull: return PromptButton::Back;
        default: return PromptButton::None;
    }
}

IDirect3DTexture9* create_prompt_texture(IDirect3DDevice9* device, ControllerFamily family, PromptButton button) {
    constexpr int Size = 32;
    constexpr const wchar_t* Xbox[] = {L"", L"A", L"B", L"X", L"Y", L"LT", L"RT", L"LB", L"RB", L"≡", L"▣"};
    constexpr const wchar_t* PS4[] = {L"", L"×", L"○", L"□", L"△", L"L2", L"R2", L"L1", L"R1", L"OPT", L"SH"};
    const unsigned role = static_cast<unsigned>(button);
    if (family == ControllerFamily::Unknown || role == 0 || role >= static_cast<unsigned>(PromptButton::Count)) {
        return nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = Size;
    info.bmiHeader.biHeight = -Size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        return nullptr;
    }
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) {
        DeleteDC(dc);
        return nullptr;
    }
    const auto* label = (family == ControllerFamily::Xbox ? Xbox : PS4)[role];
    HFONT font = CreateFontW(role <= 4 ? -21 : role <= 8 ? -16 : -12, 0, 0, 0, FW_SEMIBOLD,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI Symbol");
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    std::memset(pixels, 0, Size * Size * sizeof(DWORD));
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    RECT bounds{0, 0, Size, Size};
    DrawTextW(dc, label, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();

    IDirect3DTexture9* texture = nullptr;
    if (SUCCEEDED(device->CreateTexture(Size, Size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) {
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0))) {
            const auto* ink = static_cast<const DWORD*>(pixels);
            for (int y = 0; y < Size; ++y) {
                auto* row = reinterpret_cast<DWORD*>(static_cast<BYTE*>(locked.pBits) + y * locked.Pitch);
                for (int x = 0; x < Size; ++x) {
                    const double distance = std::hypot(x - 15.5, y - 15.5);
                    const DWORD gray = ink[y * Size + x] & 0xff;
                    row[x] = distance > 15 ? 0 : distance > 13.5 ? 0xe0ffffff :
                             0xf0000000 | (gray * 0x010101);
                }
            }
            texture->UnlockRect(0);
        } else {
            texture->Release();
            texture = nullptr;
        }
    }
    if (old_font) {
        SelectObject(dc, old_font);
    }
    SelectObject(dc, old_bitmap);
    if (font) {
        DeleteObject(font);
    }
    DeleteObject(bitmap);
    DeleteDC(dc);
    return texture;
}
