#define AC2_NATIVE_VIEW
#include "d3d9_probe.cpp"

#include <algorithm>

namespace {

float observed_fov;

const char* test_clip_names[] = {"/HUD_Controls", "/HUD_Money", "/HUD_Equipment",
                                  "/HUD_Subtitles", "/HUD_Party", "/mcLogo"};
double test_visibility[] = {1, 1, 1, 1, 0, 0};
HudOrigin test_origin{13, 29};
HudOrigin expected_origins[3];
unsigned render_count;

void* __attribute__((thiscall)) find_test_thread(void*, void*, const char* name, int) {
    for (size_t index = 0; index < std::size(test_clip_names); ++index) {
        if (!std::strcmp(name, test_clip_names[index])) {
            return &test_visibility[index];
        }
    }
    return nullptr;
}

double __attribute__((thiscall)) read_test_property(void*, void* target, int property) {
    return property == 7 ? *static_cast<double*>(target) : -1;
}

int __attribute__((thiscall)) set_test_property(void*, void* target, int property, const double* visible) {
    if (property != 7) {
        return 0;
    }
    *static_cast<double*>(target) = *visible;
    return 1;
}

int* __attribute__((thiscall)) set_test_origin(void*, int* status, SHORT x, SHORT y) {
    test_origin = {x, y};
    *status = 0;
    return status;
}

void __attribute__((thiscall)) test_movie_lock(void*) {}

int* __attribute__((thiscall)) render_test_hud(void*, int* status) {
    unsigned mask = 0;
    for (size_t index = 0; index < std::size(test_visibility); ++index) {
        mask |= static_cast<unsigned>(test_visibility[index]) << index;
    }
    constexpr unsigned expected_masks[] = {8, 1, 6};
    const unsigned pass = render_count++;
    *status = pass < std::size(expected_masks) && mask == expected_masks[pass] &&
              test_origin.x == expected_origins[pass].x && test_origin.y == expected_origins[pass].y ? 0 : -1;
    return status;
}

bool test_master_anchors() {
    BYTE player[0x119]{}, movie[8]{}, flash[8]{};
    BYTE* owner = player;
    BYTE* native_movie = movie;
    BYTE* native_flash = flash;
    const HudOrigin original{13, 29};
    std::memcpy(player + 0x2c, &owner, sizeof(owner));
    std::memcpy(player + 0x4c, &native_movie, sizeof(native_movie));
    std::memcpy(player + 0xcc, &original, sizeof(original));
    std::memcpy(movie + 4, &native_flash, sizeof(native_flash));
    std::memcpy(flash + 4, &owner, sizeof(owner));
    player[0x118] = 1;
    find_flash_thread = find_test_thread;
    get_flash_property = read_test_property;
    set_flash_property = set_test_property;
    set_fire_origin = set_test_origin;
    lock_fire_player = unlock_fire_player = test_movie_lock;
    original_fire_render = render_test_hud;
    constexpr HudAnchor anchors[] = {{0, 0}, HudTopRight, HudBottomLeft};
    constexpr float outputs[][2] = {{1440, 1080}, {1920, 1200}, {3440, 1440}, {1080, 1920}, {1950, 100}};
    for (const auto& output : outputs) {
        const HudLayout layout = fit_hud(output[0], output[1]);
        for (size_t pass = 0; pass < std::size(anchors); ++pass) {
            expected_origins[pass] = anchored_hud_origin(original, layout, anchors[pass]);
        }
        expected_origins[1].x += 92;
        int status = -1;
        render_count = 0;
        if (render_master_hud(nullptr, &status, player, layout, output[0], output[1]) != &status ||
            status != 0 || render_count != 3 || test_origin.x != original.x || test_origin.y != original.y ||
            !test_visibility[0] || !test_visibility[1] || !test_visibility[2] || !test_visibility[3] ||
            test_visibility[4] || test_visibility[5]) {
            return false;
        }
    }
    return true;
}

void observe_perspective(void*, float, float, float fov, float) { observed_fov = fov; }
int full_view() { return 0; }
int fitted_view() { return 2; }
int other_view() { return 4; }

__attribute__((naked)) void exercise_projection(PerspectiveArguments*, const BYTE*) {
    __asm__ volatile(
        "pushl %esi\n\t"
        "movl 8(%esp), %eax\n\t"
        "movl 12(%esp), %esi\n\t"
        "pushl 16(%eax)\n\t"
        "pushl 12(%eax)\n\t"
        "pushl 8(%eax)\n\t"
        "pushl 4(%eax)\n\t"
        "pushl (%eax)\n\t"
        "call _ac2_prepare_perspective\n\t"
        "addl $20, %esp\n\t"
        "popl %esi\n\t"
        "ret\n\t");
}

__attribute__((naked)) int exercise_choice(const BYTE*, int, const float*) {
    __asm__ volatile(
        "movl 4(%esp), %ecx\n\t"
        "movl 8(%esp), %eax\n\t"
        "movl 12(%esp), %edx\n\t"
        "decl %eax\n\t"
        "jmp _ac2_choose_viewport\n\t");
}

__attribute__((naked)) void observe_hud_layout() {
    __asm__ volatile(
        "movl 16(%ebp), %eax\n\t"
        "movss %xmm1, (%eax)\n\t"
        "movl -0x30(%ebp), %ecx\n\t"
        "movl -0x2c(%ebp), %edx\n\t"
        "movl %ecx, 4(%eax)\n\t"
        "movl %edx, 8(%eax)\n\t"
        "leave\n\t"
        "ret\n\t");
}

__attribute__((naked)) void exercise_hud_layout(float, float, float*) {
    __asm__ volatile(
        "pushl %ebp\n\t"
        "movl %esp, %ebp\n\t"
        "subl $0x40, %esp\n\t"
        "movl $0, -0x30(%ebp)\n\t"
        "movl $0, -0x2c(%ebp)\n\t"
        "movss 8(%ebp), %xmm0\n\t"
        "movss 12(%ebp), %xmm2\n\t"
        "jmp _ac2_prepare_hud_layout\n\t");
}

void* __stdcall observe_world_marker(void* result, double x, double y) {
    auto* position = static_cast<double*>(result);
    position[0] = x;
    position[1] = y;
    return result;
}

__attribute__((naked)) void* __stdcall exercise_world_marker(void*, double, double) {
    __asm__ volatile("jmp _ac2_prepare_world_marker");
}

#ifdef AC2_HIGH_REFRESH
DWORD wait_ms;
void observe_wait(DWORD requested) { wait_ms = requested; }
#endif

}  // namespace

