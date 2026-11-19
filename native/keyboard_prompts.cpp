#include "keyboard_prompts.h"

#include <cstring>
#include <cwchar>

namespace {
enum Slot : unsigned {
    Feet, WeaponHand, Head, EmptyHand, Fist, Medicine, Blade, Sword, Map, Pause,
    LockOn, Camera, HighProfile, WeaponWheel, FirstPerson, CenterCamera,
    MoveForward, MoveBack, MoveLeft, MoveRight, LookUp, LookDown, LookLeft, LookRight,
    SlotCount, NoSlot = SlotCount,
};
constexpr const wchar_t* ProfileFields[]{
    L"Button1", L"Button2", L"Button3", L"Button4", L"PadDown", L"PadLeft", L"PadUp", L"PadRight",
    L"Select", L"Start", L"ShoulderLeft1", L"ShoulderLeft2", L"ShoulderRight1", L"ShoulderRight2",
    L"StickLeft", L"StickRight", L"LeftStickUp", L"LeftStickDown", L"LeftStickLeft", L"LeftStickRight",
    L"RightStickUp", L"RightStickDown", L"RightStickLeft", L"RightStickRight",
};
constexpr Slot PromptSlots[]{
    NoSlot, Feet, EmptyHand, WeaponHand, Head, LockOn, HighProfile, Camera, WeaponWheel, Pause, Map,
    Feet, EmptyHand, MoveForward, MoveBack, MoveLeft, MoveRight, LookUp, LookDown, LookLeft, LookRight,
    Medicine, Blade, Sword, Fist, CenterCamera, FirstPerson,
};
static_assert(std::size(ProfileFields) == SlotCount);
static_assert(std::size(PromptSlots) == std::tuple_size_v<KeyBindings>);

constexpr std::uintptr_t InputManager = 0x01e3dbc0;
constexpr unsigned ProfileCount = 8;
constexpr unsigned ProfileStride = 0x218;
constexpr unsigned MappingStride = 0x214;

template<class T>
bool read_game(std::uintptr_t address, T& value) {
    SIZE_T size{};
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        &value, sizeof(value), &size) && size == sizeof(value);
}

void map_roles(const std::array<int, SlotCount>& assignments, KeyBindings& bindings) {
    bindings.fill(-1);
    for (unsigned role = 1; role < bindings.size(); ++role)
        bindings[role] = assignments[PromptSlots[role]];
}

UINT directinput_scan(int binding) {
    return static_cast<UINT>(binding & 0x7f) | ((binding & 0x80) ? 0xe000u : 0u);
}
}  // namespace

bool read_keyboard_profile(const wchar_t* ini_path, KeyBindings& bindings) {
    bindings.fill(-1);
    wchar_t section[128]{};
    GetPrivateProfileStringW(L"Input", L"SelectedInput", L"", section, std::size(section), ini_path);
    if (_wcsnicmp(section, L"Keyboard", 8) != 0) {
        return false;
    }

    std::array<int, SlotCount> assignments{};
    for (unsigned index = 0; index < assignments.size(); ++index) {
        const auto value = GetPrivateProfileIntW(section, ProfileFields[index], -1, ini_path);
        assignments[index] = value > 0 && value <= 263 ? static_cast<int>(value) : -1;
    }
    map_roles(assignments, bindings);
    return true;
}

bool supports_live_keyboard_profiles(std::uintptr_t image) {
    // Both the selected-device accessor and the profile iterator must match the audited Steam build.
    constexpr BYTE selected[]{0x8b,0x41,0x04,0x8b,0x48,0x04,0x8b,0x81,0x94,0x05,0x00,0x00,0x50,
        0xe8,0xbe,0xdc,0xff,0xff,0x8b,0x80,0x14,0x02,0x00,0x00,0xc3};
    constexpr BYTE enumerate[]{0x55,0x8b,0xec,0x56,0x57,0x8b,0x7d,0x08,0x33,0xf6,0x33,0xc0,
        0x8d,0x91,0x9c,0x05,0x00,0x00};
    BYTE selected_bytes[sizeof(selected)]{}, enumerate_bytes[sizeof(enumerate)]{};
    return image && read_game(image + 0x0108cde0, selected_bytes) &&
        read_game(image + 0x0108aab0, enumerate_bytes) &&
        !std::memcmp(selected, selected_bytes, sizeof(selected)) &&
        !std::memcmp(enumerate, enumerate_bytes, sizeof(enumerate));
}

