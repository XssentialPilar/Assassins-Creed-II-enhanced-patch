#include <cstdint>

#include "controller_prompts.h"
#include "keyboard_prompts.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>

#include <SDL3/SDL.h>

PromptButton prompt_button(std::uint64_t fingerprint) {
    switch (fingerprint) {
        case 0x718b3bfd74451ca5ull: return PromptButton::Apply;
        case 0x1468983cac3f0596ull: return PromptButton::South;
        case 0x28085c42e2f616beull: return PromptButton::Cancel;
        case 0xfe6268926eb63a5full: return PromptButton::East;
        case 0xbfb7cb1e4c0d1af3ull: return PromptButton::West;
        case 0xf7920ee604fd2a7full: return PromptButton::North;
        case 0xac93225b6073fbd3ull: return PromptButton::LeftTrigger;
        case 0x95f7f56474be3b54ull: return PromptButton::RightTrigger;
        case 0x3b80cf527982dbe1ull: return PromptButton::LeftShoulder;
        case 0xd3d3338d9ae2a061ull: return PromptButton::RightShoulder;
        case 0x1d29716299064f8bull: return PromptButton::Start;
        case 0xc4d7b26ce6a4473eull: return PromptButton::Back;
        case 0xf806bf57cb862e37ull: return PromptButton::MoveForward;
        case 0x755c4b803da25bf7ull: return PromptButton::MoveBack;
        case 0x21bc02c8090a1f6eull: return PromptButton::MoveLeft;
        case 0x394b74c3eddee20eull: return PromptButton::MoveRight;
        case 0x15fdbc117e1e3200ull: return PromptButton::LookUp;
        case 0xbb1112fc821b1487ull: return PromptButton::LookDown;
        case 0xdd419d7299155681ull: return PromptButton::LookLeft;
        case 0x63f780f4a88cd41cull: return PromptButton::LookRight;
        case 0x130a58054428d087ull: return PromptButton::Medicine;
        case 0xd18b07d051ed5b7dull: return PromptButton::HiddenBlade;
        case 0x17e35225ad7544beull: return PromptButton::Sword;
        case 0xda6c0ed68651c2adull: return PromptButton::Fists;
        case 0x4644c89bcb2ada2eull: return PromptButton::CenterCamera;
        case 0x898c0e69363388e5ull: return PromptButton::FirstPerson;
        default: return PromptButton::None;
    }
}

namespace {

PromptButton controller_role(PromptButton button) {
    if (button == PromptButton::Apply) return PromptButton::South;
    if (button == PromptButton::Cancel) return PromptButton::East;
    return button;
}

enum class GlyphStyle { Keycap, Symbol, Notice };
enum class KeycapSymbol { None = -1, MouseLeft, MouseRight, MouseMiddle, ShiftLeft, ShiftRight, Space };

#include "keyboard_keycap.inc"

constexpr unsigned KeycapTile = 128;
constexpr unsigned KeycapColumns = 4;
using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;

const SDL_Surface* keycap_sheet() {
    static const Surface sheet = [] {
        auto* stream = SDL_IOFromConstMem(KeyboardKeycapPng, sizeof(KeyboardKeycapPng));
        Surface decoded(stream ? SDL_LoadPNG_IO(stream, true) : nullptr, SDL_DestroySurface);
        if (!decoded || decoded->w != KeycapColumns * KeycapTile || decoded->h != 4 * KeycapTile) {
            return Surface(nullptr, SDL_DestroySurface);
        }
        return Surface(SDL_ConvertSurface(decoded.get(), SDL_PIXELFORMAT_ARGB8888), SDL_DestroySurface);
    }();
    return sheet.get();
}

struct PadSpriteRun {
    DWORD argb;
    unsigned count;
};

#include "controller_sprites.inc"

DWORD blend_ink(DWORD base, COLORREF ink, DWORD coverage) {
    const DWORD base_alpha = (base >> 24) * (255 - coverage);
    const DWORD ink_alpha = coverage * 255;
    const DWORD total_alpha = base_alpha + ink_alpha;
    if (total_alpha == 0) return 0;
    const auto channel = [=](unsigned shift, BYTE mark) {
        return ((((base >> shift) & 255) * base_alpha + mark * ink_alpha) + total_alpha / 2) / total_alpha;
    };
    return ((total_alpha + 127) / 255 << 24) | (channel(16, GetRValue(ink)) << 16) |
        (channel(8, GetGValue(ink)) << 8) | channel(0, GetBValue(ink));
}

DWORD filter_keycap(const DWORD* const* rows, unsigned x, unsigned size) {
    unsigned alpha = 0, red = 0, green = 0, blue = 0;
    for (unsigned y = 0; y < size; ++y) {
        for (unsigned column = 0; column < size; ++column) {
            const DWORD pixel = rows[y][x + column];
            const unsigned opacity = pixel >> 24;
            alpha += opacity;
            red += (pixel >> 16 & 255) * opacity;
            green += (pixel >> 8 & 255) * opacity;
            blue += (pixel & 255) * opacity;
        }
    }
    if (!alpha) return 0;
    const unsigned half = alpha / 2;
    const unsigned area = size * size;
    return (((alpha + area / 2) / area) << 24) | (((red + half) / alpha) << 16) |
        (((green + half) / alpha) << 8) | ((blue + half) / alpha);
}

const DWORD* sheet_row(const SDL_Surface* sheet, unsigned column, unsigned row) {
    return reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(sheet->pixels) + row * sheet->pitch) +
        column * KeycapTile;
}

