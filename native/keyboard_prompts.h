#pragma once

#include "controller_prompts.h"

#include <array>
#include <atomic>
#include <string>

using KeyBindings = std::array<int, static_cast<unsigned>(PromptButton::Count)>;

bool read_keyboard_profile(const wchar_t* ini_path, KeyBindings& bindings);
enum class LiveKeyboardProfile { Unavailable, Keyboard, Other };
bool supports_live_keyboard_profiles(std::uintptr_t image);
LiveKeyboardProfile read_live_keyboard_profile(std::uintptr_t image, KeyBindings& bindings);
std::wstring input_label(int binding, HKL layout = nullptr);
UINT keyboard_virtual_key(int binding, HKL layout = nullptr);

class KeyboardPrompts {
public:
    void update(std::uintptr_t image = 0);
    bool keyboard_selected() const { return keyboard_selected_; }
    bool enabled() const { return enabled_; }
    bool focused() const { return focused_; }
    HKL layout() const { return layout_.load(std::memory_order_relaxed); }
    int binding(PromptButton button) const;
    bool pressed(PromptButton button) const { return pressed_[static_cast<unsigned>(button)].load(std::memory_order_relaxed); }
    const wchar_t* notice() const;

private:
    std::wstring game_ini_, patch_ini_;
    std::array<std::atomic<int>, static_cast<unsigned>(PromptButton::Count)> bindings_{};
    std::array<std::atomic<bool>, static_cast<unsigned>(PromptButton::Count)> pressed_{};
    std::array<UINT, static_cast<unsigned>(PromptButton::Count)> keys_{};
    std::uintptr_t image_{};
    std::atomic<HKL> layout_{};
    ULONGLONG next_read_{}, next_shortcut_{}, notice_until_{};
    WIN32_FILE_ATTRIBUTE_DATA last_read_{};
    HWND foreground_window_{};
    std::atomic<bool> keyboard_selected_{}, enabled_{true};
    bool initialized_{}, have_file_{}, have_live_{}, shortcut_down_{}, focused_{};
};