LiveKeyboardProfile read_live_keyboard_profile(std::uintptr_t image, KeyBindings& bindings) {
    DWORD manager{}, provider{}, profiles{}, slot{};
    if (!image || !read_game(image + InputManager, manager) || !manager ||
        !read_game(manager + 4, provider) || !provider || !read_game(provider + 4, profiles) || !profiles ||
        !read_game(profiles + 0x594, slot) || slot >= ProfileCount) return LiveKeyboardProfile::Unavailable;
    const DWORD profile = profiles + 0x59c + slot * ProfileStride;
    DWORD device{}, table{};
    wchar_t name[16]{};
    if (!read_game(profile, device) || !device || !read_game(profile + 8, name))
        return LiveKeyboardProfile::Unavailable;
    if (_wcsnicmp(name, L"Keyboard", 8) != 0) return LiveKeyboardProfile::Other;
    if (!read_game(profile + 0x214, table) || !table) return LiveKeyboardProfile::Unavailable;

    struct Mapping { DWORD kind, subtype, code; };
    constexpr auto extent = (SlotCount - 1) * MappingStride + sizeof(Mapping);
    std::array<BYTE, extent> snapshot{};
    DWORD final_slot{}, final_table{}, final_device{};
    if (!read_game(table, snapshot) || !read_game(profiles + 0x594, final_slot) || final_slot != slot ||
        !read_game(profile + 0x214, final_table) || final_table != table ||
        !read_game(profile, final_device) || final_device != device) return LiveKeyboardProfile::Unavailable;
    std::array<int, SlotCount> assignments{};
    for (unsigned i = 0; i < assignments.size(); ++i) {
        Mapping mapping{};
        std::memcpy(&mapping, snapshot.data() + i * MappingStride, sizeof(mapping));
        assignments[i] = -1;
        if (mapping.kind == 1 && mapping.code > 0 && mapping.code < 256)
            assignments[i] = static_cast<int>(mapping.code);
        if (mapping.kind == 2 && mapping.code < 8)
            assignments[i] = 256 + static_cast<int>(mapping.code);
    }
    map_roles(assignments, bindings);
    return LiveKeyboardProfile::Keyboard;
}

