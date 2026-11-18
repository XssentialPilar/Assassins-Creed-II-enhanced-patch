#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <new>

#ifdef AC2_VIDEO_FIT
#include "d3d9_video_fit.h"
#endif
#ifdef AC2_CONTROLLER_PROMPTS
#include "controller_prompts.h"
#endif


namespace {

#ifdef AC2_LEAN_TRACE
constexpr UINT SampleInterval = 240;
#else
constexpr UINT SampleInterval = 60;
#endif
constexpr UINT EventsPerSample = 128;
constexpr UINT ProjectionEventsPerSample = 12;
constexpr UINT LayersPerSample = 32;

HMODULE system_d3d9() {
    static const HMODULE module = [] {
        wchar_t path[MAX_PATH];
        UINT length = GetSystemWow64DirectoryW(path, MAX_PATH);
        if (!length) {
            length = GetSystemDirectoryW(path, MAX_PATH);
        }
        constexpr wchar_t filename[] = L"\\d3d9.dll";
        if (!length || length + sizeof(filename) / sizeof(wchar_t) > MAX_PATH) {
            return static_cast<HMODULE>(nullptr);
        }
        std::wcscpy(path + length, filename);
        return LoadLibraryW(path);
    }();
    return module;
}

template <typename Function>
Function system_export(const char* name) {
    HMODULE module = system_d3d9();
    return module ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
}

bool trace_host() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) {
        return false;
    }
    const wchar_t* separator = std::wcsrchr(path, L'\\');
    const wchar_t* filename = separator ? separator + 1 : path;
    return _wcsicmp(filename, L"AssassinsCreedIIGame.exe") == 0 ||
           _wcsicmp(filename, L"ac2_display_smoke.exe") == 0;
}

struct GameImage {
    std::uintptr_t base{};
    DWORD code_rva{}, code_size{}, image_size{};
};

GameImage game_image() {
    static const GameImage image = [] {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!base) {
            return GameImage{};
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) ||
            dos->e_lfanew > 4096 - static_cast<LONG>(sizeof(IMAGE_NT_HEADERS32))) {
            return GameImage{};
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            return GameImage{};
        }
        return GameImage{base, nt->OptionalHeader.BaseOfCode, nt->OptionalHeader.SizeOfCode,
                         nt->OptionalHeader.SizeOfImage};
    }();
    return image;
}

DWORD code_rva(const void* address) {
    const GameImage image = game_image();
    const auto offset = reinterpret_cast<std::uintptr_t>(address) - image.base;
    return image.base && offset >= image.code_rva && offset - image.code_rva < image.code_size
        ? static_cast<DWORD>(offset) : 0;
}

HANDLE trace_file() {
    static HANDLE file = [] {
        wchar_t temp[MAX_PATH];
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        if (!length || length >= MAX_PATH) {
            return INVALID_HANDLE_VALUE;
        }
        wchar_t filename[MAX_PATH];
        if (swprintf(filename, MAX_PATH, L"%lsac2-display-trace-%lu.csv", temp,
                     GetCurrentProcessId()) < 0) {
            return INVALID_HANDLE_VALUE;
        }
        HANDLE handle = CreateFileW(filename, FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS &&
            swprintf(filename, MAX_PATH, L"%lsac2-display-trace-%lu-%llu.csv", temp,
                     GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64())) >= 0) {
            handle = CreateFileW(filename, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                 nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (handle != INVALID_HANDLE_VALUE) {
            constexpr char header[] = "event,device,frame,x,y,width,height,arg0,arg1\n";
            DWORD written;
            WriteFile(handle, header, sizeof(header) - 1, &written, nullptr);
        }
        return handle;
    }();
    return file;
}

void trace(const char* event, UINT device, UINT frame, long x = 0, long y = 0,
           long width = 0, long height = 0, double arg0 = 0, double arg1 = 0) {
    char line[192];
    const int length = std::snprintf(line, sizeof(line), "%s,%u,%u,%ld,%ld,%ld,%ld,%.6f,%.6f\n",
                                     event, device, frame, x, y, width, height, arg0, arg1);
    if (length > 0 && length < static_cast<int>(sizeof(line))) {
        DWORD written;
        const HANDLE file = trace_file();
        if (file != INVALID_HANDLE_VALUE) {
            WriteFile(file, line, length, &written, nullptr);
        }
    }
}

void trace_rect(const char* event, UINT device, UINT frame, const RECT& rect,
                double arg0 = 0, double arg1 = 0) {
    trace(event, device, frame, rect.left, rect.top, rect.right - rect.left,
          rect.bottom - rect.top, arg0, arg1);
}

void trace_viewport_stack(UINT device, UINT frame, DWORD site) {
    void* entries[16]{};
    const USHORT count = CaptureStackBackTrace(0, 16, entries, nullptr);
    for (USHORT index = 0; index < count; ++index) {
        if (const DWORD entry = code_rva(entries[index])) {
            trace("viewport_stack", device, frame, index, 0, 0, 0, site, entry);
        }
    }
}

#ifdef AC2_NATIVE_VIEW

extern "C" {
[[maybe_unused]] __attribute__((used)) std::uintptr_t ac2_full_view, ac2_fitted_view, ac2_other_view, ac2_perspective;
[[maybe_unused]] __attribute__((used)) std::uintptr_t ac2_hud_layout_resume;
[[maybe_unused]] __attribute__((used)) std::uintptr_t ac2_minimap_clip_resume;
[[maybe_unused]] __attribute__((used)) std::uintptr_t ac2_set_marker_position;
__attribute__((used)) std::atomic<DWORD> ac2_output_width_bits{0}, ac2_output_height_bits{0};
#ifdef AC2_HIGH_REFRESH
[[maybe_unused]] __attribute__((used)) std::uintptr_t ac2_game_frame_wait;
__attribute__((used)) std::atomic<BYTE> ac2_remove_frame_wait{0};
static_assert(sizeof(ac2_remove_frame_wait) == 1 && decltype(ac2_remove_frame_wait)::is_always_lock_free);
#endif
#ifdef AC2_CONTROLLER_PROMPTS
[[maybe_unused]] __attribute__((used)) std::uintptr_t ac2_original_pad, ac2_xinput_pad;
#endif
}

static_assert(sizeof(ac2_output_width_bits) == sizeof(DWORD));
static_assert(decltype(ac2_output_width_bits)::is_always_lock_free);

void set_native_output(UINT width, UINT height) noexcept {
    ac2_output_width_bits = 0;
    ac2_output_height_bits = std::bit_cast<DWORD>(static_cast<float>(height));
    ac2_output_width_bits = std::bit_cast<DWORD>(static_cast<float>(width));
}

bool native_view_is_full(const BYTE* view, float& width, float& height) {
    struct Gate { float x, y, width, height; };
    const BYTE* target;
    std::memcpy(&target, view + 0x178, sizeof(target));
    if (!target) {
        return false;
    }
    std::memcpy(&width, target + 0xc, sizeof(width));
    std::memcpy(&height, target + 0x10, sizeof(height));
    if (!(width > 0 && height > 0) || std::bit_cast<DWORD>(width) != ac2_output_width_bits.load() ||
        std::bit_cast<DWORD>(height) != ac2_output_height_bits.load()) {
        return false;
    }
    Gate gate;
    std::memcpy(&gate, view + 0x64, sizeof(gate));
    return gate.x == 0 && gate.y == 0 && gate.width == 1 && gate.height == 1;
}

struct PerspectiveArguments {
    void* output;
    float near_plane, far_plane, field_of_view, aspect;
};

extern "C" __attribute__((used)) void ac2_adjust_perspective(PerspectiveArguments* args, const BYTE* view) {
    constexpr double reference_aspect = 16.0 / 9.0;
    const double aspect = args->aspect;
    const double original_fov = args->field_of_view;
    DWORD mode;
    std::memcpy(&mode, view + 0x58, sizeof(mode));
    float width, height;
    if (mode != 2 || !(aspect > 0 && aspect < reference_aspect - 0.00001) ||
        !(original_fov > 0 && original_fov < 3.14159265) || !native_view_is_full(view, width, height) ||
        std::abs(aspect - width / height) > 0.002) {
        return;
    }
    const double tangent = std::tan(original_fov * 0.5) * reference_aspect / aspect;
    args->field_of_view = static_cast<float>(2.0 * std::atan(tangent));
    static std::atomic<UINT> samples{0};
    if (++samples <= 12) {
        trace("native_camera", 0, 0, 0, 0, static_cast<long>(width), static_cast<long>(height),
              original_fov, args->field_of_view);
    }
}

// At this point EAX is the aspect mode minus one, ECX is View* and EDX is
// the associated render target. Select the full branch for the output view.
extern "C" __attribute__((naked)) void ac2_choose_viewport() {
    __asm__ volatile(
        "subl $1, %eax\n\t"
        "jnz 1f\n\t"
        "cmpl $0, 0x50(%ecx)\n\t"
        "jne 2f\n\t"
        "pushl %eax\n\t"
        "movl 0xc(%edx), %eax\n\t"
        "testl %eax, %eax\n\t"
        "jz 3f\n\t"
        "cmpl _ac2_output_width_bits, %eax\n\t"
        "jne 3f\n\t"
        "movl 0x10(%edx), %eax\n\t"
        "testl %eax, %eax\n\t"
        "jz 3f\n\t"
        "cmpl _ac2_output_height_bits, %eax\n\t"
        "jne 3f\n\t"
        "popl %eax\n\t"
        "jmp *_ac2_full_view\n\t"
        "1: jmp *_ac2_other_view\n\t"
        "3: popl %eax\n\t"
        "2: jmp *_ac2_fitted_view\n\t");
}

// Preserve the original stack and tail-call the game's perspective builder.
// ESI is the View* at this one call site.
extern "C" __attribute__((naked)) void ac2_prepare_perspective() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "movl %esp, %ebp\n\t"
        "leal 40(%ebp), %eax\n\t"
        "andl $-16, %esp\n\t"
        "subl $8, %esp\n\t"
        "pushl %esi\n\t"
        "pushl %eax\n\t"
        "call _ac2_adjust_perspective\n\t"
        "movl %ebp, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_ac2_perspective\n\t");
}

constexpr float HudStageWidth = 1280.f, HudStageHeight = 720.f;

struct HudLayout {
    float scale, left, top;
};

struct HudOrigin {
    SHORT x, y;
};

struct FlashBounds {
    LONG left, right, top, bottom;
};

struct HudAnchor {
    int x, y;
};

constexpr HudAnchor HudTopLeft{-1, -1}, HudTopRight{1, -1};
constexpr HudAnchor HudBottomLeft{-1, 1}, HudBottomRight{1, 1};

HudLayout fit_hud(float width, float height) {
    if (!(width > 0 && height > 0 && std::isfinite(width) && std::isfinite(height))) {
        return {1.f, 0.f, 0.f};
    }
    const float scale = std::min(width / HudStageWidth, height / HudStageHeight);
    return {scale, std::max(0.f, (width - HudStageWidth * scale) * .5f),
                   std::max(0.f, (height - HudStageHeight * scale) * .5f)};
}

HudOrigin anchored_hud_origin(HudOrigin original, const HudLayout& layout, HudAnchor anchor) {
    const auto coordinate = [](long value) { return static_cast<SHORT>(std::clamp(value, -32768L, 32767L)); };
    return {coordinate(original.x + std::lround(anchor.x * layout.left / layout.scale)),
            coordinate(original.y + std::lround(anchor.y * layout.top / layout.scale))};
}

HudOrigin action_hud_origin(HudOrigin original, const HudLayout& layout, const FlashBounds& bounds) {
    constexpr float stage_right_margin = 96.f;
    constexpr float authored_right = 1092.f;  // Bounds of the standard four-button HUD
    constexpr float twips_per_stage_pixel = 20.f;
    const float measured_right = bounds.right / twips_per_stage_pixel;
    const bool stage_bounds = bounds.left >= 0 && bounds.left < bounds.right && bounds.top < bounds.bottom &&
        measured_right >= 0.65f * HudStageWidth && measured_right <= HudStageWidth;
    const float right = stage_bounds ? measured_right : authored_right;
    const long shift = std::lround(HudStageWidth - stage_right_margin - right);
    HudOrigin origin = anchored_hud_origin(original, layout, HudTopRight);
    origin.x = static_cast<SHORT>(std::clamp(origin.x + shift, -32768L, 32767L));
    return origin;
}

