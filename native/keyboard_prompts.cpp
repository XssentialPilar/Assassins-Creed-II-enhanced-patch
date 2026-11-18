#include "keyboard_prompts.h"

#include <cwchar>

bool read_keyboard_profile(const wchar_t* ini_path, KeyBindings& bindings) {
    bindings.fill(-1);
    wchar_t section[128]{};
    GetPrivateProfileStringW(L"Input", L"SelectedInput", L"", section, std::size(section), ini_path);
    if (_wcsnicmp(section, L"Keyboard", 8) != 0) {
        return false;
    }

    constexpr const wchar_t* fields[] = {
        nullptr, L"Button1", L"Button4", L"Button2", L"Button3", L"ShoulderLeft1",
        L"ShoulderRight1", L"ShoulderLeft2", L"ShoulderRight2", L"Start", L"Select", nullptr, nullptr,
    };
    static_assert(std::size(fields) == std::tuple_size_v<KeyBindings>);
    for (unsigned index = 1; index < bindings.size(); ++index) {
        if (!fields[index]) {
            continue;
        }
        const auto value = GetPrivateProfileIntW(section, fields[index], -1, ini_path);
        if (value > 0 && value <= 263) {
            bindings[index] = static_cast<int>(value);
        }
    }
    return true;
}

std::wstring input_label(int binding) {
    if (binding >= 256 && binding <= 263) {
        constexpr const wchar_t* mouse[] = {L"LMB", L"RMB", L"MMB", L"M4", L"M5", L"M6", L"M7", L"M8"};
        return mouse[binding - 256];
    }
    switch (binding) {
        case 0x01: return L"ESC";
        case 0x0e: return L"Bksp";
        case 0x0f: return L"TAB";
        case 0x1c: return L"Enter";
        case 0x1d: return L"LCtrl";
        case 0x2a: return L"LShift";
        case 0x36: return L"RShift";
        case 0x38: return L"LAlt";
        case 0x39: return L"Space";
        case 0x3a: return L"Caps";
        case 0x45: return L"Num";
        case 0x46: return L"Scroll";
        case 0x9c: return L"NumEnt";
        case 0x9d: return L"RCtrl";
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
        default: break;
    }
    if (binding <= 0 || binding > 255) {
        return {};
    }
    wchar_t name[32]{};
    const LONG scan = static_cast<LONG>(binding & 0x7f) << 16;
    const LONG extended = (binding & 0x80) ? 1L << 24 : 0;
    GetKeyNameTextW(scan | extended, name, std::size(name));
    return name;
}

UINT keyboard_virtual_key(int binding) {
    if (binding >= 256 && binding <= 260) {
        constexpr UINT mouse[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        return mouse[binding - 256];
    }
    if (binding <= 0 || binding > 255) return 0;
    if (binding == 0xc5) return VK_PAUSE;
    const UINT scan = (binding & 0x7f) | ((binding & 0x80) ? 0xe000 : 0);
    return MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
}

void KeyboardPrompts::update() {
    if (!initialized_) {
        initialized_ = true;
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
    if (now >= next_read_ && !game_ini_.empty()) {
        next_read_ = now + 500;
        WIN32_FILE_ATTRIBUTE_DATA file{};
        if (!GetFileAttributesExW(game_ini_.c_str(), GetFileExInfoStandard, &file)) {
            keyboard_selected_ = have_file_ = false;
        } else if (!have_file_ || CompareFileTime(&file.ftLastWriteTime, &last_read_.ftLastWriteTime) ||
                   file.nFileSizeLow != last_read_.nFileSizeLow || file.nFileSizeHigh != last_read_.nFileSizeHigh) {
            KeyBindings current;
            const bool selected = read_keyboard_profile(game_ini_.c_str(), current);
            for (unsigned index = 0; index < current.size(); ++index) {
                bindings_[index].store(current[index], std::memory_order_relaxed);
                keys_[index] = keyboard_virtual_key(current[index]);
            }
            keyboard_selected_ = selected;
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