std::wstring input_label(int binding, HKL layout) {
    if (binding >= 256 && binding <= 263) {
        constexpr const wchar_t* mouse[] = {L"LMB", L"RMB", L"MMB", L"M4", L"M5", L"M6", L"M7", L"M8"};
        return mouse[binding - 256];
    }
    if (binding >= 0x3b && binding <= 0x44) return L"F" + std::to_wstring(binding - 0x3a);
    if (binding >= 0x64 && binding <= 0x66) return L"F" + std::to_wstring(binding - 0x57);
    switch (binding) {
        case 0x01: return L"Esc";
        case 0x0e: return L"Bksp";
        case 0x0f: return L"Tab";
        case 0x1c: return L"Enter";
        case 0x1d: return L"LCtrl";
        case 0x2a: return L"LShift";
        case 0x36: return L"RShift";
        case 0x38: return L"LAlt";
        case 0x39: return L"Space";
        case 0x3a: return L"Caps";
        case 0x45: return L"Num";
        case 0x46: return L"ScrLk";
        case 0x37: return L"N×";
        case 0x47: return L"N7";
        case 0x48: return L"N8";
        case 0x49: return L"N9";
        case 0x4a: return L"N−";
        case 0x4b: return L"N4";
        case 0x4c: return L"N5";
        case 0x4d: return L"N6";
        case 0x4e: return L"N+";
        case 0x4f: return L"N1";
        case 0x50: return L"N2";
        case 0x51: return L"N3";
        case 0x52: return L"N0";
        case 0x53: return L"N.";
        case 0x57: return L"F11";
        case 0x58: return L"F12";
        case 0x70: return L"Kana";
        case 0x79: return L"Conv";
        case 0x7b: return L"NoConv";
        case 0x7d: return L"¥";
        case 0x7e: return L"N.";
        case 0x8d: return L"N=";
        case 0x90: return L"Prev";
        case 0x91: return L"@";
        case 0x92: return L":";
        case 0x93: return L"_";
        case 0x94: return L"Kanji";
        case 0x95: return L"Stop";
        case 0x96: return L"AX";
        case 0x97: return L"IME";
        case 0x99: return L"Next";
        case 0x9c: return L"NumEnt";
        case 0x9d: return L"RCtrl";
        case 0xa0: return L"Mute";
        case 0xa1: return L"Calc";
        case 0xa2: return L"Play";
        case 0xa4: return L"Stop";
        case 0xae: return L"Vol−";
        case 0xb0: return L"Vol+";
        case 0xb2: return L"Web";
        case 0xb3: return L"N,";
        case 0xb5: return L"N÷";
        case 0xb7: return L"PrtSc";
        case 0xb8: return L"RAlt";
        case 0xc5: return L"Pause";
        case 0xc7: return L"Home";
        case 0xc8: return L"↑";
        case 0xc9: return L"PgUp";
        case 0xcb: return L"←";
        case 0xcd: return L"→";
        case 0xcf: return L"End";
        case 0xd0: return L"↓";
        case 0xd1: return L"PgDn";
        case 0xd2: return L"Ins";
        case 0xd3: return L"Del";
        case 0xdb: return L"LWin";
        case 0xdc: return L"RWin";
        case 0xdd: return L"Menu";
        case 0xde: return L"Pwr";
        case 0xdf: return L"Sleep";
        case 0xe3: return L"Wake";
        case 0xe5: return L"Search";
        case 0xe6: return L"Fav";
        case 0xe7: return L"Reload";
        case 0xe8: return L"Web×";
        case 0xe9: return L"Web→";
        case 0xea: return L"Web←";
        case 0xeb: return L"PC";
        case 0xec: return L"Mail";
        case 0xed: return L"Media";
        default: break;
    }
    if (binding <= 0 || binding > 255) {
        return {};
    }
    if (!layout) layout = GetKeyboardLayout(0);
    const UINT code = directinput_scan(binding);
    const UINT key = MapVirtualKeyExW(code, MAPVK_VSC_TO_VK_EX, layout);
    BYTE state[256]{};
    wchar_t name[32]{};
    const int typed = key ? ToUnicodeEx(key, code & 0xff, state, name, std::size(name), 4, layout) : 0;
    if ((typed == 1 || typed == -1) && name[0] > L' ' && name[0] != 127) {
        CharUpperBuffW(name, 1);
        return std::wstring(1, name[0]);
    }

    const LONG scan = static_cast<LONG>(binding & 0x7f) << 16;
    const LONG extended = (binding & 0x80) ? 1L << 24 : 0;
    if (GetKeyNameTextW(scan | extended, name, std::size(name)) > 0) return name;
    std::swprintf(name, std::size(name), L"Sc%02X", static_cast<unsigned>(binding));
    return name;
}