using FireRender = int* (__attribute__((thiscall)) *)(void*, int*);
using FireSetOrigin = int* (__attribute__((thiscall)) *)(void*, int*, SHORT, SHORT);
using FlashFindThread = void* (__attribute__((thiscall)) *)(void*, void*, const char*, int);
using FlashGetProperty = double (__attribute__((thiscall)) *)(void*, void*, int);
using FlashSetProperty = int (__attribute__((thiscall)) *)(void*, void*, int, const double*);
using FlashGetBounds = void (*)(const BYTE*, FlashBounds*, int);
using FireLock = void (__attribute__((thiscall)) *)(void*);
FireRender original_fire_render;
FireSetOrigin set_fire_origin;
FlashFindThread find_flash_thread;
FlashGetProperty get_flash_property;
FlashSetProperty set_flash_property;
FlashGetBounds get_flash_bounds;
FireLock lock_fire_player, unlock_fire_player;
std::uintptr_t master_hud_vtable;
using FireLoadMovie = int* (__attribute__((thiscall)) *)(void*, int*, const BYTE*, DWORD, void*);
FireLoadMovie original_replace_movie;
std::atomic<void*> weapon_movie_player, shop_movie_player;
std::uintptr_t pause_menu_vtable, weapon_component_vtable, shop_component_vtable;
thread_local bool rendering_weapon_wheel, rendering_shop;
thread_local HudOrigin minimap_clip_shift;

constexpr DWORD HudMiniMapVtable = 0x019e8190;

struct ShopQuad {
    float left, top, right, bottom;
    bool translucent_white;
};

using FireDrawTriangles = void (__attribute__((thiscall)) *)(const BYTE*, const BYTE*, UINT, UINT);
FireDrawTriangles original_fire_triangles;
thread_local const ShopQuad* shop_quad;

ShopQuad read_shop_quad(const BYTE* vertices, UINT stride, DWORD color) {
    ShopQuad bounds{};
    float positions[6][2];
    for (unsigned index = 0; index < 6; ++index) {
        std::memcpy(positions[index], vertices + index * stride, sizeof(positions[index]));
        const auto [x, y] = positions[index];
        if (!std::isfinite(x) || !std::isfinite(y)) {
            return {};
        }
        bounds.left = index ? std::min(bounds.left, x) : x;
        bounds.right = index ? std::max(bounds.right, x) : x;
        bounds.top = index ? std::min(bounds.top, y) : y;
        bounds.bottom = index ? std::max(bounds.bottom, y) : y;
    }
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return {};
    }
    unsigned corners = 0;
    for (const auto& [x, y] : positions) {
        if ((x != bounds.left && x != bounds.right) || (y != bounds.top && y != bounds.bottom)) {
            return {};
        }
        corners |= 1u << ((x == bounds.right ? 1u : 0u) | (y == bounds.bottom ? 2u : 0u));
    }
    if (corners != 15u) {
        return {};
    }
    const BYTE alpha = static_cast<BYTE>(color >> 24);
    bounds.translucent_white = alpha > 0 && alpha < 255 &&
        (color & 0x00ffffffu) == 0x00ffffffu;
    return bounds;
}

void __attribute__((thiscall)) draw_shop_triangles(const BYTE* renderer, const BYTE* vertices, UINT count, UINT mode) {
    const ShopQuad* previous = shop_quad;
    ShopQuad quad{};
    if (rendering_shop && count == 6 && mode <= 3 && vertices) {
        DWORD color;
        std::memcpy(&color, renderer + 0x14, sizeof(color));
        quad = read_shop_quad(vertices, mode == 2 || mode == 3 ? 24 : 20, color);
        shop_quad = &quad;
    }
    original_fire_triangles(renderer, vertices, count, mode);
    shop_quad = previous;
}

// MapBitmap initializes two triangles for its clip mesh with fixed stage
// coordinates. The movie's origin has already moved the input geometry.
extern "C" __attribute__((used)) void ac2_shift_minimap_clip(float* vertices) {
    if (minimap_clip_shift.x == 0 && minimap_clip_shift.y == 0) {
        return;
    }
    for (unsigned vertex = 0; vertex < 6; ++vertex) {
        vertices[vertex * 5] += minimap_clip_shift.x;
        vertices[vertex * 5 + 1] += minimap_clip_shift.y;
    }
}

extern "C" __attribute__((naked)) void ac2_prepare_minimap_clip() {
    __asm__ volatile(
        "pushal\n\t"
        "movl %esp, %ebx\n\t"
        "andl $-16, %esp\n\t"
        "subl $12, %esp\n\t"
        "pushl %eax\n\t"
        "call _ac2_shift_minimap_clip\n\t"
        "movl %ebx, %esp\n\t"
        "popal\n\t"
        "movss (%ecx), %xmm0\n\t"
        "movss 0x14(%ecx), %xmm1\n\t"
        "jmp *_ac2_minimap_clip_resume\n\t");
}

int* __attribute__((thiscall)) replace_fire_movie(void* player, int* status, const BYTE* data,
                                                  DWORD length, void* options) {
    constexpr BYTE weapon_header[] = {'U', 'E', 'F', 8, 0x22, 0x8d, 0, 0};
    constexpr BYTE weapon_background[] = {0x43, 0x02, 0x33, 0x33, 0x33};
    constexpr BYTE shop_header[] = {'U', 'E', 'F', 8, 0x9f, 0xcf, 0x09, 0};
    constexpr BYTE shop_background[] = {0x43, 0x02, 0x66, 0x99, 0x99};
    const bool weapon = data && length >= 0x8d22 &&
        std::memcmp(data, weapon_header, sizeof(weapon_header)) == 0 &&
        std::memcmp(data + 0x4ed, weapon_background, sizeof(weapon_background)) == 0 &&
        std::memcmp(data + 0x121b, "weaponSelector", sizeof("weaponSelector")) == 0;
    const bool shop = data && length >= 0x9cf9f &&
        std::memcmp(data, shop_header, sizeof(shop_header)) == 0 &&
        std::memcmp(data + 0x14c, shop_background, sizeof(shop_background)) == 0 &&
        std::memcmp(data + 0x6421, "MenuShop", sizeof("MenuShop")) == 0;
    int* result = original_replace_movie(player, status, data, length, options);
    if (weapon && *status >= 0) {
        weapon_movie_player = player;
        trace("weapon_movie_loaded", 0, 0, 0, 0, length, *status);
    } else {
        void* expected = player;
        weapon_movie_player.compare_exchange_strong(expected, nullptr);
    }
    if (shop && *status >= 0) {
        shop_movie_player = player;
        trace("shop_movie_loaded", 0, 0, 0, 0, length, *status);
    } else {
        void* expected = player;
        shop_movie_player.compare_exchange_strong(expected, nullptr);
    }
    return result;
}

struct AnchoredMovie {
    DWORD vtable, destructor;
    HudAnchor anchor;
    std::uintptr_t runtime_vtable{};
};

AnchoredMovie hud_movies[] = {
    {0x019e8e80, 0x00957940, HudTopRight},   // Database discovery card
    {HudMiniMapVtable, 0x0094ab40, HudBottomRight}, // Minimap and range labels
    {0x019e8db8, 0x00956850, HudTopRight},   // Standalone action hints
    {0x019e6e98, 0x009389e0, HudBottomLeft}, // Equipped weapon display
    {0x019e8680, 0x00952d90, HudTopLeft},    // Health and notoriety
};

int* render_master_hud(void* player, int* status, const BYTE* private_player, const HudLayout& layout,
                       float width, float height) {
    const auto pointer_at = [](const BYTE* owner, size_t offset) {
        BYTE* pointer = nullptr;
        if (owner) {
            std::memcpy(&pointer, owner + offset, sizeof(pointer));
        }
        return pointer;
    };
    auto* const lock_owner = pointer_at(private_player, 0x2c);
    if (!lock_owner || !private_player[0x118]) {
        return original_fire_render(player, status);
    }

    struct Clip {
        const char* path;
        unsigned pass;
        void* target{};
    };
    Clip clips[] = {{"/HUD_Controls", 1}, {"/HUD_Money", 2}, {"/HUD_Equipment", 2},
                    {"/HUD_Subtitles", 0}, {"/HUD_Party", 0}, {"/mcLogo", 0}};
    constexpr HudAnchor anchors[] = {{0, 0}, HudTopRight, HudBottomLeft};
    constexpr int visible_property = 7;
    unsigned visible_by_pass[std::size(anchors)]{};
    unsigned found = 0;
    HudOrigin original_origin{};
    FlashBounds action_bounds{};

    auto* const lock = lock_owner + 8;
    lock_fire_player(lock);
    BYTE* const movie = pointer_at(private_player, 0x4c);
    BYTE* const core = pointer_at(pointer_at(movie, 4), 4);
    if (core) {
        for (size_t index = 0; index < std::size(clips); ++index) {
            auto& clip = clips[index];
            clip.target = find_flash_thread(core, nullptr, clip.path, 1);
            if (!clip.target) {
                clip.target = find_flash_thread(core, nullptr, clip.path + 1, 1);
            }
            if (!clip.target) {
                continue;
            }
            const unsigned bit = 1u << index;
            const double visible = get_flash_property(core, clip.target, visible_property);
            if (visible == 0 || visible == 1) {
                found |= bit;
                if (visible == 1) {
                    visible_by_pass[clip.pass] |= bit;
                }
            }
            if (index == 0 && visible == 1 && get_flash_bounds) {
                auto* thread = static_cast<BYTE*>(clip.target);
                if (pointer_at(thread, 0x40)) {
                    if (const auto* display = pointer_at(thread, 0x6c)) {
                        get_flash_bounds(display, &action_bounds, 0);
                    }
                }
            }
        }
        std::memcpy(&original_origin, private_player + 0xcc, sizeof(original_origin));
    }
    unlock_fire_player(lock);

    const unsigned original_visible = visible_by_pass[0] | visible_by_pass[1] | visible_by_pass[2];
    if (!core || (found & 7) != 7 || !(original_visible & 7)) {
        return original_fire_render(player, status);
    }

    unsigned current_visible = original_visible;
    const auto update_visibility = [&](unsigned next_visible) {
        lock_fire_player(lock);
        bool success = true;
        for (size_t index = 0; index < std::size(clips); ++index) {
            const unsigned bit = 1u << index;
            if (!((current_visible ^ next_visible) & bit)) {
                continue;
            }
            const double visible = (next_visible & bit) != 0;
            if (set_flash_property(core, clips[index].target, visible_property, &visible)) {
                current_visible ^= bit;
            } else {
                success = false;
            }
        }
        unlock_fire_player(lock);
        return success;
    };

    unsigned drawn = 0;
    int origin_status;
    int* result = status;
    for (size_t pass = 0; pass < std::size(anchors); ++pass) {
        if (!visible_by_pass[pass]) {
            continue;
        }
        if (!update_visibility(visible_by_pass[pass])) {
            break;
        }
        const HudOrigin origin = pass == clips[0].pass ? action_hud_origin(original_origin, layout, action_bounds) :
                                                         anchored_hud_origin(original_origin, layout, anchors[pass]);
        set_fire_origin(player, &origin_status, origin.x, origin.y);
        if (origin_status != 0) {
            break;
        }
        result = original_fire_render(player, status);
        if (*status < 0) {
            break;
        }
        if (pass == clips[0].pass) {
            static thread_local float action_width = 0, action_height = 0;
            if (width != action_width || height != action_height) {
                action_width = width;
                action_height = height;
                trace("hud_action_anchor", 0, 0, origin.x - original_origin.x, origin.y - original_origin.y,
                      static_cast<long>(width), static_cast<long>(height), action_bounds.left, action_bounds.right);
            }
        }
        drawn |= visible_by_pass[pass];
    }
    set_fire_origin(player, &origin_status, original_origin.x, original_origin.y);
    update_visibility(original_visible);
    if (!drawn) {
        return original_fire_render(player, status);
    }

    static thread_local float last_width = 0, last_height = 0;
    static thread_local unsigned last_drawn = 0;
    if (width != last_width || height != last_height || drawn != last_drawn) {
        last_width = width;
        last_height = height;
        last_drawn = drawn;
        trace("hud_master_passes", 0, 0, 0, 0, static_cast<long>(width), static_cast<long>(height), found, drawn);
    }
    return result;
}