bool fill_keycap_mips(IDirect3DTexture9* texture) {
    for (unsigned level = 1, size = KeycapTile / 2; level < texture->GetLevelCount(); ++level, size /= 2) {
        D3DLOCKED_RECT previous{}, result{};
        if (FAILED(texture->LockRect(level - 1, &previous, nullptr, D3DLOCK_READONLY))) return false;
        if (FAILED(texture->LockRect(level, &result, nullptr, 0))) {
            texture->UnlockRect(level - 1);
            return false;
        }
        for (unsigned y = 0; y < size; ++y) {
            const auto* scan = static_cast<BYTE*>(previous.pBits) + 2 * y * previous.Pitch;
            const DWORD* rows[] = {reinterpret_cast<const DWORD*>(scan),
                                   reinterpret_cast<const DWORD*>(scan + previous.Pitch)};
            auto* destination = reinterpret_cast<DWORD*>(static_cast<BYTE*>(result.pBits) + y * result.Pitch);
            for (unsigned x = 0; x < size; ++x) destination[x] = filter_keycap(rows, x * 2, 2);
        }
        texture->UnlockRect(level);
        texture->UnlockRect(level - 1);
    }
    return true;
}

IDirect3DTexture9* make_glyph(IDirect3DDevice9* device, GlyphStyle style, const wchar_t* label,
                             int font_height, COLORREF ink, KeycapSymbol symbol = KeycapSymbol::None, bool pressed = false) {
    const bool keyboard = style != GlyphStyle::Notice;
    const auto* sheet = keyboard ? keycap_sheet() : nullptr;
    if (keyboard && !sheet) return nullptr;
    const auto length = std::wcslen(label);
    const unsigned tile = symbol == KeycapSymbol::Space ? 2 :
        symbol == KeycapSymbol::ShiftLeft || symbol == KeycapSymbol::ShiftRight ? 1 :
        length > 6 ? 3 : length > 4 ? 2 : length > 2 ? 1 : 0;
    constexpr int widths[] = {14, 19, 23, 26};
    const int logical_width = style == GlyphStyle::Notice ? 256 : 32;
    constexpr int LogicalHeight = 32;
    const int pixel_scale = keyboard ? 4 : 1;
    const int width = logical_width * pixel_scale;
    const int height = LogicalHeight * pixel_scale;
    constexpr int Supersampling = 2;
    const int source_width = width * Supersampling;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = source_width;
    info.bmiHeader.biHeight = -(height * Supersampling);
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
    HGDIOBJ original_bitmap = SelectObject(dc, bitmap);
    std::memset(pixels, 0, source_width * height * Supersampling * sizeof(DWORD));
    SetGraphicsMode(dc, GM_ADVANCED);
    const float drawing_scale = static_cast<float>(Supersampling * pixel_scale);
    const XFORM upscale{drawing_scale, 0, 0, drawing_scale, 0, 0};
    SetWorldTransform(dc, &upscale);

    if (style != GlyphStyle::Symbol) {
        HFONT font = CreateFontW(font_height, 0, 0, 0, keyboard ? FW_BOLD : FW_SEMIBOLD,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                                 style == GlyphStyle::Keycap ? L"Segoe UI" : L"Segoe UI Symbol");
        HGDIOBJ original_font = font ? SelectObject(dc, font) : nullptr;
        if (font && style == GlyphStyle::Keycap) {
            SIZE metrics{};
            const int length = static_cast<int>(std::wcslen(label));
            GetTextExtentPoint32W(dc, label, length, &metrics);
            if (metrics.cx > widths[tile]) {
                LOGFONTW smaller{};
                GetObjectW(font, sizeof(smaller), &smaller);
                TEXTMETRICW font_metrics{};
                GetTextMetricsW(dc, &font_metrics);
                const LONG font_width = std::max<LONG>(1, font_metrics.tmAveCharWidth);
                smaller.lfWidth = std::max<LONG>(3, font_width * widths[tile] / metrics.cx);
                smaller.lfHeight = std::min<LONG>(-9, font_height * (widths[tile] + 2) / metrics.cx);
                if (HFONT fitted = CreateFontIndirectW(&smaller)) {
                    SelectObject(dc, fitted);
                    DeleteObject(font);
                    font = fitted;
                }
            }
        }
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        RECT bounds{0, 0, logical_width, LogicalHeight};
        DrawTextW(dc, label, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
        if (original_font) SelectObject(dc, original_font);
        if (font) DeleteObject(font);
    }
    GdiFlush();

    const auto* mask = static_cast<const DWORD*>(pixels);
    const int source_height = height * Supersampling;
    int offset_x = 0, offset_y = 0;
    if (style == GlyphStyle::Keycap) {
        int left = source_width, top = source_height, right = -1, bottom = -1;
        for (int y = 0; y < source_height; ++y) {
            for (int x = 0; x < source_width; ++x) {
                if ((mask[y * source_width + x] & 255) < 32) continue;
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
        if (right >= left && bottom >= top) {
            offset_x = (left + right + 1 - source_width) / 2;
            offset_y = (top + bottom + 1 - source_height) / 2;
        }
    }
    IDirect3DTexture9* texture = nullptr;
    if (SUCCEEDED(device->CreateTexture(width, height, keyboard ? 4 : 1, 0, D3DFMT_A8R8G8B8,
                                        D3DPOOL_MANAGED, &texture, nullptr))) {
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0))) {
            for (int y = 0; y < height; ++y) {
                // Fire's sprites use bottom-up V coordinates. The confirmation quad uses normal D3D coordinates.
                const int texture_y = style == GlyphStyle::Notice ? y : height - 1 - y;
                auto* row = reinterpret_cast<DWORD*>(static_cast<BYTE*>(locked.pBits) + texture_y * locked.Pitch);
                const DWORD* png_row = sheet ? sheet_row(sheet, tile, y + static_cast<unsigned>(pressed) * KeycapTile)
                                             : nullptr;
                const unsigned mask_index = static_cast<unsigned>(symbol);
                const DWORD* symbol_ink = symbol != KeycapSymbol::None
                    ? sheet_row(sheet, mask_index % KeycapColumns, y + (2 + mask_index / KeycapColumns) * KeycapTile)
                    : nullptr;
                for (int x = 0; x < width; ++x) {
                    const int sx = x * Supersampling + offset_x, sy = y * Supersampling + offset_y;
                    DWORD coverage = 0;
                    if (sx >= 0 && sx + 1 < source_width && sy >= 0 && sy + 1 < source_height) {
                        const auto* sample = mask + sy * source_width + sx;
                        coverage = ((sample[0] & 255) + (sample[1] & 255) + (sample[source_width] & 255) +
                                    (sample[source_width + 1] & 255) + 2) / 4;
                    }
                    if (png_row) {
                        const DWORD base = png_row[x];
                        if (symbol_ink) coverage = symbol_ink[x] >> 24;
                        row[x] = base >> 24 ? blend_ink(base, ink, coverage) : base;
                        continue;
                    }
                    const double distance = std::hypot(std::max(0., std::abs(x - (width - 1) * .5) - (width * .5 - 5)),
                                                       std::max(0., std::abs(y - 15.5) - 9.)) - 4.5;
                    const DWORD alpha = static_cast<DWORD>(255 * std::clamp(.5 - distance, 0., 1.) + .5);
                    const DWORD base = (alpha << 24) | (distance > -1.2 ? 0xcdd1d7 : 0x14161a);
                    row[x] = alpha ? blend_ink(base, ink, coverage) : 0;
                }
            }
            texture->UnlockRect(0);
            if (keyboard && !fill_keycap_mips(texture)) {
                texture->Release();
                texture = nullptr;
            }
        } else {
            texture->Release();
            texture = nullptr;
        }
    }

    SelectObject(dc, original_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return texture;
}

}  // namespace

