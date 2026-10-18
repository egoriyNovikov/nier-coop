#include <atomic>
#include <cmath>
#include <sstream>
#include <unordered_map>

#include <d3d11.h>
#include <wrl.h>
#include <spdlog/spdlog.h>

#include <sdk/Entity.hpp>
#include <sdk/EntityList.hpp>
#include <sdk/Game.hpp>

#include "../AutomataMP.hpp"
#include "../TestMode.hpp"
#include "TestBot.hpp"

using Microsoft::WRL::ComPtr;

namespace {
TestBot* g_bot{};

// Virtual pad 0 as the bot holds it. Read by the XInput hook on the game thread.
std::atomic<WORD> g_buttons{0};
std::atomic<SHORT> g_stick_x{0};
std::atomic<SHORT> g_stick_y{0};
std::atomic<DWORD> g_packet{0};

constexpr auto RELEASE_GAP = std::chrono::milliseconds{150}; // between two commands, so repeated presses register

const std::unordered_map<std::string, WORD> BUTTONS{
    {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
    {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK},
    {"UP", XINPUT_GAMEPAD_DPAD_UP}, {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
    {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
};
// Triggers are axes in XInput; kept in the high bits of g_buttons, which XInput doesn't use for buttons.
constexpr WORD LT = 0x0400;
constexpr WORD RT = 0x0800;
}

std::optional<std::string> TestBot::on_initialize() {
    if (!testmode::enabled()) {
        return std::nullopt;
    }

    g_bot = this;
    m_out.open(testmode::root() / "coop_test_out.txt", std::ios::out | std::ios::trunc);
    std::filesystem::create_directories(testmode::root() / "shots");

    const auto xinput = GetModuleHandleW(L"xinput1_4.dll");
    const auto get_state = xinput != nullptr ? GetProcAddress(xinput, "XInputGetState") : nullptr;

    if (get_state == nullptr) {
        spdlog::error("[Test] XInputGetState not found, the bot can't press buttons");
    } else {
        m_xinput_hook = std::make_unique<FunctionHook>(get_state, &TestBot::xinput_get_state);

        if (!m_xinput_hook->create()) {
            spdlog::error("[Test] Failed to hook XInputGetState");
        }
    }

    spdlog::info("[Test] Test bot ready, commands from {}", (testmode::root() / "coop_test_cmd.txt").string());
    finish("ready");
    return std::nullopt;
}

// Pad 0 is the bot's: always connected, so the game sees a controller even on a PC without one.
DWORD WINAPI TestBot::xinput_get_state(DWORD user_index, XINPUT_STATE* state) {
    const auto original = g_bot->m_xinput_hook->get_original<decltype(xinput_get_state)>();

    if (user_index != 0 || state == nullptr) {
        return original(user_index, state);
    }

    const auto buttons = g_buttons.load();
    *state = {};
    state->dwPacketNumber = g_packet.load();
    state->Gamepad.wButtons = buttons & ~(LT | RT);
    state->Gamepad.bLeftTrigger = (buttons & LT) ? 255 : 0;
    state->Gamepad.bRightTrigger = (buttons & RT) ? 255 : 0;
    state->Gamepad.sThumbLX = g_stick_x.load();
    state->Gamepad.sThumbLY = g_stick_y.load();
    return ERROR_SUCCESS;
}

void TestBot::on_present() {
    if (g_bot != this) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (m_busy && now >= m_until) {
        if (g_buttons.load() != 0 || g_stick_x.load() != 0 || g_stick_y.load() != 0) {
            // Release first, then give the game a moment before the next command.
            g_buttons = 0;
            g_stick_x = 0;
            g_stick_y = 0;
            ++g_packet;
            m_until = now + RELEASE_GAP;
            return;
        }

        m_busy = false;
        finish("ok " + m_current);
    }

    if (++m_frame % 15 == 0) {
        read_commands();
    }

    if (!m_busy && !m_queue.empty()) {
        const auto command = m_queue.front();
        m_queue.pop_front();
        start(command);
    }
}

void TestBot::read_commands() {
    const auto path = testmode::root() / "coop_test_cmd.txt";
    std::error_code ec{};

    if (!std::filesystem::exists(path, ec)) {
        return;
    }

    std::ifstream file{path};
    std::string line{};

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty() || line[0] == '#') {
            continue;
        }

        Command command{};
        command.text = line;
        std::istringstream words{line};
        words >> command.verb >> command.arg1 >> command.arg2 >> command.arg3;
        m_queue.push_back(command);
    }

    file.close();
    std::filesystem::remove(path, ec);
}

void TestBot::start(const Command& command) {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = [](const std::string& s) { return std::chrono::milliseconds{s.empty() ? 0 : std::stoi(s)}; };

    m_current = command.text;

    try {
        if (command.verb == "pad") {
            WORD buttons{0};
            std::istringstream names{command.arg1};
            std::string name{};

            while (std::getline(names, name, '+')) {
                if (name == "LT") {
                    buttons |= LT;
                } else if (name == "RT") {
                    buttons |= RT;
                } else if (const auto it = BUTTONS.find(name); it != BUTTONS.end()) {
                    buttons |= it->second;
                } else {
                    finish("error unknown button " + name + ": " + command.text);
                    return;
                }
            }

            g_buttons = buttons;
            ++g_packet;
            m_busy = true;
            m_until = now + ms(command.arg2);
        } else if (command.verb == "stick") {
            g_stick_x = (SHORT)std::clamp(std::stof(command.arg1) * 32767.0f, -32767.0f, 32767.0f);
            g_stick_y = (SHORT)std::clamp(std::stof(command.arg2) * 32767.0f, -32767.0f, 32767.0f);
            ++g_packet;
            m_busy = true;
            m_until = now + ms(command.arg3);
        } else if (command.verb == "wait") {
            m_busy = true;
            m_until = now + ms(command.arg1);
        } else if (command.verb == "shot") {
            take_screenshot(command.arg1.empty() ? "shot" : command.arg1);
        } else if (command.verb == "state") {
            finish(describe_state());
        } else {
            finish("error unknown command: " + command.text);
        }
    } catch (const std::exception& e) {
        finish(std::string{"error "} + e.what() + ": " + command.text);
    }
}

void TestBot::finish(const std::string& line) {
    spdlog::info("[Test] {}", line);
    m_out << line << std::endl;
}

std::string TestBot::describe_state() {
    std::ostringstream s{};
    s << "state loading=" << sdk::is_loading();

    if (!sdk::is_loading()) {
        const auto entity_list = sdk::EntityList::get();
        const auto player = entity_list != nullptr ? entity_list->get_by_name("Player") : nullptr;

        if (player != nullptr && player->behavior != nullptr) {
            const auto& p = player->behavior->position();
            s << " player=" << p.x << "," << p.y << "," << p.z;
        } else {
            s << " player=none";
        }
    }

    return s.str();
}

// Copies the back buffer before the mod's own UI is drawn on it and writes a 32-bit BMP.
void TestBot::take_screenshot(const std::string& name) {
    const auto& hook = g_framework->get_d3d11_hook();
    const auto device = hook != nullptr ? hook->get_device() : nullptr;
    const auto swap_chain = hook != nullptr ? hook->get_swap_chain() : nullptr;

    if (device == nullptr || swap_chain == nullptr) {
        finish("error shot: no D3D11 device");
        return;
    }

    ComPtr<ID3D11Texture2D> back_buffer{};
    ComPtr<ID3D11DeviceContext> context{};

    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        finish("error shot: no back buffer");
        return;
    }