extern "C" __attribute__((used)) int* ac2_render_hud(const BYTE* item, void* player, int* status) {
    std::uintptr_t vtable;
    std::memcpy(&vtable, item, sizeof(vtable));
    const auto* movie = std::find_if(std::begin(hud_movies), std::end(hud_movies),
        [vtable](const AnchoredMovie& entry) { return entry.runtime_vtable == vtable; });
    if (vtable != master_hud_vtable && movie == std::end(hud_movies)) {
        const bool previous = rendering_weapon_wheel;
        const bool was_shop = rendering_shop;
        rendering_shop = player && player == shop_movie_player.load();
        if (vtable == pause_menu_vtable) {
            const std::uintptr_t* active_component;
            std::memcpy(&active_component, item + 0x88, sizeof(active_component));
            rendering_shop = rendering_shop || (active_component && *active_component == shop_component_vtable);
            rendering_weapon_wheel = (player && player == weapon_movie_player.load()) ||
                                     (active_component && *active_component == weapon_component_vtable);
            static thread_local bool last_weapon_frame = false;
            if (rendering_weapon_wheel != last_weapon_frame) {
                last_weapon_frame = rendering_weapon_wheel;
                trace("weapon_wheel_state", 0, 0, last_weapon_frame, 0);
            }
        }
        int* result = original_fire_render(player, status);
        rendering_weapon_wheel = previous;
        rendering_shop = was_shop;
        return result;
    }
    const float width = std::bit_cast<float>(ac2_output_width_bits.load());
    const float height = std::bit_cast<float>(ac2_output_height_bits.load());
    const HudLayout layout = fit_hud(width, height);
    if (!player || (layout.left == 0 && layout.top == 0)) {
        return original_fire_render(player, status);
    }
    const BYTE* private_player;
    std::memcpy(&private_player, static_cast<BYTE*>(player) + 4, sizeof(private_player));
    if (!private_player) {
        return original_fire_render(player, status);
    }
    if (vtable == master_hud_vtable) {
        return render_master_hud(player, status, private_player, layout, width, height);
    }
    HudOrigin old{};
    std::memcpy(&old, private_player + 0xcc, sizeof(old));
    const HudOrigin origin = anchored_hud_origin(old, layout, movie->anchor);
    if (old.x == origin.x && old.y == origin.y) {
        return original_fire_render(player, status);
    }
    int set_status;
    set_fire_origin(player, &set_status, origin.x, origin.y);
    const HudOrigin previous_shift = minimap_clip_shift;
    if (set_status == 0 && movie->vtable == HudMiniMapVtable) {
        minimap_clip_shift = {static_cast<SHORT>(origin.x - old.x), static_cast<SHORT>(origin.y - old.y)};
    }
    int* result = original_fire_render(player, status);
    minimap_clip_shift = previous_shift;
    if (set_status == 0) {
        set_fire_origin(player, &set_status, old.x, old.y);
        struct Output { float width, height; };
        static thread_local Output last[std::size(hud_movies)]{};
        auto& dimensions = last[movie - hud_movies];
        if (*status >= 0 && (dimensions.width != width || dimensions.height != height)) {
            dimensions = {width, height};
            trace("hud_movie_anchor", 0, 0, origin.x - old.x, origin.y - old.y,
                  static_cast<long>(width), static_cast<long>(height), movie->vtable);
        }
    }
    return result;
}

extern "C" __attribute__((naked)) void ac2_prepare_fire_render() {
    __asm__ volatile(
        "pushl %ebp\n\t"
        "movl %esp, %ebp\n\t"
        "andl $-16, %esp\n\t"
        "subl $4, %esp\n\t"
        "pushl 8(%ebp)\n\t"
        "pushl %ecx\n\t"
        "pushl %edi\n\t"
        "call _ac2_render_hud\n\t"
        "leave\n\t"
        "ret $4\n\t");
}

bool expand_popup_projection(float* matrix, float width, float height) {
    const HudLayout layout = fit_hud(width, height);
    const float span_x = HudStageWidth * layout.scale / width;
    const float span_y = HudStageHeight * layout.scale / height;
    if (std::abs(span_x - 1) < .00001f && std::abs(span_y - 1) < .00001f) {
        return false;
    }
    const auto approximately = [](float value, float expected) {
        return std::abs(value - expected) < .00001f;
    };
    if (matrix[15] != 1 || matrix[1] != 0 || matrix[4] != 0 ||
        !approximately(matrix[0], 2.f * span_x / HudStageWidth) ||
        !approximately(matrix[5], -2.f * span_y / HudStageHeight)) {
        return false;
    }
    const bool row_vector = approximately(matrix[12], -span_x) && approximately(matrix[13], span_y) &&
                            matrix[3] == 0 && matrix[7] == 0 && matrix[11] == 0;
    const bool column_vector = approximately(matrix[3], -span_x) && approximately(matrix[7], span_y) &&
                               matrix[12] == 0 && matrix[13] == 0 && matrix[14] == 0;
    if (!row_vector && !column_vector) {
        return false;
    }
    matrix[0] = 2.f / HudStageWidth;
    matrix[5] = -2.f / HudStageHeight;
    matrix[row_vector ? 12 : 3] = -1.f;
    matrix[row_vector ? 13 : 7] = 1.f;
    return true;
}

bool expand_weapon_projection(float* matrix, float width, float height) {
    if (expand_popup_projection(matrix, width, height)) {
        return true;
    }
    // The wheel's solid quad can use a projection centered on the viewport.
    // Scale its clip-space axes around that center while preserving depth and W.
    constexpr float epsilon = .00001f;
    if (!(matrix[0] > 0 && matrix[0] < .01f && matrix[5] < 0 && matrix[5] > -.01f) ||
        std::abs(matrix[15] - 1.f) > epsilon) {
        return false;
    }
    for (const unsigned index : {1u, 2u, 3u, 4u, 6u, 7u, 8u, 9u, 11u, 12u, 13u}) {
        if (!(std::abs(matrix[index]) < epsilon)) {
            return false;
        }
    }
    const HudLayout layout = fit_hud(width, height);
    if (!layout.left && !layout.top) {
        return false;
    }
    matrix[0] *= width / (HudStageWidth * layout.scale);
    matrix[5] *= height / (HudStageHeight * layout.scale);
    return true;
}

bool expand_shop_quad(float* matrix, float width, float height, const ShopQuad& quad) {
    if (!quad.translucent_white || width <= 0 || height <= 0 || matrix[0] <= 0 || matrix[5] >= 0 ||
        matrix[15] != 1 || matrix[1] != 0 || matrix[2] != 0 || matrix[4] != 0 || matrix[6] != 0 ||
        matrix[8] != 0 || matrix[9] != 0 || matrix[11] != 0) {
        return false;
    }
    const bool row_vector = matrix[3] == 0 && matrix[7] == 0;
    if (!row_vector && (matrix[12] != 0 || matrix[13] != 0 || matrix[14] != 0)) {
        return false;
    }
    float& x = matrix[row_vector ? 12 : 3];
    float& y = matrix[row_vector ? 13 : 7];
    const HudLayout layout = fit_hud(width, height);
    const float span_x = HudStageWidth * layout.scale / width;
    const float span_y = HudStageHeight * layout.scale / height;
    const auto approximately = [](float a, float b) { return std::abs(a - b) < .002f; };
    if ((!layout.left && !layout.top) ||
        !approximately(quad.left * matrix[0] + x, -span_x) || !approximately(quad.right * matrix[0] + x, span_x) ||
        !approximately(quad.top * matrix[5] + y, span_y) || !approximately(quad.bottom * matrix[5] + y, -span_y)) {
        return false;
    }
    matrix[0] = 2.f / (quad.right - quad.left);
    matrix[5] = -2.f / (quad.bottom - quad.top);
    x = -1.f - matrix[0] * quad.left;
    y = 1.f - matrix[5] * quad.top;
    return true;
}

std::uint64_t texture_fingerprint(IDirect3DTexture9* texture, int rows, int row_bytes) {
    D3DLOCKED_RECT level{};
    if (FAILED(texture->LockRect(0, &level, nullptr, D3DLOCK_READONLY))) {
        return 0;
    }
    std::uint64_t hash = 0;
    if (level.Pitch >= row_bytes) {
        hash = 0xcbf29ce484222325ull;
        for (int row = 0; row < rows; ++row) {
            const BYTE* scan = static_cast<const BYTE*>(level.pBits) + row * level.Pitch;
            for (int column = 0; column < row_bytes; ++column) {
                hash = (hash ^ scan[column]) * 0x100000001b3ull;
            }
        }
    }
    texture->UnlockRect(0);
    return hash;
}

bool is_popup_texture(IDirect3DBaseTexture9* texture) {
    if (!texture || texture->GetType() != D3DRTYPE_TEXTURE) {
        return false;
    }
    constexpr GUID key{0x25fb6a63, 0x80fc, 0x4caa, {0xac, 0x05, 0x28, 0x5e, 0x97, 0x6c, 0xe3, 0xd7}};
    BYTE match = 0;
    DWORD bytes = sizeof(match);
    if (SUCCEEDED(texture->GetPrivateData(key, &match, &bytes))) {
        return match != 0;
    }
    auto* color = static_cast<IDirect3DTexture9*>(texture);
    D3DSURFACE_DESC desc{};
    if (FAILED(color->GetLevelDesc(0, &desc))) {
        return false;
    }
    if (!desc.Usage && desc.Width == 16 && desc.Height == 512 && desc.Format == D3DFMT_DXT3) {
        // menus-popups-bgGradient_Map
        match = texture_fingerprint(color, 128, 64) == 0xc3c9b925c21f47fdull;
    } else if (!desc.Usage && desc.Width == 4 && desc.Height == 256 && desc.Format == D3DFMT_DXT1) {
        // Animus_Popup_BackgroundGradient_Map
        match = texture_fingerprint(color, 64, 8) == 0xcba261394f323ea7ull;
    }
    texture->SetPrivateData(key, &match, sizeof(match), 0);
    if (match) {
        trace("popup_texture", 0, 0, 0, 0, desc.Width, desc.Height, desc.Format, desc.Pool);
    }
    return match != 0;
}

extern "C" __attribute__((used)) float ac2_center_hud(float width, float height, float* margins) {
    const HudLayout layout = fit_hud(width, height);
    margins[0] = layout.left;
    margins[1] = layout.top;
    static thread_local float last_width = 0, last_height = 0;
    if (width > 0 && height > 0 && (width != last_width || height != last_height)) {
        last_width = width;
        last_height = height;
        trace("hud_layout", 0, 0, 0, 0, static_cast<long>(width), static_cast<long>(height),
              layout.left, layout.top);
    }
    return layout.scale;
}

extern "C" __attribute__((used)) void ac2_place_world_marker(void* stack_arguments, const void* caller) {
    const float width = std::bit_cast<float>(ac2_output_width_bits.load());
    const float height = std::bit_cast<float>(ac2_output_height_bits.load());
    if (!width || !height) {
        return;
    }
    const HudLayout layout = fit_hud(width, height);
    auto* coordinates = static_cast<BYTE*>(stack_arguments) + sizeof(void*);
    double position[2];
    std::memcpy(position, coordinates, sizeof(position));
    position[0] = (position[0] * (width / HudStageWidth) - layout.left) / layout.scale;
    position[1] = (position[1] * (height / HudStageHeight) - layout.top) / layout.scale;
    std::memcpy(coordinates, position, sizeof(position));
    static std::atomic<UINT> samples{0};
    if (samples.load(std::memory_order_relaxed) < 12 && ++samples <= 12) {
        trace("world_marker", 0, 0, 0, 0, static_cast<long>(width), static_cast<long>(height),
              code_rva(caller), position[1]);
    }
}

// The game has loaded the viewport width and height in XMM0 and XMM2. Write
// the two local translation components and return its scale in XMM1.
extern "C" __attribute__((naked)) void ac2_prepare_hud_layout() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "movl %esp, %esi\n\t"
        "andl $-16, %esp\n\t"
        "subl $16, %esp\n\t"
        "movss %xmm0, (%esp)\n\t"
        "movss %xmm2, 4(%esp)\n\t"
        "leal -0x30(%ebp), %eax\n\t"
        "movl %eax, 8(%esp)\n\t"
        "call _ac2_center_hud\n\t"
        "fstps (%esp)\n\t"
        "movss (%esp), %xmm1\n\t"
        "movl %esi, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_ac2_hud_layout_resume\n\t");
}

