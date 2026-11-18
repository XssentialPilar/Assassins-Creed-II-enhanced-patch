#include <cstdint>

#include "controller_prompts.h"
#include "keyboard_prompts.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iterator>

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
        default: return PromptButton::None;
    }
}

namespace {

PromptButton controller_role(PromptButton button) {
    if (button == PromptButton::Apply) return PromptButton::South;
    if (button == PromptButton::Cancel) return PromptButton::East;
    return button;
}

enum class GlyphStyle { Keycap, Space, Mouse, Notice };

struct KeycapGeometry {
    float cx, cy, radius, stroke, fill_opacity, stroke_opacity;
    DWORD fill, outline;
};

#include "keyboard_keycap.inc"

struct PadSpriteRun {
    DWORD argb;
    unsigned count;
};

#include "controller_sprites.inc"

void draw_mouse(HDC dc, int button) {
    const HGDIOBJ brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    if (!pen) {
        SelectObject(dc, brush);
        return;
    }
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, 9, 7, 24, 26, 11, 11);
    MoveToEx(dc, 9, 16, nullptr);
    LineTo(dc, 24, 16);
    MoveToEx(dc, 16, 7, nullptr);
    LineTo(dc, 16, 16);
    const RECT buttons[] = {{10, 9, 16, 16}, {17, 9, 23, 16}, {14, 7, 19, 15}};
    FillRect(dc, &buttons[button], static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SelectObject(dc, old_pen);
    SelectObject(dc, brush);
    DeleteObject(pen);
}

void draw_space_bar(HDC dc) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    if (!pen) return;
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    const POINT path[] = {{8, 14}, {8, 18}, {24, 18}, {24, 14}};
    Polyline(dc, path, std::size(path));
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

IDirect3DTexture9* make_glyph(IDirect3DDevice9* device, GlyphStyle style, const wchar_t* label,
                             int font_height, COLORREF ink, int mouse = -1, bool pressed = false) {
    const bool keyboard = style != GlyphStyle::Notice;
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

    if (style == GlyphStyle::Mouse) {
        draw_mouse(dc, mouse);
    } else if (style == GlyphStyle::Space) {
        draw_space_bar(dc);
    } else {
        HFONT font = CreateFontW(font_height, 0, 0, 0, FW_SEMIBOLD,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                                 style == GlyphStyle::Keycap ? L"Segoe UI" : L"Segoe UI Symbol");
        HGDIOBJ original_font = font ? SelectObject(dc, font) : nullptr;
        if (font && style == GlyphStyle::Keycap) {
            SIZE metrics{};
            const int length = static_cast<int>(std::wcslen(label));
            GetTextExtentPoint32W(dc, label, length, &metrics);
            if (metrics.cx > 21) {
                LOGFONTW smaller{};
                GetObjectW(font, sizeof(smaller), &smaller);
                TEXTMETRICW font_metrics{};
                GetTextMetricsW(dc, &font_metrics);
                const LONG font_width = std::max<LONG>(1, font_metrics.tmAveCharWidth);
                smaller.lfWidth = std::max<LONG>(4, font_width * 21 / metrics.cx);
                smaller.lfHeight = std::min<LONG>(-11, font_height * 27 / metrics.cx);
                if (HFONT fitted = CreateFontIndirectW(&smaller)) {
                    SelectObject(dc, fitted);
                    DeleteObject(font);
                    font = fitted;
                }
            }
        }
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        RECT bounds{2, 0, logical_width - 2, LogicalHeight};
        DrawTextW(dc, label, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (original_font) SelectObject(dc, original_font);
        if (font) DeleteObject(font);
    }
    GdiFlush();

    const auto* mask = static_cast<const DWORD*>(pixels);
    const int source_height = height * Supersampling;
    int offset_x = 0, offset_y = 0;
    if (keyboard) {
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
    const auto& shape = pressed ? KeycapPressed : Keycap;
    const DWORD background = keyboard ? shape.fill : 0x14161a;
    const DWORD border = keyboard ? shape.outline : 0xcdd1d7;
    IDirect3DTexture9* texture = nullptr;
    if (SUCCEEDED(device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) {
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0))) {
            for (int y = 0; y < height; ++y) {
                // Fire's sprites use bottom-up V coordinates. The confirmation quad uses normal D3D coordinates.
                const int texture_y = style == GlyphStyle::Notice ? y : height - 1 - y;
                auto* row = reinterpret_cast<DWORD*>(static_cast<BYTE*>(locked.pBits) + texture_y * locked.Pitch);
                for (int x = 0; x < width; ++x) {
                    double distance;
                    if (keyboard) {
                        const double px = (x + 0.5) / pixel_scale - shape.cx;
                        const double py = (y + 0.5) / pixel_scale - shape.cy;
                        distance = std::hypot(px, py) - shape.radius - shape.stroke / 2;
                    } else {
                        distance = std::hypot(std::max(0., std::abs(x - (width - 1) * .5) - (width * .5 - 5)),
                                              std::max(0., std::abs(y - 15.5) - 9.)) - 4.5;
                    }
                    const int sx = x * Supersampling + offset_x, sy = y * Supersampling + offset_y;
                    DWORD coverage = 0;
                    if (sx >= 0 && sx + 1 < source_width && sy >= 0 && sy + 1 < source_height) {
                        const auto* sample = mask + sy * source_width + sx;
                        coverage = ((sample[0] & 255) + (sample[1] & 255) + (sample[source_width] & 255) +
                                    (sample[source_width + 1] & 255) + 2) / 4;
                    }
                    const float outer = static_cast<float>(std::clamp(0.5 - distance * pixel_scale, 0., 1.));
                    if (outer == 0) {
                        row[x] = 0;
                    } else {
                        const bool rim = distance > -(keyboard ? shape.stroke : 1.2);
                        const DWORD base = rim ? border : background;
                        const float base_alpha = keyboard ? (rim ? shape.stroke_opacity : shape.fill_opacity) : .96f;
                        const float mark_alpha = coverage / 255.0f;
                        const float visible_base = base_alpha * (1.0f - mark_alpha);
                        const float combined = mark_alpha + visible_base;
                        const auto channel = [&](unsigned shift, BYTE mark) {
                            return static_cast<DWORD>((((base >> shift) & 255) * visible_base +
                                mark * mark_alpha) / combined + .5f);
                        };
                        row[x] = (static_cast<DWORD>(combined * outer * 255 + .5f) << 24) |
                            (channel(16, GetRValue(ink)) << 16) | (channel(8, GetGValue(ink)) << 8) |
                            channel(0, GetBValue(ink));
                    }
                }
            }
            texture->UnlockRect(0);
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

IDirect3DTexture9* create_key_prompt_texture(IDirect3DDevice9* device, int binding, bool pressed) {
    const COLORREF ink = pressed ? RGB(20, 19, 18) : RGB(250, 250, 250);
    if (binding >= 256 && binding <= 258) {
        return make_glyph(device, GlyphStyle::Mouse, L"", 0, ink, binding - 256, pressed);
    }
    if (binding == 0x39) return make_glyph(device, GlyphStyle::Space, L"", 0, ink, -1, pressed);
    auto label = input_label(binding);
    switch (binding) {
        case 0x2a: label = L"L⇧"; break;
        case 0x36: label = L"R⇧"; break;
        default: break;
    }
    if (label.empty()) {
        return nullptr;
    }
    const int font = label.size() <= 2 ? -19 : -13;
    return make_glyph(device, GlyphStyle::Keycap, label.c_str(), font, ink, -1, pressed);
}

IDirect3DTexture9* create_prompt_notice_texture(IDirect3DDevice9* device, const wchar_t* label) {
    return make_glyph(device, GlyphStyle::Notice, label, -17, RGB(255, 255, 255));
}

namespace {

constexpr LONG AtlasCell = 32;
constexpr LONG OutputCell = 64;

bool copy_scaled_sprite(IDirect3DTexture9* output, const RECT* output_area, unsigned width, unsigned height,
                        IDirect3DTexture9* input, const RECT* input_area, unsigned source_width, unsigned source_height,
                        unsigned inset = 0) {
    D3DLOCKED_RECT source{}, destination{};
    if (FAILED(input->LockRect(0, &source, input_area, D3DLOCK_READONLY))) return false;
    if (FAILED(output->LockRect(0, &destination, output_area, 0))) {
        input->UnlockRect(0);
        return false;
    }
    for (unsigned y = 0; y < height; ++y) {
        auto* pixels = reinterpret_cast<DWORD*>(static_cast<BYTE*>(destination.pBits) + y * destination.Pitch);
        if (y < inset || y >= height - inset) {
            std::fill_n(pixels, width, 0);
            continue;
        }
        const unsigned sample_y = ((y - inset) * source_height + source_height / 2) / (height - inset * 2);
        const auto* row = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(source.pBits) + sample_y * source.Pitch);
        std::fill_n(pixels, inset, 0);
        for (unsigned x = inset; x < width - inset; ++x) {
            pixels[x] = row[((x - inset) * source_width + source_width / 2) / (width - inset * 2)];
        }
        std::fill_n(pixels + width - inset, inset, 0);
    }
    output->UnlockRect(0);
    input->UnlockRect(0);
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
    for (const auto& tile : tiles) {
        if (tile.role != role) continue;
        const RECT stock{tile.x * AtlasCell, tile.y * AtlasCell, (tile.x + 1) * AtlasCell, (tile.y + 1) * AtlasCell};
        const RECT target{tile.x * OutputCell, tile.y * OutputCell, (tile.x + 1) * OutputCell, (tile.y + 1) * OutputCell};
        if (!copy_scaled_sprite(atlas, &target, OutputCell, OutputCell, replacement ? replacement : original,
                                replacement ? nullptr : &stock, replacement ? description.Width : AtlasCell,
                                replacement ? description.Height : AtlasCell, replacement ? 8 : 0)) return false;
    }
    return true;
}