bool controller_prompt_pressed(PromptButton button, DWORD pressed) {
    constexpr DWORD controls[] = {0, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                                  PromptLeftTrigger, PromptRightTrigger, XINPUT_GAMEPAD_LEFT_SHOULDER,
                                  XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK};
    const auto index = static_cast<unsigned>(controller_role(button));
    return index < std::size(controls) && (pressed & controls[index]) != 0;
}

IDirect3DTexture9* create_prompt_texture(IDirect3DDevice9* device, ControllerFamily family, PromptButton button,
                                       bool pressed) {
    button = controller_role(button);
    static_assert(std::size(PadSpriteOffsets) == (static_cast<unsigned>(ControllerFamily::Count) - 1) *
                  2 * PadSpriteButtons + 1);
    const unsigned role = static_cast<unsigned>(button);
    if (family <= ControllerFamily::Unknown || family >= ControllerFamily::Count ||
        role == 0 || role > PadSpriteButtons) {
        return nullptr;
    }
    const unsigned sprite = ((static_cast<unsigned>(family) - 1) * 2 + static_cast<unsigned>(pressed)) *
        PadSpriteButtons + role - 1;
    DWORD rows[PadSpriteSize * PadSpriteSize];
    DWORD* output = rows;
    for (unsigned i = PadSpriteOffsets[sprite]; i < PadSpriteOffsets[sprite + 1]; ++i) {
        output = std::fill_n(output, PadSpriteRuns[i].count, PadSpriteRuns[i].argb);
    }
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(PadSpriteSize, PadSpriteSize, 1, 0, D3DFMT_A8R8G8B8,
                                      D3DPOOL_MANAGED, &texture, nullptr))) return nullptr;
    D3DLOCKED_RECT lock{};
    if (FAILED(texture->LockRect(0, &lock, nullptr, 0))) {
        texture->Release();
        return nullptr;
    }
    for (unsigned y = 0; y < PadSpriteSize; ++y) {
        auto* dest = static_cast<BYTE*>(lock.pBits) + y * lock.Pitch;
        std::memcpy(dest, rows + (PadSpriteSize - 1 - y) * PadSpriteSize, PadSpriteSize * sizeof(DWORD));
    }
    texture->UnlockRect(0);
    return texture;
}