extern "C" __attribute__((naked)) void ac2_prepare_world_marker() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "movl %esp, %esi\n\t"
        "leal 40(%esi), %eax\n\t"
        "andl $-16, %esp\n\t"
        "subl $8, %esp\n\t"
        "pushl 36(%esi)\n\t"
        "pushl %eax\n\t"
        "call _ac2_place_world_marker\n\t"
        "movl %esi, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_ac2_set_marker_position\n\t");
}

#ifdef AC2_HIGH_REFRESH
extern "C" __attribute__((naked)) void ac2_frame_wait() {
    __asm__ volatile(
        "cmpb $0, _ac2_remove_frame_wait\n\t"
        "jne 1f\n\t"
        "jmp *_ac2_game_frame_wait\n\t"
        "1: ret\n\t");
}

void update_frame_wait(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& parameters, UINT id, UINT frame) {
    UINT refresh = parameters.FullScreen_RefreshRateInHz;
    if (refresh == 0) {
        D3DDISPLAYMODE mode{};
        if (SUCCEEDED(device->GetDisplayMode(0, &mode))) {
            refresh = mode.RefreshRate;
        }
    }
    ac2_remove_frame_wait = refresh > 60;
    trace("frame_wait_mode", id, frame, 0, 0, 0, 0, refresh, ac2_remove_frame_wait.load());
}
#endif

bool matching_code(const GameImage& image, DWORD site, const BYTE* expected, size_t size) {
    if (!image.base || site < image.code_rva || site + size > image.code_rva + image.code_size) {
        return false;
    }
    const auto* code = reinterpret_cast<const BYTE*>(image.base + site);
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(code, &region, sizeof(region)) && region.State == MEM_COMMIT &&
           region.Type == MEM_IMAGE && region.AllocationBase == reinterpret_cast<void*>(image.base) &&
           (region.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
           !(region.Protect & PAGE_GUARD) &&
           reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize >= image.base + site + size &&
           std::memcmp(code, expected, size) == 0;
}

bool replace_code(std::uintptr_t site, const BYTE* before, const BYTE* after, size_t size) {
    auto* code = reinterpret_cast<BYTE*>(site);
    DWORD protection;
    if (!VirtualProtect(code, size, PAGE_EXECUTE_WRITECOPY, &protection)) {
        return false;
    }
    std::memcpy(code, after, size);
    DWORD ignored;
    if (FlushInstructionCache(GetCurrentProcess(), code, size) &&
        VirtualProtect(code, size, protection, &ignored)) {
        return true;
    }
    std::memcpy(code, before, size);
    FlushInstructionCache(GetCurrentProcess(), code, size);
    VirtualProtect(code, size, protection, &ignored);
    return false;
}

template<class Function>
bool redirect_code(std::uintptr_t site, const BYTE* expected, BYTE opcode, Function destination, BYTE (&replacement)[5]) {
    const auto displacement = reinterpret_cast<std::uintptr_t>(destination) - (site + sizeof(replacement));
    static_assert(sizeof(displacement) == 4);
    replacement[0] = opcode;
    std::memcpy(replacement + 1, &displacement, sizeof(displacement));
    return replace_code(site, expected, replacement, sizeof(replacement));
}

void apply_hud_anchors(const GameImage& image) {
    constexpr DWORD draw_site = 0x01053bfa, draw_target = 0x013ac1d0;
    constexpr DWORD set_origin = 0x013ab790;
    constexpr BYTE draw_code[] = {0xe8, 0xd1, 0x85, 0x35, 0, 0x85, 0xf6};
    constexpr BYTE draw_prefix[] = {0x56, 0x8b, 0xf1, 0x57, 0x8b, 0x7e, 0x04};
    constexpr BYTE origin_code[] = {0x56, 0x57, 0x8b, 0xf9, 0x8b, 0x77, 0x04, 0x85, 0xf6};
    if (!matching_code(image, draw_site, draw_code, sizeof(draw_code)) ||
        !matching_code(image, draw_target, draw_prefix, sizeof(draw_prefix)) ||
        !matching_code(image, set_origin, origin_code, sizeof(origin_code))) {
        trace("hud_anchors_skipped", 0, 0);
        return;
    }
    const auto verified_vtable = [&image](DWORD rva, DWORD destructor) -> std::uintptr_t {
        if (image.image_size < rva + sizeof(std::uintptr_t)) {
            return 0;
        }
        const auto address = image.base + rva;
        return *reinterpret_cast<const std::uintptr_t*>(address) == image.base + destructor ? address : 0;
    };
    unsigned movie_count = 0;
    for (auto& movie : hud_movies) {
        movie.runtime_vtable = verified_vtable(movie.vtable, movie.destructor);
        movie_count += movie.runtime_vtable != 0;
    }
    constexpr DWORD find_thread = 0x013ed2d0, get_property = 0x013f0670, set_property = 0x013ec060;
    constexpr BYTE find_code[] = {0x8b, 0x44, 0x24, 0x04, 0x85, 0xc0, 0x74, 0x11, 0x8b, 0x54, 0x24, 0x08};
    constexpr BYTE get_code[] = {0x83, 0xec, 0x10, 0x8b, 0x54, 0x24, 0x18, 0x56, 0x8b, 0xf1};
    constexpr BYTE set_code[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xc0, 0x83, 0xec, 0x34, 0x53};
    constexpr BYTE lock_call[] = {0x8b, 0x4f, 0x2c, 0x83, 0xc1, 0x08, 0xe8, 0xdb, 0x96, 0x00, 0x00};
    constexpr BYTE unlock_call[] = {0x8b, 0x4f, 0x2c, 0x83, 0xc1, 0x08, 0xe8, 0x93, 0x96, 0x00, 0x00};
    BYTE replacement[5];
    const auto master = verified_vtable(0x019dbc28, 0x0088a1d0);
    if (master && matching_code(image, find_thread, find_code, sizeof(find_code)) &&
        matching_code(image, get_property, get_code, sizeof(get_code)) &&
        matching_code(image, set_property, set_code, sizeof(set_code)) &&
        matching_code(image, 0x013ac1ea, lock_call, sizeof(lock_call)) &&
        matching_code(image, 0x013ac242, unlock_call, sizeof(unlock_call))) {
        find_flash_thread = reinterpret_cast<FlashFindThread>(image.base + find_thread);
        get_flash_property = reinterpret_cast<FlashGetProperty>(image.base + get_property);
        set_flash_property = reinterpret_cast<FlashSetProperty>(image.base + set_property);
        constexpr DWORD bounds = 0x0140bc00;
        constexpr BYTE width_property[] = {0x6a, 0x00, 0x8d, 0x54, 0x24, 0x40, 0x52, 0x51,
                                           0xe8, 0x3c, 0x21, 0x02, 0x00, 0x8b, 0x44, 0x24, 0x4c};
        if (matching_code(image, 0x013e9ab7, width_property, sizeof(width_property))) {
            get_flash_bounds = reinterpret_cast<FlashGetBounds>(image.base + bounds);
        }
        lock_fire_player = reinterpret_cast<FireLock>(image.base + 0x013b58d0);
        unlock_fire_player = reinterpret_cast<FireLock>(image.base + 0x013b58e0);
        master_hud_vtable = master;
    }
    original_fire_render = reinterpret_cast<FireRender>(image.base + draw_target);
    set_fire_origin = reinterpret_cast<FireSetOrigin>(image.base + set_origin);
    if (redirect_code(image.base + draw_site, draw_code, 0xe8, ac2_prepare_fire_render, replacement)) {
        trace("hud_anchors_applied", 0, 0, movie_count, master_hud_vtable != 0, 0, 0, draw_site);
    } else {
        trace("hud_anchors_skipped", 0, 0, 0, 0, 0, 0, draw_site, GetLastError());
        return;
    }
    constexpr DWORD clip_site = 0x00f95965;
    constexpr BYTE clip_code[] = {0xf3, 0x0f, 0x10, 0x01, 0xf3, 0x0f, 0x10, 0x49, 0x14,
                                  0xf3, 0x0f, 0x10, 0x51, 0x18};
    constexpr BYTE clip_vertices[] = {0xf3, 0x0f, 0x11, 0x40, 0x68, 0xf3, 0x0f, 0x11, 0x40, 0x40,
                                      0xf3, 0x0f, 0x11, 0x40, 0x2c};
    if (matching_code(image, clip_site, clip_code, sizeof(clip_code)) &&
        matching_code(image, clip_site - sizeof(clip_vertices), clip_vertices, sizeof(clip_vertices))) {
        ac2_minimap_clip_resume = image.base + clip_site + 9;
        if (redirect_code(image.base + clip_site, clip_code, 0xe9, ac2_prepare_minimap_clip, replacement)) {
            trace("minimap_clip_applied", 0, 0, 0, 0, 0, 0, clip_site);
        }
    }
    constexpr BYTE active_component_code[] = {0x83, 0xb9, 0x88, 0x00, 0x00, 0x00, 0x00, 0x74, 0x0d,
                                               0x8b, 0x89, 0x88, 0x00, 0x00, 0x00};
    const auto pause = verified_vtable(0x019d78f0, 0x008462f0);
    const auto weapon = verified_vtable(0x019c5f98, 0x0072abc0);
    if (pause && weapon && matching_code(image, 0x00846190, active_component_code, sizeof(active_component_code))) {
        weapon_component_vtable = weapon;
        shop_component_vtable = verified_vtable(0x019d15c0, 0x007d2b00);
        pause_menu_vtable = pause;
        trace("weapon_veil_applied", 0, 0, 0, 0, 0, 0, 0x019d78f0);
    }
    constexpr DWORD triangles_target = 0x0105aac0;
    constexpr BYTE triangles_prefix[] = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf1, 0x8b, 0x4e, 0x0c};
    constexpr BYTE untextured_call[] = {0xe8, 0x29, 0xf0, 0xff, 0xff};
    constexpr BYTE textured_call[] = {0xe8, 0xcc, 0xee, 0xff, 0xff};
    if (shop_component_vtable && matching_code(image, triangles_target, triangles_prefix, sizeof(triangles_prefix)) &&
        matching_code(image, 0x0105ba92, untextured_call, sizeof(untextured_call)) &&
        matching_code(image, 0x0105bbef, textured_call, sizeof(textured_call))) {
        original_fire_triangles = reinterpret_cast<FireDrawTriangles>(image.base + triangles_target);
        redirect_code(image.base + 0x0105ba92, untextured_call, 0xe8, draw_shop_triangles, replacement);
        redirect_code(image.base + 0x0105bbef, textured_call, 0xe8, draw_shop_triangles, replacement);
    }
    constexpr DWORD replace_site = 0x01056399, replace_target = 0x013ac120;
    constexpr DWORD init_site = 0x0105684d;
    constexpr BYTE replace_call[] = {0xe8, 0x82, 0x5d, 0x35, 0x00, 0x8b, 0x08};
    constexpr BYTE init_call[] = {0xe8, 0xce, 0x58, 0x35, 0x00, 0x8b, 0x4e, 0x64};
    constexpr BYTE replace_code[] = {0x53, 0x8b, 0xd9, 0x56, 0x8b, 0x73, 0x04, 0x85, 0xf6};
    if (pause_menu_vtable && matching_code(image, replace_site, replace_call, sizeof(replace_call)) &&
        matching_code(image, replace_target, replace_code, sizeof(replace_code))) {
        original_replace_movie = reinterpret_cast<FireLoadMovie>(image.base + replace_target);
        if (redirect_code(image.base + replace_site, replace_call, 0xe8, replace_fire_movie, replacement)) {
            trace("weapon_movie_route", 0, 0, 0, 0, 0, 0, replace_site);
        }
        if (matching_code(image, init_site, init_call, sizeof(init_call)) &&
            redirect_code(image.base + init_site, init_call, 0xe8, replace_fire_movie, replacement)) {
            trace("shop_movie_route", 0, 0, 0, 0, 0, 0, init_site);
        }
    }
}

#ifdef AC2_CONTROLLER_PROMPTS
decltype(&XInputGetState) original_pad_state;
decltype(&XInputSetState) original_pad_vibration;
decltype(&XInputGetCapabilities) original_pad_capabilities;

