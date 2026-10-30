#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <spdlog/spdlog.h>

#include "../TestMode.hpp"
#include "InputFocus.hpp"

// DllMain.cpp: the system's dinput8.dll, which our dinput8.dll forwards to.
extern HMODULE g_dinput;
bool load_dinput8();

namespace {
InputFocus* g_focus{};

// Not from dxguid.lib: the IDs of IDirectInput8W and of the system keyboard.
constexpr GUID iid_direct_input8w{0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr GUID guid_sys_keyboard{0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

void hook(std::unique_ptr<FunctionHook>& out, void* target, void* destination, const char* name) {
    if (target == nullptr) {
        spdlog::error("[Input] {} not found", name);
        return;
    }

    out = std::make_unique<FunctionHook>(target, destination);

    if (!out->create()) {
        spdlog::error("[Input] Failed to hook {}", name);
    }
}
}

bool InputFocus::has_focus() {
    if (!enabled) {
        return true;
    }

    DWORD process{};
    const auto window = GetForegroundWindow();
    return window != nullptr && GetWindowThreadProcessId(window, &process) != 0 && process == GetCurrentProcessId();
}

std::optional<std::string> InputFocus::on_initialize() {
    g_focus = this;

    const auto user32 = GetModuleHandleW(L"user32.dll");
    hook(m_async_key_hook, GetProcAddress(user32, "GetAsyncKeyState"), &InputFocus::get_async_key_state, "GetAsyncKeyState");
    hook(m_key_hook, GetProcAddress(user32, "GetKeyState"), &InputFocus::get_key_state, "GetKeyState");
    hook_direct_input();

    // In a test copy the bot hooks XInputGetState itself (its pad 0) and asks has_focus() for the real pads.
    if (!testmode::enabled()) {
        const auto xinput = GetModuleHandleW(L"xinput1_4.dll");
        hook(m_xinput_hook, xinput != nullptr ? GetProcAddress(xinput, "XInputGetState") : nullptr, &InputFocus::xinput_get_state,
            "XInputGetState");
    }

    spdlog::info("[Input] Input of the player is ignored while the game's window is not in front");
    return std::nullopt;
}

// The game's keyboard and mouse devices are made before the mods start. Every DirectInput device has the same
// methods, so a keyboard device of our own gives their addresses.
void InputFocus::hook_direct_input() {
    if (!load_dinput8()) {
        return;
    }

    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const auto create = (CreateFn)GetProcAddress(g_dinput, "DirectInput8Create");
    IDirectInput8W* input{};

    if (create == nullptr || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, iid_direct_input8w, (LPVOID*)&input, nullptr))) {
        spdlog::error("[Input] DirectInput8Create failed, keys in the background still reach the game");
        return;
    }

    IDirectInputDevice8W* keyboard{};

    if (SUCCEEDED(input->CreateDevice(guid_sys_keyboard, &keyboard, nullptr))) {
        const auto methods = *(void***)keyboard;
        // IUnknown (3 methods), then GetCapabilities, EnumObjects, GetProperty, SetProperty, Acquire, Unacquire,
        // GetDeviceState (9), GetDeviceData (10).
        hook(m_device_state_hook, methods[9], &InputFocus::get_device_state, "IDirectInputDevice8::GetDeviceState");
        hook(m_device_data_hook, methods[10], &InputFocus::get_device_data, "IDirectInputDevice8::GetDeviceData");
        keyboard->Release();
    } else {
        spdlog::error("[Input] No DirectInput keyboard, keys in the background still reach the game");
    }

    input->Release();
}

bool InputFocus::on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {
    return message != WM_INPUT || has_focus();
}

void InputFocus::filter_pad(XINPUT_STATE* state) {
    if (!has_focus()) {
        // Still connected (no "controller disconnected" from the game), but nothing pressed.
        state->Gamepad = {};
    } else if (block_cutscene_pause) {
        state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_START;
    }
}

SHORT WINAPI InputFocus::get_async_key_state(int key) {
    const auto result = g_focus->m_async_key_hook->get_original<decltype(get_async_key_state)>()(key);
    return has_focus() && !(key == VK_ESCAPE && block_cutscene_pause) ? result : 0;
}

SHORT WINAPI InputFocus::get_key_state(int key) {
    const auto result = g_focus->m_key_hook->get_original<decltype(get_key_state)>()(key);
    // Toggles (Caps Lock, bit 0) are not input of the player, keep them.
    return has_focus() && !(key == VK_ESCAPE && block_cutscene_pause) ? result : (SHORT)(result & 1);
}

HRESULT WINAPI InputFocus::get_device_state(void* device, DWORD size, void* data) {
    const auto result = g_focus->m_device_state_hook->get_original<decltype(get_device_state)>()(device, size, data);

    if (SUCCEEDED(result) && data != nullptr && !has_focus()) {
        // Nothing pressed, no mouse movement (the game reads pads through XInput, not as DirectInput joysticks).
        std::memset(data, 0, size);
    } else if (SUCCEEDED(result) && data != nullptr && size == 256 && block_cutscene_pause) {
        ((uint8_t*)data)[DIK_ESCAPE] = 0; // the keyboard's state: a byte per key
    }

    return result;
}

HRESULT WINAPI InputFocus::get_device_data(void* device, DWORD object_size, void* data, DWORD* count, DWORD flags) {
    // Read anyway, so that the presses of the background don't come all at once when the window is back in front.
    const auto result = g_focus->m_device_data_hook->get_original<decltype(get_device_data)>()(device, object_size, data, count, flags);

    if (SUCCEEDED(result) && count != nullptr && data != nullptr && !has_focus()) {
        *count = 0;
    } else if (SUCCEEDED(result) && count != nullptr && data != nullptr && block_cutscene_pause && object_size >= sizeof(DIDEVICEOBJECTDATA)) {
        // Buffered keyboard events: drop the ones of Esc (a mouse has no object at that offset).
        DWORD kept{0};

        for (DWORD i = 0; i < *count; ++i) {
            const auto event = (const DIDEVICEOBJECTDATA*)((const uint8_t*)data + i * object_size);

            if (event->dwOfs != DIK_ESCAPE) {
                std::memmove((uint8_t*)data + kept++ * object_size, event, object_size);
            }
        }

        *count = kept;
    }

    return result;
}

DWORD WINAPI InputFocus::xinput_get_state(DWORD user_index, XINPUT_STATE* state) {
    const auto result = g_focus->m_xinput_hook->get_original<decltype(xinput_get_state)>()(user_index, state);

    if (result == ERROR_SUCCESS && state != nullptr) {
        filter_pad(state);
    }

    return result;
}