    device->GetImmediateContext(&context);

    D3D11_TEXTURE2D_DESC desc{};
    back_buffer->GetDesc(&desc);

    ComPtr<ID3D11Texture2D> source = back_buffer;

    if (desc.SampleDesc.Count > 1) {
        auto resolved_desc = desc;
        resolved_desc.SampleDesc = {1, 0};
        resolved_desc.BindFlags = 0;
        ComPtr<ID3D11Texture2D> resolved{};
        device->CreateTexture2D(&resolved_desc, nullptr, &resolved);
        context->ResolveSubresource(resolved.Get(), 0, back_buffer.Get(), 0, desc.Format);
        source = resolved;
    }

    auto staging_desc = desc;
    staging_desc.SampleDesc = {1, 0};
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging{};

    if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
        finish("error shot: can't create a staging texture");
        return;
    }

    context->CopyResource(staging.Get(), source.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};

    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        finish("error shot: can't map the staging texture");
        return;
    }

    const auto rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const auto bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    if (!rgba && !bgra) {
        context->Unmap(staging.Get(), 0);
        finish("error shot: unsupported back buffer format " + std::to_string(desc.Format));
        return;
    }

    const auto path = testmode::root() / "shots" / (name + ".bmp");
    std::ofstream file{path, std::ios::binary};

    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = (LONG)desc.Width;
    info.biHeight = -(LONG)desc.Height; // top-down
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    file_header.bfType = 'MB';
    file_header.bfOffBits = sizeof(file_header) + sizeof(info);
    file_header.bfSize = file_header.bfOffBits + desc.Width * desc.Height * 4;

    file.write((const char*)&file_header, sizeof(file_header));
    file.write((const char*)&info, sizeof(info));

    std::vector<uint8_t> row(desc.Width * 4);

    for (UINT y = 0; y < desc.Height; ++y) {
        const auto src = (const uint8_t*)mapped.pData + y * mapped.RowPitch;

        for (UINT x = 0; x < desc.Width; ++x) {
            row[x * 4 + 0] = src[x * 4 + (rgba ? 2 : 0)];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + (rgba ? 0 : 2)];
            row[x * 4 + 3] = 255;
        }

        file.write((const char*)row.data(), row.size());
    }

    context->Unmap(staging.Get(), 0);
    finish("shot " + path.string());
}