DWORD WINAPI read_gamepad(DWORD slot, XINPUT_STATE* state) {
    const DWORD result = controller_state(slot, state);
    if (result != ERROR_NOT_FOUND) {
        return result;
    }
    const DWORD native = original_pad_state(slot, state);
    if (native == ERROR_SUCCESS && state) {
        observe_native_controller(slot, *state);
    }
    return native;
}

DWORD WINAPI gamepad_caps(DWORD slot, DWORD flags, XINPUT_CAPABILITIES* capabilities) {
    const DWORD result = controller_capabilities(slot, flags, capabilities);
    return result == ERROR_NOT_FOUND ? original_pad_capabilities(slot, flags, capabilities) : result;
}

DWORD WINAPI vibrate_gamepad(DWORD slot, XINPUT_VIBRATION* vibration) {
    const DWORD result = controller_vibration(slot, vibration);
    return result == ERROR_NOT_FOUND ? original_pad_vibration(slot, vibration) : result;
}

extern "C" __attribute__((used)) bool ac2_bind_gamepad(DWORD slot, DWORD product) {
    if (bind_controller(slot, product)) {
        trace("controller_registered", 0, 0, slot, 0, 0, 0, LOWORD(product), HIWORD(product));
        return true;
    }
    return false;
}

// The game's device callback has already read DIPROP_VIDPID. Reuse its own
// PadXenon constructor for a supported physical device bound by the bridge.
extern "C" __attribute__((naked)) void ac2_route_controller() {
    __asm__ volatile(
        "movl -0x1c(%ebp), %eax\n\t"
        "pushal\n\t"
        "movl %esp, %esi\n\t"
        "andl $-16, %esp\n\t"
        "subl $8, %esp\n\t"
        "pushl %eax\n\t"
        "pushl 8(%ebx)\n\t"
        "call _ac2_bind_gamepad\n\t"
        "testb %al, %al\n\t"
        "movl %esi, %esp\n\t"
        "popal\n\t"
        "jz 1f\n\t"
        "jmp *_ac2_xinput_pad\n\t"
        "1: movzwl %ax, %ecx\n\t"
        "jmp *_ac2_original_pad\n\t");
}

void apply_controller_routes(const GameImage& image) {
    constexpr DWORD callback = 0x0108d487, register_pad = 0x0108d586, xinput = 0x018a5464;
    constexpr BYTE callback_code[] = {0x8b, 0x45, 0xe4, 0x0f, 0xb7, 0xc8, 0x8b, 0xf8, 0xc1, 0xef, 0x10};
    constexpr BYTE register_code[] = {0x52, 0x6a, 0x10, 0x68, 0xb0, 0x05, 0x00, 0x00};
    const HMODULE module = GetModuleHandleW(L"XINPUT1_3.dll");
    if (!module || image.image_size < xinput + 3 * sizeof(std::uintptr_t) ||
        !matching_code(image, callback, callback_code, sizeof(callback_code)) ||
        !matching_code(image, register_pad + 6, register_code, sizeof(register_code))) {
        trace("controller_routes_skipped", 0, 0);
        return;
    }
    original_pad_state = reinterpret_cast<decltype(original_pad_state)>(GetProcAddress(module, "XInputGetState"));
    original_pad_vibration = reinterpret_cast<decltype(original_pad_vibration)>(GetProcAddress(module, "XInputSetState"));
    original_pad_capabilities = reinterpret_cast<decltype(original_pad_capabilities)>(
        GetProcAddress(module, "XInputGetCapabilities"));
    const std::uintptr_t original[] = {reinterpret_cast<std::uintptr_t>(original_pad_capabilities),
        reinterpret_cast<std::uintptr_t>(original_pad_state), reinterpret_cast<std::uintptr_t>(original_pad_vibration)};
    const std::uintptr_t replacement[] = {reinterpret_cast<std::uintptr_t>(gamepad_caps),
        reinterpret_cast<std::uintptr_t>(read_gamepad), reinterpret_cast<std::uintptr_t>(vibrate_gamepad)};
    auto* table = reinterpret_cast<std::uintptr_t*>(image.base + xinput);
    if (!original[0] || !original[1] || !original[2] ||
        std::memcmp(table, original, sizeof(original)) != 0 || !initialize_controller_input()) {
        trace("controller_routes_skipped", 0, 0, 0, 0, 0, 0, xinput);
        return;
    }
    ac2_original_pad = image.base + callback + 6;
    ac2_xinput_pad = image.base + register_pad;
    DWORD protection;
    if (!VirtualProtect(table, sizeof(original), PAGE_READWRITE, &protection)) {
        trace("controller_routes_skipped", 0, 0, 0, 0, 0, 0, xinput, GetLastError());
        return;
    }
    BYTE edited[5];
    if (redirect_code(image.base + callback, callback_code, 0xe9, ac2_route_controller, edited)) {
        std::memcpy(table, replacement, sizeof(replacement));
        trace("controller_routes_applied", 0, 0, 0, 0, 0, 0, callback, xinput);
    } else {
        trace("controller_routes_skipped", 0, 0, 0, 0, 0, 0, callback, GetLastError());
    }
    DWORD ignored;
    VirtualProtect(table, sizeof(original), protection, &ignored);
}

PromptButton texture_prompt(IDirect3DBaseTexture9* texture) {
    if (!texture || texture->GetType() != D3DRTYPE_TEXTURE) {
        return PromptButton::None;
    }
    constexpr GUID key{0x820cc67c, 0x384b, 0x4af4, {0xb7, 0xb0, 0x27, 0x9b, 0x1e, 0xab, 0x90, 0x31}};
    PromptButton button = PromptButton::None;
    DWORD bytes = sizeof(button);
    if (SUCCEEDED(texture->GetPrivateData(key, &button, &bytes))) {
        return button;
    }
    auto* color = static_cast<IDirect3DTexture9*>(texture);
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(color->GetLevelDesc(0, &desc)) && !desc.Usage && desc.Format == D3DFMT_A8R8G8B8 &&
        desc.Width == 32 && desc.Height == 32) {
        button = prompt_button(texture_fingerprint(color, 32, 128));
    }
    texture->SetPrivateData(key, &button, sizeof(button), 0);
    return button;
}
#endif

void apply_world_markers(const GameImage& image) {
    constexpr DWORD position = 0x010575f0;
    constexpr BYTE position_prefix[] = {0x55, 0x8b, 0xec, 0xdd, 0x45, 0x14, 0x56, 0x8b, 0x75, 0x08};
    constexpr DWORD calls[] = {0x0100adbb, 0x0100c2d6, 0x0100c920, 0x0100cbf5, 0x0100cf70};
    BYTE originals[std::size(calls)][5];
    if (!matching_code(image, position, position_prefix, sizeof(position_prefix))) {
        return;
    }
    for (size_t index = 0; index < std::size(calls); ++index) {
        BYTE* code = originals[index];
        const DWORD displacement = position - (calls[index] + 5);
        code[0] = 0xe8;
        std::memcpy(code + 1, &displacement, sizeof(displacement));
        if (!matching_code(image, calls[index], code, sizeof(originals[index]))) {
            trace("world_markers_skipped", 0, 0, 0, 0, 0, 0, calls[index]);
            return;
        }
    }
    ac2_set_marker_position = image.base + position;
    BYTE replacements[std::size(calls)][5];
    for (size_t index = 0; index < std::size(calls); ++index) {
        if (!redirect_code(image.base + calls[index], originals[index], 0xe8, ac2_prepare_world_marker,
                           replacements[index])) {
            const DWORD error = GetLastError();
            trace("world_markers_skipped", 0, 0, 0, 0, 0, 0, calls[index], error);
            while (index > 0) {
                --index;
                replace_code(image.base + calls[index], replacements[index], originals[index], sizeof(originals[index]));
            }
            return;
        }
    }
    trace("world_markers_applied", 0, 0, 0, 0, 0, 0, std::size(calls));
}

void apply_hud_layout(const GameImage& image) {
    constexpr DWORD layout_site = 0x0105b685;
    constexpr DWORD layout_resume = 0x0105b748;
    constexpr BYTE layout_code[] = {
        0x0f, 0x5a, 0xc8, 0x0f, 0x5a, 0xda, 0x66, 0x0f, 0x2f, 0xcb, 0x0f, 0x5a, 0xc0, 0x76, 0x56,
    };
    constexpr BYTE resume_code[] = {
        0x8b, 0x73, 0x08, 0xf3, 0x0f, 0x11, 0x45, 0xec, 0xf3, 0x0f, 0x11, 0x45, 0xe8,
        0xf3, 0x0f, 0x11, 0x4d, 0xe4, 0xf3, 0x0f, 0x11, 0x4d, 0xe0,
    };
    if (!matching_code(image, layout_site, layout_code, sizeof(layout_code)) ||
        !matching_code(image, layout_resume + 8, resume_code, sizeof(resume_code))) {
        trace("hud_layout_skipped", 0, 0);
        return;
    }
    ac2_hud_layout_resume = image.base + layout_resume;
    BYTE replacement[5];
    if (!redirect_code(image.base + layout_site, layout_code, 0xe9, ac2_prepare_hud_layout, replacement)) {
        trace("hud_layout_skipped", 0, 0, 0, 0, 0, 0, layout_site, GetLastError());
        return;
    }
    trace("hud_layout_applied", 0, 0, 0, 0, 0, 0, layout_site);
    apply_world_markers(image);
    apply_hud_anchors(image);
}

#ifdef AC2_HIGH_REFRESH
void apply_frame_wait(const GameImage& image) {
    constexpr DWORD limit = 0x011d4e8b, wait_call = 0x011d4e98, wait = 0x01061f00;
    constexpr BYTE limit_code[] = {0x83, 0xf8, 0x10, 0x73, 0x10, 0xb9, 0x10, 0, 0, 0,
                                   0x2b, 0xc8, 0x51, 0xe8, 0x63, 0xd0, 0xe8, 0xff, 0x83, 0xc4, 0x04};
    constexpr BYTE wait_prefix[] = {0x8b, 0x44, 0x24, 0x04, 0x50, 0xff, 0x15};
    if (!matching_code(image, limit, limit_code, sizeof(limit_code)) ||
        !matching_code(image, wait, wait_prefix, sizeof(wait_prefix))) {
        trace("frame_wait_skipped", 0, 0);
        return;
    }
    ac2_game_frame_wait = image.base + wait;
    BYTE replacement[5];
    if (!redirect_code(image.base + wait_call, limit_code + wait_call - limit, 0xe8, ac2_frame_wait, replacement)) {
        trace("frame_wait_skipped", 0, 0, 0, 0, 0, 0, wait_call, GetLastError());
        return;
    }
    trace("frame_wait_applied", 0, 0, 0, 0, 0, 0, wait_call);
}
#endif

void apply_native_view() {
    static const bool applied = [] {
        constexpr DWORD viewport_site = 0x011875ae;
        constexpr BYTE viewport_code[] = {
            0x83, 0xe8, 0x01, 0x74, 0x0a, 0x83, 0xe8, 0x01, 0x74,
            0x1f, 0xd9, 0x45, 0xfc, 0xeb, 0x41, 0xd9, 0x45, 0xf8,
        };
        constexpr DWORD camera_site = 0x01188165;
        constexpr BYTE camera_code[] = {0xe8, 0x96, 0x23, 0x00, 0x00, 0xeb, 0x6f};
        constexpr DWORD camera_target = 0x0118a500;
        constexpr BYTE perspective_prefix[] = {0x53, 0x8b, 0xdc, 0x83, 0xec, 0x08, 0x83, 0xe4, 0xf0};
        const GameImage image = game_image();
        if (!matching_code(image, viewport_site, viewport_code, sizeof(viewport_code)) ||
            !matching_code(image, camera_site, camera_code, sizeof(camera_code)) ||
            !matching_code(image, camera_target, perspective_prefix, sizeof(perspective_prefix))) {
            trace("native_view_skipped", 0, 0);
            return false;
        }

        ac2_other_view = image.base + viewport_site + 5;
        ac2_full_view = image.base + 0x011875b8;
        ac2_fitted_view = image.base + 0x011875bd;
        ac2_perspective = image.base + camera_target;
        BYTE camera_replacement[5], viewport_replacement[5];
        if (!redirect_code(image.base + camera_site, camera_code, 0xe8, ac2_prepare_perspective, camera_replacement)) {
            trace("native_view_skipped", 0, 0, 0, 0, 0, 0, camera_site, GetLastError());
            return false;
        }
        if (!redirect_code(image.base + viewport_site, viewport_code, 0xe9, ac2_choose_viewport, viewport_replacement)) {
            const DWORD error = GetLastError();
            replace_code(image.base + camera_site, camera_replacement, camera_code, sizeof(camera_replacement));
            trace("native_view_skipped", 0, 0, 0, 0, 0, 0, viewport_site, error);
            return false;
        }
        trace("native_view_applied", 0, 0, 0, 0, 0, 0, viewport_site, camera_site);
        apply_hud_layout(image);
#ifdef AC2_HIGH_REFRESH
        apply_frame_wait(image);
#endif
#ifdef AC2_CONTROLLER_PROMPTS
        apply_controller_routes(image);
#endif
        return true;
    }();
    (void)applied;
}