UINT keyboard_virtual_key(int binding, HKL layout) {
    if (binding >= 256 && binding <= 260) {
        constexpr UINT mouse[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        return mouse[binding - 256];
    }
    if (binding <= 0 || binding > 255) return 0;
    if (binding == 0xc5) return VK_PAUSE;
    constexpr UINT keypad[]{VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, VK_SUBTRACT, VK_NUMPAD4, VK_NUMPAD5,
        VK_NUMPAD6, VK_ADD, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD0, VK_DECIMAL};
    if (binding >= 0x47 && binding <= 0x53) return keypad[binding - 0x47];
    return MapVirtualKeyExW(directinput_scan(binding), MAPVK_VSC_TO_VK_EX,
        layout ? layout : GetKeyboardLayout(0));
}

void KeyboardPrompts::update(std::uintptr_t image) {
    if (!initialized_) {
        initialized_ = true;
        if (supports_live_keyboard_profiles(image)) image_ = image;
        wchar_t appdata[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"APPDATA", appdata, std::size(appdata));
        if (length && length < std::size(appdata)) {
            std::wstring dir = std::wstring(appdata) + L"\\Ubisoft\\Assassin's Creed 2\\";
            game_ini_ = dir + L"Assassin2.ini";
            patch_ini_ = dir + L"AC2EnhancedPatch.ini";
            enabled_ = GetPrivateProfileIntW(L"Interface", L"AssignedKeys", 1, patch_ini_.c_str()) != 0;
        }
    }

    const ULONGLONG now = GetTickCount64();
    const auto install_bindings = [&](const KeyBindings& current, bool keyboard) {
        for (unsigned index = 0; index < current.size(); ++index) {
            if (bindings_[index].load(std::memory_order_relaxed) == current[index]) continue;
            bindings_[index].store(current[index], std::memory_order_relaxed);
            keys_[index] = keyboard_virtual_key(current[index], layout());
        }
        keyboard_selected_ = keyboard;
    };
    KeyBindings live;
    const auto profile = read_live_keyboard_profile(image_, live);
    if (profile == LiveKeyboardProfile::Keyboard) {
        have_live_ = true;
        install_bindings(live, true);
    } else if (profile == LiveKeyboardProfile::Other) {
        have_live_ = true;
        keyboard_selected_ = false;
    }
    if (!have_live_ && now >= next_read_ && !game_ini_.empty()) {
        next_read_ = now + 500;
        WIN32_FILE_ATTRIBUTE_DATA file{};
        if (!GetFileAttributesExW(game_ini_.c_str(), GetFileExInfoStandard, &file)) {
            keyboard_selected_ = have_file_ = false;
        } else if (!have_file_ || CompareFileTime(&file.ftLastWriteTime, &last_read_.ftLastWriteTime) ||
                   file.nFileSizeLow != last_read_.nFileSizeLow || file.nFileSizeHigh != last_read_.nFileSizeHigh) {
            KeyBindings current;
            const bool selected = read_keyboard_profile(game_ini_.c_str(), current);
            install_bindings(current, selected);
            last_read_ = file;
            have_file_ = true;
        }
    }

    if (now < next_shortcut_) return;
    next_shortcut_ = now + 8;
    const HWND foreground = GetForegroundWindow();
    if (foreground != foreground_window_) {
        foreground_window_ = foreground;
        DWORD foreground_pid = 0;
        GetWindowThreadProcessId(foreground, &foreground_pid);
        focused_ = foreground_pid == GetCurrentProcessId();
    }
    const auto current_layout = GetKeyboardLayout(focused_ ? GetWindowThreadProcessId(foreground, nullptr) : 0);
    if (current_layout != layout()) {
        for (unsigned index = 1; index < keys_.size(); ++index)
            keys_[index] = keyboard_virtual_key(bindings_[index].load(std::memory_order_relaxed), current_layout);
        layout_.store(current_layout, std::memory_order_relaxed);
    }
    const auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    const bool shortcut = focused_ && down(VK_CONTROL) && down(VK_MENU) && down(VK_F11);
    if (shortcut && !shortcut_down_) {
        enabled_ = !enabled_;
        notice_until_ = now + 2500;
        if (!patch_ini_.empty()) {
            WritePrivateProfileStringW(L"Interface", L"AssignedKeys", enabled_ ? L"1" : L"0", patch_ini_.c_str());
        }
    }
    shortcut_down_ = shortcut;
    const bool show_pressed = focused_ && enabled_ && keyboard_selected_;
    for (unsigned index = 1; index < keys_.size(); ++index) {
        pressed_[index].store(show_pressed && keys_[index] && down(keys_[index]), std::memory_order_relaxed);
    }
}

int KeyboardPrompts::binding(PromptButton button) const {
    const auto index = static_cast<unsigned>(button);
    return enabled_ && keyboard_selected_ && index < bindings_.size()
        ? bindings_[index].load(std::memory_order_relaxed) : -1;
}

const wchar_t* KeyboardPrompts::notice() const {
    if (GetTickCount64() >= notice_until_) {
        return nullptr;
    }
    return enabled_ ? L"PC prompts: Assigned keys" : L"PC prompts: Original icons";
}