IDirect3DTexture9* create_key_prompt_texture(IDirect3DDevice9* device, int binding, bool pressed, HKL layout) {
    const COLORREF ink = pressed ? RGB(20, 19, 18) : RGB(250, 250, 250);
    auto symbol = KeycapSymbol::None;
    if (binding >= 256 && binding <= 258) {
        symbol = static_cast<KeycapSymbol>(binding - 256);
    }
    switch (binding) {
        case 0x2a: symbol = KeycapSymbol::ShiftLeft; break;
        case 0x36: symbol = KeycapSymbol::ShiftRight; break;
        case 0x39: symbol = KeycapSymbol::Space; break;
        default: break;
    }
    if (symbol != KeycapSymbol::None) return make_glyph(device, GlyphStyle::Symbol, L"", 0, ink, symbol, pressed);
    const auto label = input_label(binding, layout);
    if (label.empty()) {
        return nullptr;
    }
    const int font = label.size() <= 2 ? -17 : label.size() <= 4 ? -12 : -11;
    return make_glyph(device, GlyphStyle::Keycap, label.c_str(), font, ink, KeycapSymbol::None, pressed);
}

IDirect3DTexture9* create_prompt_notice_texture(IDirect3DDevice9* device, const wchar_t* label) {
    return make_glyph(device, GlyphStyle::Notice, label, -17, RGB(255, 255, 255));
}