#else

void trace_display_ratio_references() {
    struct Anchor {
        DWORD rva;
        std::uint64_t bits;
        size_t size;
    };
    static constexpr Anchor view_anchors[] = {
        {0x01a42258, 0x3ffc71c720000000, 8},  // 16:9 in the View data block
        {0x01a42260, 0x3fe2000000000000, 8},  // 9:16
        {0x01a42268, 0x3faaaaab, 4},  // 4:3
    };
    static constexpr Anchor stage_anchors[] = {
        {0x01a30948, 0x4094000000000000, 8},  // 1280 logical units, HUD marker table
        {0x01a30950, 0x4086800000000000, 8},  // 720 logical units
    };
    // Match the read-only View and Flash stage data before examining their consumers.
    static const bool scanned = [] {
        const GameImage image = game_image();
        if (image.image_size <= stage_anchors[1].rva + sizeof(float) || image.code_rva >= image.image_size ||
            image.code_size > image.image_size - image.code_rva) {
            return false;
        }
        const auto anchor_matches = [&image](const Anchor& anchor) {
            MEMORY_BASIC_INFORMATION memory{};
            const auto* location = reinterpret_cast<const void*>(image.base + anchor.rva);
            if (!VirtualQuery(location, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
                (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                return false;
            }
            std::uint64_t bits = 0;
            std::memcpy(&bits, location, anchor.size);
            return bits == anchor.bits;
        };
        for (const Anchor& anchor : view_anchors) {
            if (!anchor_matches(anchor)) {
                return false;
            }
        }
        const bool probe_stage = anchor_matches(stage_anchors[0]) && anchor_matches(stage_anchors[1]);

        const std::uintptr_t end = image.base + image.code_rva + image.code_size;
        UINT view_count = 0, stage_count = 0;
        DWORD last_view_block = 0;
        for (auto start = image.base + image.code_rva; start < end;) {
            MEMORY_BASIC_INFORMATION memory{};
            if (!VirtualQuery(reinterpret_cast<const void*>(start), &memory, sizeof(memory))) {
                break;
            }
            const auto region_end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
            const auto limit = region_end < end ? region_end : end;
            const DWORD executable_read = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (memory.State == MEM_COMMIT && (memory.Protect & executable_read) &&
                !(memory.Protect & PAGE_GUARD)) {
                for (auto address = start + 4; address + sizeof(DWORD) <= limit; ++address) {
                    DWORD operand;
                    std::memcpy(&operand, reinterpret_cast<const void*>(address), sizeof(operand));
                    const DWORD rva = operand - image.base;
                    const bool view = rva == view_anchors[0].rva || rva == view_anchors[1].rva ||
                                      rva == view_anchors[2].rva;
                    const bool stage = probe_stage && (rva == stage_anchors[0].rva || rva == stage_anchors[1].rva);
                    if (!view && !stage) {
                        continue;
                    }
                    const auto* bytes = reinterpret_cast<const BYTE*>(address);
                    const bool direct = (bytes[-1] & 0xc7) == 0x05;
                    const bool x87 = direct && bytes[-2] >= 0xd8 && bytes[-2] <= 0xdf;
                    const bool sse = direct && (bytes[-4] == 0xf3 || bytes[-4] == 0xf2) && bytes[-3] == 0x0f;
                    if (!x87 && !sse) {
                        continue;
                    }
                    const DWORD site = address - image.base - (x87 ? 2 : 4);
                    trace(view ? "view_reference" : "stage_reference", 0, 0, 0, 0, 0, 0, site, rva);
                    if (stage) {
                        ++stage_count;
                        continue;
                    }
                    ++view_count;
                    // Keep the small instruction fragments in the temporary trace only.
                    if (view_count <= 16 && (!last_view_block || site - last_view_block >= 192)) {
                        const auto instruction = image.base + site;
                        const auto begin = instruction - start < 192 ? start : (instruction - 192) & ~std::uintptr_t{15};
                        const auto finish = limit - instruction < 240 ? limit : instruction + 240;
                        for (auto position = begin; position + sizeof(DWORD) <= finish; position += sizeof(DWORD)) {
                            DWORD word;
                            std::memcpy(&word, reinterpret_cast<const void*>(position), sizeof(word));
                            trace("view_code_word", 0, 0, 0, 0, 0, 0, position - image.base, word);
                        }
                        last_view_block = site;
                    }
                }
            }
            start = limit;
        }
        trace("display_reference_count", 0, 0, 0, 0, 0, 0, view_count, stage_count);
        return true;
    }();
    (void)scanned;
}

#endif

LONGLONG counter() {
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    return ticks.QuadPart;
}

double microseconds(LONGLONG ticks) {
    static const double per_tick = [] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return 1e6 / frequency.QuadPart;
    }();
    return ticks * per_tick;
}

struct Axis {
    double x, y, z;

    double dot(Axis other) const { return x * other.x + y * other.y + z * other.z; }
    double length_squared() const { return dot(*this); }
};

struct ProjectionAxes {
    double x, y, depth_error;
};

ProjectionAxes projection_axes(const float* m, bool transposed) {
    // For a perspective view-projection matrix, w follows depth and the
    // screen axes form an orthogonal frame. Translation is in the fourth component.
    const auto axis = [m, transposed](int row) -> Axis {
        return transposed ? Axis{m[row], m[row + 4], m[row + 8]}
                          : Axis{m[4 * row], m[4 * row + 1], m[4 * row + 2]};
    };
    const Axis x = axis(0), y = axis(1), z = axis(2), w = axis(3);
    const double xx = x.length_squared(), yy = y.length_squared();
    const double zz = z.length_squared(), ww = w.length_squared();
    if (!(xx > 1e-8 && yy > 1e-8 && zz > 1e-8 && ww > 1e-8)) {
        return {};
    }
    const double xp = x.dot(w), yp = y.dot(w), zp = z.dot(w), xy = x.dot(y);
    if (!(xp * xp < 0.0001 * xx * ww && yp * yp < 0.0001 * yy * ww &&
          xy * xy < 0.0001 * xx * yy && zp * zp > 0.999 * zz * ww)) {
        return {};
    }
    return {std::sqrt(xx / ww), std::sqrt(yy / ww), std::abs(std::sqrt(zz / ww) - 1.0)};
}

bool flat_projection(const float* m) {
    const bool affine = ((m[3] == 0 && m[7] == 0 && m[11] == 0) ||
                         (m[12] == 0 && m[13] == 0 && m[14] == 0)) && m[15] == 1;
    return affine && m[1] == 0 && m[4] == 0 && std::abs(m[0]) > 0.000001f &&
           std::abs(m[0]) < 0.1f && std::abs(m[5]) > 0.000001f && std::abs(m[5]) < 0.1f;
}

void trace_flat_callers(UINT device, UINT frame, DWORD site, const std::uintptr_t* caller) {
    using StackLimits = void (WINAPI*)(PULONG_PTR, PULONG_PTR);
    static const auto get_limits = reinterpret_cast<StackLimits>(GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "GetCurrentThreadStackLimits"));
    if (!get_limits) {
        return;
    }
    ULONG_PTR bottom, top;
    get_limits(&bottom, &top);
    for (UINT level = 0; level < 10; ++level) {
        const auto address = reinterpret_cast<std::uintptr_t>(caller);
        if (address < bottom || address > top - 2 * sizeof(std::uintptr_t) ||
            address % alignof(std::uintptr_t)) {
            break;
        }
        if (const DWORD rva = code_rva(reinterpret_cast<void*>(caller[1]))) {
            trace("flat_caller", device, frame, level, 0, 0, 0, site, rva);
        }
        if (caller[0] <= address) {
            break;
        }
        caller = reinterpret_cast<const std::uintptr_t*>(caller[0]);
    }
}