int main() {
    if (!test_master_anchors()) {
        return 12;
    }
    std::printf("pid=%lu\n", GetCurrentProcessId());
    BYTE view[0x180]{};
    float target[] = {0, 0, 0, 1920, 1200};
    const float* target_address = target;
    std::memcpy(view + 0x178, &target_address, sizeof(target_address));
    constexpr float full_gate[] = {0, 0, 1, 1};
    std::memcpy(view + 0x64, full_gate, sizeof(full_gate));
    view[0x58] = 2;
    ac2_perspective = reinterpret_cast<std::uintptr_t>(observe_perspective);
    ac2_full_view = reinterpret_cast<std::uintptr_t>(full_view);
    ac2_fitted_view = reinterpret_cast<std::uintptr_t>(fitted_view);
    ac2_other_view = reinterpret_cast<std::uintptr_t>(other_view);
    ac2_hud_layout_resume = reinterpret_cast<std::uintptr_t>(observe_hud_layout);
    ac2_set_marker_position = reinterpret_cast<std::uintptr_t>(observe_world_marker);
    constexpr UINT outputs[][2] = {{640, 480}, {1920, 1200}, {720, 480}, {640, 512}, {512, 640},
        {960, 480}, {1008, 432}, {950, 400}, {740, 400}, {940, 400}, {956, 400}, {960, 400},
        {1104, 400}, {450, 800}, {800, 400}, {1950, 100}, {1000, 450}};
    for (const auto& output : outputs) {
        target[3] = static_cast<float>(output[0]);
        target[4] = static_cast<float>(output[1]);
        set_native_output(output[0], output[1]);
        const float aspect = target[3] / target[4];
        PerspectiveArguments arguments{nullptr, .1f, 1000.f, .8f, aspect};
        exercise_projection(&arguments, view);
        const float original = std::tan(arguments.field_of_view * .5f);
        const float adjusted = std::tan(observed_fov * .5f);
        if (std::abs(adjusted * aspect - original * std::max(16.f/9, aspect)) > .0001f ||
            adjusted + .0001f < original || exercise_choice(view, 2, target) != 0) {
            return 1;
        }
        float layout[3]{};
        exercise_hud_layout(target[3], target[4], layout);
        const float stage_width = layout[0] * 1280.f;
        const float stage_height = layout[0] * 720.f;
        if (layout[0] <= 0 || layout[1] < 0 || layout[2] < 0 ||
            std::abs(2 * layout[1] + stage_width - target[3]) > .001f ||
            std::abs(2 * layout[2] + stage_height - target[4]) > .001f ||
            std::min(layout[1], layout[2]) > .001f) {
            return 5;
        }
        for (const HudAnchor anchor : {HudTopLeft, HudTopRight, HudBottomLeft, HudBottomRight}) {
            const HudOrigin origin = anchored_hud_origin({13, 29}, {layout[0], layout[1], layout[2]}, anchor);
            if (std::abs((origin.x - 13) * layout[0] - anchor.x * layout[1]) > layout[0] * .5f + .001f ||
                std::abs((origin.y - 29) * layout[0] - anchor.y * layout[2]) > layout[0] * .5f + .001f) {
                return 11;
            }
        }
        for (const LONG authored_right : {21840L, 24800L}) {
            const auto origin = action_hud_origin({}, {layout[0], layout[1], layout[2]},
                                                  {15000, authored_right, 400, 1600});
            const float right = layout[1] + layout[0] * (origin.x + authored_right / 20.f);
            const float inset = target[3] - right;
            if (std::abs(inset - layout[0] * 96.f) > layout[0] * .5f + .001f ||
                layout[1] + layout[0] * (origin.x + 750) < 0 ||
                std::abs(layout[2] + layout[0] * origin.y) > layout[0] * .5f + .001f) {
                return 16;
            }
        }
        constexpr float clip_mesh[6][5] = {
            {1024, 510, 1, 0, 0}, {1177, 510, 1, 1, 0}, {1024, 664, 1, 0, 1},
            {1024, 510, 1, 0, 0}, {1177, 664, 1, 1, 1}, {1177, 664, 1, 1, 1},
        };
        float shifted_mesh[6][5];
        std::memcpy(shifted_mesh, clip_mesh, sizeof(clip_mesh));
        minimap_clip_shift = anchored_hud_origin({}, {layout[0], layout[1], layout[2]}, HudBottomRight);
        ac2_shift_minimap_clip(shifted_mesh[0]);
        for (unsigned vertex = 0; vertex < 6; ++vertex) {
            if (shifted_mesh[vertex][0] != clip_mesh[vertex][0] + minimap_clip_shift.x ||
                shifted_mesh[vertex][1] != clip_mesh[vertex][1] + minimap_clip_shift.y ||
                std::memcmp(&shifted_mesh[vertex][2], &clip_mesh[vertex][2], 3 * sizeof(float)) != 0) {
                return 14;
            }
        }
        minimap_clip_shift = {};
        float centered_veil[16] = {1.f / 2400, 0, 0, 0, 0, -1.f / 2048, 0, 0,
                                   0, 0, 1, 0, 0, 0, 0, 1};
        if (!expand_weapon_projection(centered_veil, target[3], target[4]) ||
            std::abs(centered_veil[0] * stage_width * 2400 - target[3]) > .001f ||
            std::abs(-centered_veil[5] * stage_height * 2048 - target[4]) > .001f) {
            return 13;
        }
        double marker[2]{};
        if (exercise_world_marker(marker, 960, 180) != marker ||
            std::abs(marker[0] * layout[0] + layout[1] - target[3] * .75) > .001 ||
            std::abs(marker[1] * layout[0] + layout[2] - target[4] * .25) > .001) {
            return 7;
        }
        const float span_x = stage_width / target[3], span_y = stage_height / target[4];
        float projection[16] = {2 * span_x / 1280, 0, 0, 0,
                                0, -2 * span_y / 720, 0, 0,
                                0, 0, 1, 0, -span_x, span_y, 0, 1};
        float shop_projection[16];
        std::memcpy(shop_projection, projection, sizeof(projection));
        constexpr ShopQuad veil{0, 0, 1280, 720, true};
        if (expand_shop_quad(shop_projection, target[3], target[4], {12, 20, 1268, 700, true}) ||
            expand_shop_quad(shop_projection, target[3], target[4], {0, 0, 1280, 720, false}) ||
            !expand_shop_quad(shop_projection, target[3], target[4], veil) ||
            std::abs(shop_projection[0] * 1280 + shop_projection[12] - 1) > .0001f ||
            std::abs(shop_projection[5] * 720 + shop_projection[13] + 1) > .0001f) {
            return 15;
        }
        float transposed[16]{};
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                transposed[row * 4 + column] = projection[column * 4 + row];
            }
        }
        if (!expand_popup_projection(projection, target[3], target[4]) ||
            !expand_popup_projection(transposed, target[3], target[4]) ||
            projection[12] != -1 || projection[13] != 1 || transposed[3] != -1 || transposed[7] != 1 ||
            projection[0] != 2.f / 1280 || projection[5] != -2.f / 720 ||
            transposed[0] != projection[0] || transposed[5] != projection[5] ||
            expand_popup_projection(projection, target[3], target[4])) {
            return 10;
        }
    }
    set_native_output(1920, 1200);
    if (exercise_choice(view, 2, target) != 2 || exercise_choice(view, 1, target) != 4) {
        return 2;
    }
    target[3] = 1920;
    target[4] = 1200;
    view[0x50] = 1;
    if (exercise_choice(view, 2, target) != 2) {
        return 3;
    }
    view[0x50] = 0;
    constexpr float fitted_gate[] = {0, .05f, 1, .9f};
    std::memcpy(view + 0x64, fitted_gate, sizeof(fitted_gate));
    PerspectiveArguments auxiliary{nullptr, .1f, 1000.f, .8f, 1.6f};
    exercise_projection(&auxiliary, view);
    if (observed_fov != auxiliary.field_of_view) {
        return 4;
    }
    float reference_layout[3]{};
    exercise_hud_layout(1920, 1080, reference_layout);
    if (reference_layout[0] != 1.5f || reference_layout[1] != 0 || reference_layout[2] != 0) {
        return 6;
    }
#ifdef AC2_HIGH_REFRESH
    ac2_game_frame_wait = reinterpret_cast<std::uintptr_t>(observe_wait);
    const auto wait = reinterpret_cast<void (*)(DWORD)>(ac2_frame_wait);
    ac2_remove_frame_wait = 0;
    wait(9);
    if (wait_ms != 9) {
        return 8;
    }
    ac2_remove_frame_wait = 1;
    wait(5);
    if (wait_ms != 9) {
        return 9;
    }
#endif
    std::puts("Camera, HUD, world markers, and popup geometry verified for all 17 requested formats and 16:9");
    return 0;
}