namespace {

constexpr LONG AtlasCell = 32;
constexpr LONG OutputCell = 64;
// Keep clear space between the original label anchors and the longest key.
constexpr unsigned KeycapInset = 6;

DWORD interpolate_keycap(const DWORD* first, const DWORD* second, unsigned x, unsigned fx, unsigned fy) {
    constexpr unsigned Unit = 256;
    const unsigned weights[] = {(Unit - fx) * (Unit - fy), fx * (Unit - fy), (Unit - fx) * fy, fx * fy};
    const DWORD pixels[] = {first[x], first[x + 1], second[x], second[x + 1]};
    unsigned alpha = 0, red = 0, green = 0, blue = 0;
    for (unsigned index = 0; index < 4; ++index) {
        const DWORD pixel = pixels[index];
        const unsigned opacity = (pixel >> 24) * weights[index];
        alpha += opacity;
        red += (pixel >> 16 & 255) * opacity;
        green += (pixel >> 8 & 255) * opacity;
        blue += (pixel & 255) * opacity;
    }
    if (!alpha) return 0;
    const unsigned half = alpha / 2;
    return (((alpha + 32768) >> 16) << 24) | (((red + half) / alpha) << 16) |
        (((green + half) / alpha) << 8) | ((blue + half) / alpha);
}

bool copy_scaled_sprite(IDirect3DTexture9* output, const RECT* output_area, unsigned width, unsigned height,
                        IDirect3DTexture9* input, const RECT* input_area, unsigned source_width, unsigned source_height,
                        unsigned inset = 0, unsigned source_level = 0) {
    D3DLOCKED_RECT source{}, destination{};
    if (FAILED(input->LockRect(source_level, &source, input_area, D3DLOCK_READONLY))) return false;
    if (FAILED(output->LockRect(0, &destination, output_area, 0))) {
        input->UnlockRect(source_level);
        return false;
    }
    for (unsigned y = 0; y < height; ++y) {
        auto* pixels = reinterpret_cast<DWORD*>(static_cast<BYTE*>(destination.pBits) + y * destination.Pitch);
        if (y < inset || y >= height - inset) {
            std::fill_n(pixels, width, 0);
            continue;
        }
        const unsigned scaled_y = (2 * (y - inset) + 1) * source_height;
        const unsigned target_height = height - inset * 2;
        const unsigned sample_y = source_level ? (scaled_y * 128 / target_height - 128) : scaled_y / (2 * target_height);
        const auto* scan = static_cast<const BYTE*>(source.pBits) + (source_level ? sample_y >> 8 : sample_y) * source.Pitch;
        const auto* row = reinterpret_cast<const DWORD*>(scan);
        std::fill_n(pixels, inset, 0);
        for (unsigned x = inset; x < width - inset; ++x) {
            const unsigned scaled_x = (2 * (x - inset) + 1) * source_width;
            const unsigned target_width = width - inset * 2;
            if (source_level) {
                const unsigned sample_x = scaled_x * 128 / target_width - 128;
                pixels[x] = interpolate_keycap(row, reinterpret_cast<const DWORD*>(scan + source.Pitch),
                                               sample_x >> 8, sample_x & 255, sample_y & 255);
            } else {
                pixels[x] = row[scaled_x / (2 * target_width)];
            }
        }
        std::fill_n(pixels + width - inset, inset, 0);
    }
    output->UnlockRect(0);
    input->UnlockRect(source_level);
    return true;
}

}  // namespace