class Factory final : public IDirect3D9 {
public:
    explicit Factory(IDirect3D9* real) : real_(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) noexcept override {
        if (!object) {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IDirect3D9) {
            *object = static_cast<IDirect3D9*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(iid, object);
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG remaining = --references_;
        if (!remaining) {
            real_->Release();
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND window,
        DWORD behavior, D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device) noexcept override;

#include "factory_forward.inc"

private:
    IDirect3D9* const real_;
    std::atomic<ULONG> references_{1};
};

class Device final : public IDirect3DDevice9 {
public:
    Device(IDirect3DDevice9* real, Factory* factory, const D3DPRESENT_PARAMETERS& parameters)
        : real_(real), factory_(factory), id_(++next_id_) {
        factory_->AddRef();
        trace_parameters("create_device", parameters);
        trace_backbuffer("backbuffer", 0);
#ifdef AC2_HIGH_REFRESH
        update_frame_wait(real_, parameters, id_, 0);
#endif
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) noexcept override {
        if (!object) {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IDirect3DDevice9) {
            *object = static_cast<IDirect3DDevice9*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(iid, object);
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG remaining = --references_;
        if (!remaining) {
            trace("release_device", id_, frame_);
#ifdef AC2_NATIVE_VIEW
            set_native_output(0, 0);
#endif
#ifdef AC2_HIGH_REFRESH
            ac2_remove_frame_wait = 0;
#endif
#ifdef AC2_VIDEO_FIT
            movie_surround_.reset();
#endif
#ifdef AC2_CONTROLLER_PROMPTS
            for (auto& family : prompt_textures_) {
                for (auto* texture : family) {
                    if (texture) {
                        texture->Release();
                    }
                }
            }
#endif
            real_->Release();
            factory_->Release();
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** factory) noexcept override {
        if (!factory) {
            return D3DERR_INVALIDCALL;
        }
        factory_->AddRef();
        *factory = factory_;
        return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* parameters) noexcept override {
#ifdef AC2_NATIVE_VIEW
        set_native_output(0, 0);
        popup_texture_ = false;
        popup_reported_ = false;
        weapon_reported_ = false;
        weapon_projection_reported_ = false;
        shop_reported_ = false;
#endif
#ifdef AC2_HIGH_REFRESH
        ac2_remove_frame_wait = 0;
#endif
#ifdef AC2_LEAN_TRACE
        last_present_ = 0;
        last_sample_ = 0;
        frame_period_count_ = 0;
#endif
#ifdef AC2_VIDEO_FIT
        movie_surround_.reset();
#endif
        const HRESULT result = real_->Reset(parameters);
        if (SUCCEEDED(result) && parameters) {
            trace_parameters("reset", *parameters);
            trace_backbuffer("backbuffer", frame_);
#ifdef AC2_HIGH_REFRESH
            update_frame_wait(real_, *parameters, id_, frame_);
#endif
        } else {
            trace("reset_failed", id_, frame_, 0, 0, 0, 0, result);
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE Present(const RECT* source, const RECT* destination,
        HWND window, const RGNDATA* dirty) noexcept override {
        const UINT frame = frame_.fetch_add(1);
        const bool sample = sampled(frame);
        D3DVIEWPORT9 viewport{};
        RECT scissor{};
        DWORD scissor_enabled = FALSE;
        const bool have_viewport = detailed(frame) && SUCCEEDED(real_->GetViewport(&viewport));
        const bool have_scissor = detailed(frame) &&
            SUCCEEDED(real_->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor_enabled)) &&
            scissor_enabled && SUCCEEDED(real_->GetScissorRect(&scissor));
        const LONGLONG begin = counter();
        const LONGLONG previous = last_present_.exchange(begin);
        const HRESULT result = real_->Present(source, destination, window, dirty);
        const LONGLONG end = counter();
#ifdef AC2_LEAN_TRACE
        if (SUCCEEDED(result) && previous) {
            measure_cadence(frame, begin - previous);
        }
#endif
        const UINT events = event_count_.exchange(0);
        const UINT uploads = shader_uploads_.exchange(0);
        projection_count_ = 0;
        layer_count_ = 0;
        quad_count_ = 0;
        if (sample) {
            trace("present", id_, frame, 0, 0, 0, result,
                  previous ? microseconds(begin - previous) : 0, microseconds(end - begin));
            const LONGLONG previous_sample = last_sample_.exchange(begin);
            if (previous_sample) {
                trace("present_rate", id_, frame, 0, 0, 0, 0, SampleInterval,
                      microseconds(begin - previous_sample));
            }
            if (have_viewport) {
                trace("present_viewport", id_, frame, viewport.X, viewport.Y, viewport.Width,
                      viewport.Height, viewport.MinZ, viewport.MaxZ);
            }
            if (have_scissor) {
                trace_rect("present_scissor", id_, frame, scissor);
            }
            if (source) {
                trace_rect("present_source", id_, frame, *source);
            }
            if (destination) {
                trace_rect("present_destination", id_, frame, *destination);
            }
            if (uploads) {
                trace("shader_uploads", id_, frame, 0, 0, 0, 0, uploads);
            }
            if (events > EventsPerSample) {
                trace("events_omitted", id_, frame, 0, 0, 0, 0, events - EventsPerSample);
            }
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* viewport) noexcept override {
        const HRESULT result = real_->SetViewport(viewport);
        const UINT frame = frame_;
        if (SUCCEEDED(result) && viewport && record(frame)) {
            trace("viewport", id_, frame, viewport->X, viewport->Y, viewport->Width,
                  viewport->Height, viewport->MinZ, viewport->MaxZ);
            if (const DWORD site = code_rva(__builtin_return_address(0))) {
                const bool output = active_target_.load() == backbuffer_identity_.load();
                trace("viewport_site", id_, frame, viewport->X, viewport->Y, viewport->Width,
                      viewport->Height, site, output);
                if ((viewport->X || viewport->Y) && output && last_viewport_site_.exchange(site) != site) {
                    trace_viewport_stack(id_, frame, site);
#ifndef AC2_NATIVE_VIEW
                    trace_display_ratio_references();
#endif
                }
            }
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* rect) noexcept override {
        const HRESULT result = real_->SetScissorRect(rect);
        const UINT frame = frame_;
        if (SUCCEEDED(result) && rect && record(frame)) {
            trace_rect("scissor", id_, frame, *rect);
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) noexcept override {
        const HRESULT result = real_->SetRenderState(state, value);
        const UINT frame = frame_;
        if (SUCCEEDED(result) && state == D3DRS_SCISSORTESTENABLE && record(frame)) {
            trace("scissor_enabled", id_, frame, 0, 0, 0, 0, value);
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD slot, IDirect3DSurface9* surface) noexcept override {
        const HRESULT result = real_->SetRenderTarget(slot, surface);
        const UINT frame = frame_;
        if (SUCCEEDED(result) && slot == 0) {
            active_target_ = surface;
        }
        if (SUCCEEDED(result) && slot == 0 && record(frame)) {
            const char* event = surface == backbuffer_identity_.load() ? "bind_backbuffer" : "target";
            trace_surface(event, frame, surface, nullptr);
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* source, const RECT* source_rect,
        IDirect3DSurface9* dest, const RECT* dest_rect, D3DTEXTUREFILTERTYPE filter) noexcept override {
        const HRESULT result = real_->StretchRect(source, source_rect, dest, dest_rect, filter);
        const UINT frame = frame_;
        if (SUCCEEDED(result) && record(frame)) {
            IDirect3DSurface9* backbuffer = backbuffer_identity_;
            trace_surface(source == backbuffer ? "stretch_from_backbuffer" : "stretch_source",
                          frame, source, source_rect);
            trace_surface(dest == backbuffer ? "stretch_to_backbuffer" : "stretch_destination",
                          frame, dest, dest_rect);
            if (source == backbuffer && source_rect) {
                trace_rect("composition_site", id_, frame, *source_rect,
                           code_rva(__builtin_return_address(0)));
            }
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* rects, DWORD flags,
        D3DCOLOR color, float depth, DWORD stencil) noexcept override {
        const HRESULT result = real_->Clear(count, rects, flags, color, depth, stencil);
        const UINT frame = frame_;
        if (SUCCEEDED(result) && (flags & D3DCLEAR_TARGET) && record(frame)) {
            if (count && rects) {
                const D3DRECT& rect = rects[0];
                trace("clear_rect", id_, frame, rect.x1, rect.y1, rect.x2 - rect.x1,
                      rect.y2 - rect.y1, color, count);
            } else {
                trace("clear", id_, frame, 0, 0, 0, 0, color, flags);
            }
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state,
                                           const D3DMATRIX* matrix) noexcept override {
        const HRESULT result = real_->SetTransform(state, matrix);
        const UINT frame = frame_;
        if (SUCCEEDED(result) && state == D3DTS_PROJECTION && matrix && record(frame)) {
            trace("projection", id_, frame, 0, 0, 0, 0, matrix->_11, matrix->_22);
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float* data,
                                                       UINT count) noexcept override {
        const HRESULT result = real_->SetVertexShaderConstantF(start, data, count);
        const UINT frame = frame_;
        if (SUCCEEDED(result) && data && detailed(frame)) {
            ++shader_uploads_;
            if (count < 4) {
                return result;
            }
            const bool output = rendering_output();
            if (output && layer_count_.load() < LayersPerSample) {
                for (UINT offset = 0; offset <= count - 4 && layer_count_.load() < LayersPerSample; ++offset) {
                    const float* matrix = data + 4 * offset;
                    if (flat_projection(matrix) && record_layer()) {
                        trace("flat_projection", id_, frame, start + offset, start, count, 0, matrix[0], matrix[5]);
                        const bool row_vectors = matrix[3] == 0 && matrix[7] == 0 && matrix[11] == 0;
                        trace("flat_origin", id_, frame, start + offset, 0, 0, 0,
                              matrix[row_vectors ? 12 : 3], matrix[row_vectors ? 13 : 7]);
                        const DWORD site = code_rva(__builtin_return_address(0));
                        trace("flat_site", id_, frame, start + offset, 0, 0, 0, site);
                        if (site && last_flat_site_.exchange(site) != site) {
                            trace_flat_callers(id_, frame, site,
                                static_cast<const std::uintptr_t*>(__builtin_frame_address(0)));
                        }
                    }
                }
            }
            for (UINT offset = 0; offset <= count - 4 &&
                 projection_count_.load() < ProjectionEventsPerSample; ++offset) {
                const float* matrix = data + 4 * offset;
                ProjectionAxes axes = projection_axes(matrix, false);
                const ProjectionAxes transposed = projection_axes(matrix, true);
                if (transposed.x && (!axes.x || transposed.depth_error < axes.depth_error)) {
                    axes = transposed;
                }
                if (axes.x && projection_count_++ < ProjectionEventsPerSample) {
                    trace("shader_projection_candidate", id_, frame, start + offset, start,
                          count, 0, axes.x, axes.y);
                }
            }
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) noexcept override {
#ifdef AC2_CONTROLLER_PROMPTS
        if (stage == 0 && code_rva(__builtin_return_address(0)) == 0x011dfa59) {
            const ControllerFamily family = active_controller_family();
            const PromptButton button = family != ControllerFamily::Unknown ? texture_prompt(texture) : PromptButton::None;
            if (button != PromptButton::None) {
                auto*& replacement = prompt_textures_[static_cast<unsigned>(family) - 1][static_cast<unsigned>(button)];
                if (!replacement) {
                    replacement = create_prompt_texture(real_, family, button);
                }
                if (replacement) {
                    texture = replacement;
                }
            }
        }
#endif
        const HRESULT result = real_->SetTexture(stage, texture);
#ifdef AC2_NATIVE_VIEW
        if (SUCCEEDED(result) && stage == 0) {
            popup_texture_ = code_rva(__builtin_return_address(0)) == 0x011dfa59 && is_popup_texture(texture);
        }
#endif
        const UINT frame = frame_;
        if (SUCCEEDED(result) && stage == 0 && texture && detailed(frame) && rendering_output() &&
            layer_count_.load() < LayersPerSample && texture->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc)) &&
                desc.Width >= 512 && desc.Height >= 256 && !(desc.Usage & D3DUSAGE_RENDERTARGET) && record_layer()) {
                trace("output_texture", id_, frame, 0, 0, desc.Width, desc.Height,
                      code_rva(__builtin_return_address(0)), desc.Format);
            }
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) noexcept override {
        if (count == 2 && detailed(frame_)) {
            trace_quad("quad", type, start, count, __builtin_return_address(0));
        }
#ifdef AC2_NATIVE_VIEW
        const bool weapon_veil = rendering_weapon_wheel && type == D3DPT_TRIANGLELIST && count == 2 &&
                                 code_rva(__builtin_return_address(0)) == 0x012954e4 && solid_flash_texture();
        const bool shop_veil = shop_quad && type == D3DPT_TRIANGLELIST && count == 2 &&
                              code_rva(__builtin_return_address(0)) == 0x012954e4 && rendering_output();
        if (rendering_shop) {
            trace_shop_draw(code_rva(__builtin_return_address(0)), type, count);
        }
#else
        constexpr bool weapon_veil = false;
        constexpr bool shop_veil = false;
#endif
        return draw_with_veil_projection([&] { return real_->DrawPrimitive(type, start, count); }, weapon_veil, shop_veil);
    }

    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base, UINT min,
                                                    UINT vertices, UINT start, UINT count) noexcept override {
        if (count == 2 && detailed(frame_)) {
            trace_quad("quad_indexed", type, start, vertices, __builtin_return_address(0));
        }
#ifdef AC2_NATIVE_VIEW
        if (rendering_shop) {
            trace_shop_draw(code_rva(__builtin_return_address(0)), type, count);
        }
#endif
        return draw_with_veil_projection([&] {
            return real_->DrawIndexedPrimitive(type, base, min, vertices, start, count);
        });
    }

    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT min, UINT vertices, UINT count,
        const void* indices, D3DFORMAT index_format, const void* data, UINT stride) noexcept override {
        if (count == 2 && detailed(frame_)) {
            trace_quad("quad_indexed_up", type, stride, vertices, __builtin_return_address(0));
        }
#ifdef AC2_NATIVE_VIEW
        if (rendering_shop) {
            trace_shop_draw(code_rva(__builtin_return_address(0)), type, count);
        }
#endif
        return draw_with_veil_projection([&] {
            return real_->DrawIndexedPrimitiveUP(type, min, vertices, count, indices, index_format, data, stride);
        });
    }

    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count,
                                              const void* vertices, UINT stride) noexcept override {
        const UINT frame = frame_;
#ifdef AC2_NATIVE_VIEW
        if (rendering_shop) {
            trace_shop_draw(code_rva(__builtin_return_address(0)), type, count);
        }
#endif
        if (count == 2 && detailed(frame)) {
            trace_quad("quad_up", type, stride, count, __builtin_return_address(0));
        }
#ifdef AC2_VIDEO_FIT
        constexpr DWORD movie_site = 0x01204d32;
        if (type == D3DPT_TRIANGLESTRIP && count == 2 && stride == 20 && rendering_output() &&
            code_rva(__builtin_return_address(0)) == movie_site) {
            HRESULT movie_result;
            UINT bands_drawn = 0;
            const UINT width = output_width_.load(), height = output_height_.load();
            if (width && height && draw_fitted_movie(real_, width, height, vertices, movie_surround_, movie_result,
                                                     bands_drawn)) {
                if (sampled(frame)) {
                    trace("movie_fit", id_, frame, 0, 0, width, height, movie_result, bands_drawn);
                }
                return movie_result;
            }
            if (sampled(frame)) {
                trace("movie_fit_skipped", id_, frame, 0, 0, width, height, movie_site);
            }
        }
#endif
        const HRESULT result = draw_with_veil_projection([&] {
            return real_->DrawPrimitiveUP(type, count, vertices, stride);
        });
        if (SUCCEEDED(result) && detailed(frame) && rendering_output() && record_layer()) {
            trace("output_immediate_draw", id_, frame, type, 0, count, stride,
                  code_rva(__builtin_return_address(0)));
        }
        return result;
    }

#include "device_forward.inc"

private:
#ifdef AC2_NATIVE_VIEW
    bool solid_flash_texture() {
        if (!rendering_output()) {
            return false;
        }
        IDirect3DBaseTexture9* texture = nullptr;
        if (FAILED(real_->GetTexture(0, &texture)) || !texture) {
            return false;
        }
        D3DSURFACE_DESC desc{};
        const bool small_texture = texture->GetType() == D3DRTYPE_TEXTURE &&
            SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc)) &&
            desc.Width == 4 && desc.Height == 4 && !(desc.Usage & D3DUSAGE_RENDERTARGET);
        texture->Release();
        return small_texture;
    }

    void trace_shop_draw(DWORD site, D3DPRIMITIVETYPE type, UINT count) {
        const UINT frame = frame_;
        if (!shop_trace_frame_) {
            shop_trace_frame_ = frame + 1;
        }
        if ((frame + 1 != shop_trace_frame_ && frame + 1 != shop_trace_frame_ + 240) ||
            shop_trace_count_++ >= 160) {
            return;
        }
        IDirect3DBaseTexture9* texture = nullptr;
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(real_->GetTexture(0, &texture)) && texture) {
            if (texture->GetType() == D3DRTYPE_TEXTURE) {
                static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc);
            }
            texture->Release();
        }
        trace("shop_draw", id_, frame, type, count, desc.Width, desc.Height, site, desc.Format);
        D3DVIEWPORT9 viewport{};
        if (SUCCEEDED(real_->GetViewport(&viewport))) {
            DWORD scissor = 0;
            real_->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
            trace("shop_viewport", id_, frame, viewport.X, viewport.Y, viewport.Width, viewport.Height,
                  rendering_output(), scissor);
        }
        float matrix[16]{};
        if (SUCCEEDED(real_->GetVertexShaderConstantF(0, matrix, 4))) {
            trace("shop_projection", id_, frame, 0, 0, 0, 0, matrix[0], matrix[5]);
            trace("shop_origin", id_, frame, 0, 0, 0, 0, matrix[12] + matrix[3], matrix[13] + matrix[7]);
        }
        if (shop_quad && shop_quad->right > shop_quad->left) {
            trace("shop_rect", id_, frame, shop_quad->translucent_white, 0, 0, 0, shop_quad->left, shop_quad->top);
            trace("shop_rect_end", id_, frame, 0, 0, 0, 0, shop_quad->right, shop_quad->bottom);
        }
    }
#endif

    void trace_quad(const char* event, D3DPRIMITIVETYPE type, UINT a, UINT b, const void* caller) {
        if (!rendering_output() || quad_count_++ >= 128) {
            return;
        }
        IDirect3DBaseTexture9* texture = nullptr;
        real_->GetTexture(0, &texture);
        D3DSURFACE_DESC desc{};
        if (texture && texture->GetType() == D3DRTYPE_TEXTURE) {
            static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc);
        }
        trace(event, id_, frame_, type, texture != nullptr, desc.Width, desc.Height,
              code_rva(caller), a);
        if (!texture) {
            float matrix[16]{};
            if (SUCCEEDED(real_->GetVertexShaderConstantF(0, matrix, 4))) {
                trace("quad_matrix", id_, frame_, b, 0, 0, 0, matrix[0], matrix[5]);
            }
        } else {
            texture->Release();
        }
    }

#ifdef AC2_LEAN_TRACE
    void measure_cadence(UINT frame, LONGLONG elapsed) {
        frame_periods_[frame_period_count_++] = elapsed;
        if (frame_period_count_ == SampleInterval) {
            std::sort(std::begin(frame_periods_), std::end(frame_periods_));
            constexpr size_t p95 = (SampleInterval * 95 + 99) / 100 - 1;
            constexpr size_t p99 = (SampleInterval * 99 + 99) / 100 - 1;
            trace("frame_cadence", id_, frame, 0, 0, SampleInterval,
                  static_cast<long>(microseconds(frame_periods_[SampleInterval - 1])),
                  microseconds(frame_periods_[p95]), microseconds(frame_periods_[p99]));
            frame_period_count_ = 0;
        }
    }
#endif

    template<class Draw>
    HRESULT draw_with_veil_projection(Draw draw, bool weapon_veil = false, bool shop_veil = false) {
#ifdef AC2_NATIVE_VIEW
        if (weapon_veil || shop_veil || (popup_texture_ && rendering_output())) {
            const float width = std::bit_cast<float>(ac2_output_width_bits.load());
            const float height = std::bit_cast<float>(ac2_output_height_bits.load());
            float original[16], expanded[16];
            if (width > 0 && height > 0 && SUCCEEDED(real_->GetVertexShaderConstantF(0, original, 4))) {
                std::memcpy(expanded, original, sizeof(expanded));
                if (weapon_veil && !weapon_projection_reported_.exchange(true)) {
                    trace("weapon_projection", id_, frame_, 0, 0,
                          static_cast<long>(width), static_cast<long>(height), original[0], original[5]);
                    trace("weapon_projection_origin", id_, frame_, 0, 0, 0, 0,
                          original[12] + original[3], original[13] + original[7]);
                }
                const bool expanded_veil = weapon_veil ? expand_weapon_projection(expanded, width, height) :
                    shop_veil ? expand_shop_quad(expanded, width, height, *shop_quad) :
                                expand_popup_projection(expanded, width, height);
                if (expanded_veil && SUCCEEDED(real_->SetVertexShaderConstantF(0, expanded, 4))) {
                    const HRESULT result = draw();
                    real_->SetVertexShaderConstantF(0, original, 4);
                    auto& reported = weapon_veil ? weapon_reported_ : shop_veil ? shop_reported_ : popup_reported_;
                    if (SUCCEEDED(result) && !reported.exchange(true)) {
                        trace(weapon_veil ? "weapon_veil" : shop_veil ? "shop_veil" : "popup_veil", id_, frame_, 0, 0,
                              static_cast<long>(width), static_cast<long>(height));
                    }
                    return result;
                }
            }
        }
#else
        (void)weapon_veil;
        (void)shop_veil;
#endif
        return draw();
    }

    static bool sampled(UINT frame) { return frame % SampleInterval == 0; }
    static bool detailed(UINT frame) {
#ifdef AC2_LEAN_TRACE
        (void)frame;
        return false;
#else
        return frame < 36000 && sampled(frame);
#endif
    }

    bool record(UINT frame) { return detailed(frame) && event_count_++ < EventsPerSample; }
    bool rendering_output() const {
        const auto* output = backbuffer_identity_.load();
        return output && output == active_target_.load();
    }

    bool record_layer() { return layer_count_++ < LayersPerSample; }

    void trace_parameters(const char* event, const D3DPRESENT_PARAMETERS& p) {
        trace(event, id_, frame_, p.Windowed, p.BackBufferCount, p.BackBufferWidth,
              p.BackBufferHeight, p.FullScreen_RefreshRateInHz, p.PresentationInterval);
    }

    void trace_surface(const char* event, UINT frame, IDirect3DSurface9* surface,
                       const RECT* rect) {
        D3DSURFACE_DESC description{};
        if (surface && SUCCEEDED(surface->GetDesc(&description))) {
            if (rect) {
                trace_rect(event, id_, frame, *rect, description.Width, description.Height);
            } else {
                trace(event, id_, frame, 0, 0, description.Width, description.Height,
                      description.Width, description.Height);
            }
        }
    }

    void trace_backbuffer(const char* event, UINT frame) {
        IDirect3DSurface9* surface = nullptr;
        if (SUCCEEDED(real_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface)) && surface) {
            backbuffer_identity_ = surface;
            active_target_ = surface;
#if defined(AC2_VIDEO_FIT) || defined(AC2_NATIVE_VIEW)
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(surface->GetDesc(&desc))) {
#ifdef AC2_NATIVE_VIEW
                set_native_output(desc.Width, desc.Height);
#endif
#ifdef AC2_VIDEO_FIT
                output_width_ = desc.Width;
                output_height_ = desc.Height;
#endif
            }
#endif
            trace_surface(event, frame, surface, nullptr);
            surface->Release();
        } else {
            backbuffer_identity_ = nullptr;
            active_target_ = nullptr;
#ifdef AC2_NATIVE_VIEW
            set_native_output(0, 0);
#endif
#ifdef AC2_VIDEO_FIT
            output_width_ = output_height_ = 0;
#endif
        }
    }

    inline static std::atomic<UINT> next_id_{0};
    IDirect3DDevice9* const real_;
    Factory* const factory_;
    const UINT id_;
    std::atomic<ULONG> references_{1};
    std::atomic<UINT> frame_{0};
    std::atomic<UINT> event_count_{0};
    std::atomic<UINT> shader_uploads_{0};
    std::atomic<UINT> projection_count_{0};
    std::atomic<UINT> layer_count_{0};
    std::atomic<UINT> quad_count_{0};
    std::atomic<IDirect3DSurface9*> backbuffer_identity_{nullptr};
    std::atomic<IDirect3DSurface9*> active_target_{nullptr};
#ifdef AC2_NATIVE_VIEW
    std::atomic<bool> popup_texture_{false}, popup_reported_{false}, weapon_reported_{false};
    std::atomic<bool> weapon_projection_reported_{false};
    std::atomic<bool> shop_reported_{false};
    UINT shop_trace_frame_{}, shop_trace_count_{};
#endif
#ifdef AC2_CONTROLLER_PROMPTS
    IDirect3DTexture9* prompt_textures_[2][static_cast<unsigned>(PromptButton::Count)]{};
#endif
#ifdef AC2_VIDEO_FIT
    std::atomic<UINT> output_width_{0}, output_height_{0};
    MovieSurround movie_surround_;
#endif
    std::atomic<DWORD> last_viewport_site_{0};
    std::atomic<DWORD> last_flat_site_{0};
    std::atomic<LONGLONG> last_present_{0};
    std::atomic<LONGLONG> last_sample_{0};
#ifdef AC2_LEAN_TRACE
    LONGLONG frame_periods_[SampleInterval]{};
    UINT frame_period_count_{0};
#endif
};

HRESULT STDMETHODCALLTYPE Factory::CreateDevice(UINT adapter, D3DDEVTYPE type, HWND window,
    DWORD behavior, D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device) noexcept {
    const HRESULT result = real_->CreateDevice(adapter, type, window, behavior, parameters, device);
    if (SUCCEEDED(result) && parameters && device && *device) {
        if (auto* wrapper = new (std::nothrow) Device(*device, this, *parameters)) {
            *device = wrapper;
        }
    } else if (parameters) {
        trace("create_failed", 0, 0, 0, 0, parameters->BackBufferWidth,
              parameters->BackBufferHeight, result);
    }
    return result;
}

}  // namespace

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT version) {
    const auto create = system_export<decltype(&Direct3DCreate9)>("Direct3DCreate9");
    IDirect3D9* real = create ? create(version) : nullptr;
    if (real && trace_host()) {
#ifdef AC2_NATIVE_VIEW
        apply_native_view();
#endif
        if (auto* wrapper = new (std::nothrow) Factory(real)) {
            return wrapper;
        }
    }
    return real;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT version, IDirect3D9Ex** d3d) {
    const auto create = system_export<decltype(&Direct3DCreate9Ex)>("Direct3DCreate9Ex");
    return create ? create(version, d3d) : D3DERR_NOTAVAILABLE;
}

#define FORWARD_PERF(result, name, parameters, arguments, fallback) \
    extern "C" result WINAPI name parameters { \
        const auto function = system_export<decltype(&name)>(#name); \
        if (function) { \
            return function arguments; \
        } \
        return fallback; \
    }

FORWARD_PERF(int, D3DPERF_BeginEvent, (D3DCOLOR color, LPCWSTR label), (color, label), -1)
FORWARD_PERF(int, D3DPERF_EndEvent, (), (), -1)
FORWARD_PERF(DWORD, D3DPERF_GetStatus, (), (), 0)
FORWARD_PERF(BOOL, D3DPERF_QueryRepeatFrame, (), (), FALSE)
FORWARD_PERF(void, D3DPERF_SetMarker, (D3DCOLOR color, LPCWSTR label), (color, label), void())
FORWARD_PERF(void, D3DPERF_SetOptions, (DWORD options), (options), void())
FORWARD_PERF(void, D3DPERF_SetRegion, (D3DCOLOR color, LPCWSTR label), (color, label), void())

#undef FORWARD_PERF
