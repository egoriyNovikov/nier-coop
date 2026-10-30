#pragma once

#include <atomic>
#include <memory>

#include <Windows.h>
#include <Xinput.h>

#include <utility/FunctionHook.hpp>

#include "../Mod.hpp"

// The game doesn't hear its player while its window is not in front (owner's report 2026-10-30: with two copies on
// one PC, keys pressed in one window moved the character in the other). The mod hides WM_ACTIVATE/WA_INACTIVE from
// the game so that it doesn't pause on Alt+Tab, so the game keeps reading input in the background. This blocks every
// way the game reads its player then: DirectInput keyboard and mouse, GetAsyncKeyState/GetKeyState, raw input and
// real XInput pads. The test bot's virtual pad (pad 0 of a test copy, TestBot) still works in the background.
class InputFocus : public Mod {
public:
    std::string_view get_name() const override { return "InputFocus"; }

    std::optional<std::string> on_initialize() override;
    bool on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) override;

    // A window of this process is in front (always true with the bot option input_focus 0).
    static bool has_focus();

    static inline std::atomic<bool> enabled{true};

private:
    static SHORT WINAPI get_async_key_state(int key);
    static SHORT WINAPI get_key_state(int key);
    static HRESULT WINAPI get_device_state(void* device, DWORD size, void* data);
    static HRESULT WINAPI get_device_data(void* device, DWORD object_size, void* data, DWORD* count, DWORD flags);
    static DWORD WINAPI xinput_get_state(DWORD user_index, XINPUT_STATE* state);

    void hook_direct_input();

    std::unique_ptr<FunctionHook> m_async_key_hook{};
    std::unique_ptr<FunctionHook> m_key_hook{};
    std::unique_ptr<FunctionHook> m_device_state_hook{};
    std::unique_ptr<FunctionHook> m_device_data_hook{};
    std::unique_ptr<FunctionHook> m_xinput_hook{};
};