IDirect3DTexture9* create_action_prompt_atlas(IDirect3DDevice9* device, IDirect3DTexture9* original) {
    constexpr UINT Width = 4 * AtlasCell, Height = 8 * AtlasCell;
    D3DSURFACE_DESC description{};
    if (FAILED(original->GetLevelDesc(0, &description)) || description.Width != Width ||
        description.Height != Height || description.Format != D3DFMT_A8R8G8B8) return nullptr;

    IDirect3DTexture9* atlas = nullptr;
    if (FAILED(device->CreateTexture(4 * OutputCell, 8 * OutputCell, 1, 0, D3DFMT_A8R8G8B8,
                                     D3DPOOL_MANAGED, &atlas, nullptr))) return nullptr;
    if (!copy_scaled_sprite(atlas, nullptr, 4 * OutputCell, 8 * OutputCell, original, nullptr, Width, Height)) {
        atlas->Release();
        return nullptr;
    }
    return atlas;
}

bool update_action_prompt_atlas(IDirect3DTexture9* atlas, IDirect3DTexture9* original,
                                PromptButton role, IDirect3DTexture9* replacement) {
    // Coordinates in the stock atlas memory, including light and dark art for each input family.
    constexpr struct { LONG x, y; PromptButton role; } tiles[] = {
        {0, 2, PromptButton::South}, {0, 3, PromptButton::South},
        {2, 0, PromptButton::South}, {2, 1, PromptButton::South},
        {1, 1, PromptButton::South}, {1, 2, PromptButton::South},
        {0, 0, PromptButton::East}, {1, 3, PromptButton::East},
        {2, 4, PromptButton::East}, {1, 4, PromptButton::East},
        {3, 3, PromptButton::East}, {2, 3, PromptButton::East},
        {3, 0, PromptButton::West}, {3, 1, PromptButton::West},
        {0, 1, PromptButton::West}, {3, 2, PromptButton::West},
        {1, 0, PromptButton::West}, {2, 2, PromptButton::West},
        {3, 4, PromptButton::North}, {1, 5, PromptButton::North},
        {1, 6, PromptButton::North}, {2, 6, PromptButton::North},
        {2, 5, PromptButton::North}, {3, 5, PromptButton::North},
        {0, 7, PromptButton::LeftShoulder}, {0, 5, PromptButton::LeftShoulder},
    };
    D3DSURFACE_DESC description{};
    if (replacement && FAILED(replacement->GetLevelDesc(0, &description))) return false;
    const bool keycap = description.Width == KeycapTile && description.Height == KeycapTile &&
        replacement->GetLevelCount() > 1;
    for (const auto& tile : tiles) {
        if (tile.role != role) continue;
        const RECT stock{tile.x * AtlasCell, tile.y * AtlasCell, (tile.x + 1) * AtlasCell, (tile.y + 1) * AtlasCell};
        const RECT target{tile.x * OutputCell, tile.y * OutputCell, (tile.x + 1) * OutputCell, (tile.y + 1) * OutputCell};
        if (!copy_scaled_sprite(atlas, &target, OutputCell, OutputCell, replacement ? replacement : original,
                                replacement ? nullptr : &stock, replacement ? description.Width >> keycap : AtlasCell,
                                replacement ? description.Height >> keycap : AtlasCell,
                                keycap ? KeycapInset : replacement ? 8 : 0, keycap ? 1 : 0)) return false;
    }
    return true;
}
