/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/engine.h"
#include "engines/melonds/nds_adapter_test.h"
#include "engines/melonds/nds_platform_bridge.hpp"

#include <Args.h>
#include <NDS.h>
#include <NDSCart.h>
#include <SPI_Firmware.h>
#include <net/LocalMP.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr unsigned kMachineCount = DUALBOY_MACHINE_COUNT;
constexpr unsigned kScreenWidth = 256U;
constexpr unsigned kScreenHeight = 192U;
constexpr unsigned kMachineHeight = kScreenHeight * 2U;
constexpr std::size_t kMachinePixels =
    static_cast<std::size_t>(kScreenWidth) * kMachineHeight;
constexpr std::uint32_t kContextMagic = UINT32_C(0x44424e44); /* DBND */
constexpr std::uint32_t kMaximumRomSize = UINT32_C(512) * 1024U * 1024U;
constexpr double kFrameRate = 59.8260982880808;
constexpr std::chrono::milliseconds kFrameDeadline{10000};
constexpr std::size_t kFirmwareIdentifierOffset = 0x08U;
constexpr std::size_t kFirmwareConsoleTypeOffset = 0x1dU;
constexpr std::size_t kFirmwareUserSettingsOffset = 0x20U;
constexpr std::size_t kFirmwareWifiConfigLengthOffset = 0x2cU;
constexpr std::uint16_t kGeneratedUserSettingsOffset = 0x3fc0U;
static_assert(offsetof(melonDS::Firmware::FirmwareHeader, Identifier) ==
              kFirmwareIdentifierOffset);
static_assert(offsetof(melonDS::Firmware::FirmwareHeader, ConsoleType) ==
              kFirmwareConsoleTypeOffset);
static_assert(offsetof(melonDS::Firmware::FirmwareHeader, UserSettingsOffset) ==
              kFirmwareUserSettingsOffset);
static_assert(offsetof(melonDS::Firmware::FirmwareHeader, WifiConfigLength) ==
              kFirmwareWifiConfigLengthOffset);
constexpr std::array<std::uint8_t, 6U> kDefaultMac0{{
    UINT8_C(0x02), UINT8_C(0x44), UINT8_C(0x42),
    UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00),
}};
constexpr std::array<std::uint8_t, 6U> kDefaultMac1{{
    UINT8_C(0x02), UINT8_C(0x44), UINT8_C(0x42),
    UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x01),
}};

std::mutex g_platform_config_mutex;
dualboy_log_fn g_platform_log = nullptr;
void *g_platform_log_context = nullptr;
std::string g_platform_system_directory = ".";

struct MelonDSPair;

enum class FrameState {
    Idle,
    Running,
    DeadlineExceeded,
    Poisoned,
    ShuttingDown,
};

struct MachineContext {
    std::uint32_t magic = kContextMagic;
    MelonDSPair *pair = nullptr;
    unsigned id = 0U;
    std::unique_ptr<melonDS::NDS> nds;
    std::array<std::uint32_t, kMachinePixels> video{};
    std::vector<std::uint8_t> save_shadow;
    std::vector<std::int16_t> audio;
    std::size_t audio_offset_frames = 0U;
    struct dualboy_machine_input input{};
    std::string rom_name;
    std::atomic<bool> save_dirty{false};
    std::atomic<bool> firmware_dirty{false};
    std::atomic<bool> mp_requested{false};
    std::atomic<bool> mp_registered{false};
    std::atomic<int> stop_reason{-1};
    std::atomic<std::uint64_t> completed_frames{0U};
    bool last_touch_active = false;
    std::uint16_t last_touch_x = 0U;
    std::uint16_t last_touch_y = 0U;
};

struct MelonDSPair {
    explicit MelonDSPair(const struct dualboy_engine_config &source_config)
        : config(source_config)
    {
        for (unsigned machine = 0U; machine < kMachineCount; ++machine) {
            machines[machine].pair = this;
            machines[machine].id = machine;
        }
        local_mp.SetRecvTimeout(25);
        try {
            for (unsigned machine = 0U; machine < kMachineCount; ++machine) {
                workers[machine] =
                    std::thread([this, machine]() { WorkerMain(machine); });
                worker_started[machine] = true;
            }
        } catch (...) {
            ShutdownWorkers();
            throw;
        }
    }

    MelonDSPair(const MelonDSPair &) = delete;
    MelonDSPair &operator=(const MelonDSPair &) = delete;

    ~MelonDSPair()
    {
        ShutdownWorkers();
        SetLink(false);
        for (auto &machine : machines) {
            machine.nds.reset();
            machine.magic = 0U;
            machine.pair = nullptr;
        }
    }

    void ShutdownWorkers()
    {
        {
            std::lock_guard<std::mutex> lock(frame_mutex);
            if (frame_state != FrameState::ShuttingDown) {
                frame_state = FrameState::ShuttingDown;
                abort_requested.store(true, std::memory_order_release);
                ++generation;
            }
        }
        start_cv.notify_all();
        state_cv.notify_all();
        for (unsigned machine = 0U; machine < kMachineCount; ++machine) {
            if (worker_started[machine] && workers[machine].joinable()) {
                workers[machine].join();
            }
            worker_started[machine] = false;
        }
    }

    bool SetLink(bool enabled)
    {
        std::lock_guard<std::mutex> lock(mp_control_mutex);

        link_enabled.store(enabled, std::memory_order_release);
        for (unsigned machine = 0U; machine < kMachineCount; ++machine) {
            MachineContext &context = machines[machine];
            const bool requested =
                context.mp_requested.load(std::memory_order_acquire);
            const bool registered =
                context.mp_registered.load(std::memory_order_acquire);
            if (enabled && requested && !registered) {
                local_mp.Begin(static_cast<int>(machine));
                context.mp_registered.store(true, std::memory_order_release);
            } else if (!enabled && registered) {
                context.mp_registered.store(false, std::memory_order_release);
                local_mp.End(static_cast<int>(machine));
            }
        }
        return true;
    }

    void MPBegin(MachineContext &context)
    {
        context.mp_requested.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mp_control_mutex);
        if (link_enabled.load(std::memory_order_acquire) &&
            !context.mp_registered.load(std::memory_order_acquire)) {
            local_mp.Begin(static_cast<int>(context.id));
            context.mp_registered.store(true, std::memory_order_release);
        }
    }

    void MPEnd(MachineContext &context)
    {
        context.mp_requested.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mp_control_mutex);
        if (context.mp_registered.exchange(false,
                                           std::memory_order_acq_rel)) {
            local_mp.End(static_cast<int>(context.id));
        }
    }

    bool TransportActive() const
    {
        return link_enabled.load(std::memory_order_acquire) &&
               machines[0].mp_registered.load(std::memory_order_acquire) &&
               machines[1].mp_registered.load(std::memory_order_acquire);
    }

    void WorkerMain(unsigned machine_id) noexcept
    {
        std::uint64_t observed_generation = 0U;
        MachineContext &machine = machines[machine_id];

        for (;;) {
            {
                std::unique_lock<std::mutex> lock(frame_mutex);
                start_cv.wait(lock, [this, observed_generation]() {
                    return frame_state == FrameState::ShuttingDown ||
                           generation > observed_generation;
                });
                if (frame_state == FrameState::ShuttingDown) {
                    return;
                }
                observed_generation = generation;

                if (debug_hold_next_frame[machine_id]) {
                    debug_hold_next_frame[machine_id] = false;
                    debug_frame_held[machine_id] = true;
                    state_cv.notify_all();
                    state_cv.wait(lock, [this, machine_id]() {
                        return frame_state == FrameState::ShuttingDown ||
                               !debug_frame_held[machine_id];
                    });
                    if (frame_state == FrameState::ShuttingDown) {
                        return;
                    }
                }
            }

            bool success = true;
            std::array<char, 256U> message{};
            try {
                if (abort_requested.load(std::memory_order_acquire)) {
                    success = false;
                    (void)std::snprintf(message.data(), message.size(), "%s",
                                        "frame abandoned after worker deadline");
                } else if (machine.nds == nullptr) {
                    success = false;
                    (void)std::snprintf(message.data(), message.size(), "%s",
                                        "worker has no Nintendo DS instance");
                } else {
                    ApplyInput(machine);
                    (void)machine.nds->RunFrame();
                    if (abort_requested.load(std::memory_order_acquire)) {
                        success = false;
                        (void)std::snprintf(
                            message.data(), message.size(), "%s",
                            "frame abandoned after worker deadline");
                    } else if (!SnapshotVideo(machine)) {
                        success = false;
                        (void)std::snprintf(
                            message.data(), message.size(), "%s",
                            "melonDS did not expose software framebuffers");
                    }
                    if (!abort_requested.load(std::memory_order_acquire)) {
                        DrainAudio(machine);
                        machine.completed_frames.fetch_add(
                            1U, std::memory_order_relaxed);
                    }
                    if (!machine.nds->IsRunning()) {
                        success = false;
                        if (message[0] == '\0') {
                            (void)std::snprintf(
                                message.data(), message.size(), "%s",
                                "Nintendo DS instance stopped");
                        }
                    }
                }
            } catch (const std::exception &exception) {
                success = false;
                (void)std::snprintf(message.data(), message.size(), "%s",
                                    exception.what());
            } catch (...) {
                success = false;
                (void)std::snprintf(
                    message.data(), message.size(), "%s",
                    "unknown exception in Nintendo DS frame worker");
            }

            if (!success) {
                /* Wifi::Reset and NDS::Stop do not themselves call MP_End.
                 * Remove a failed machine before publishing the quiescent
                 * completion so stale LocalMP registration cannot keep the
                 * peer or frontend fast-forward policy active. */
                MPEnd(machine);
            }

            {
                std::lock_guard<std::mutex> lock(frame_mutex);
                worker_success[machine_id] = success;
                worker_error[machine_id] = message;
                ++completed_workers;
            }
            done_cv.notify_one();
        }
    }

    static void ApplyInput(MachineContext &machine)
    {
        const std::uint16_t buttons = machine.input.buttons;
        std::uint32_t pressed = 0U;

        if ((buttons & DUALBOY_BUTTON_A) != 0U) pressed |= 1U << 0U;
        if ((buttons & DUALBOY_BUTTON_B) != 0U) pressed |= 1U << 1U;
        if ((buttons & DUALBOY_BUTTON_SELECT) != 0U) pressed |= 1U << 2U;
        if ((buttons & DUALBOY_BUTTON_START) != 0U) pressed |= 1U << 3U;
        if ((buttons & DUALBOY_BUTTON_RIGHT) != 0U) pressed |= 1U << 4U;
        if ((buttons & DUALBOY_BUTTON_LEFT) != 0U) pressed |= 1U << 5U;
        if ((buttons & DUALBOY_BUTTON_UP) != 0U) pressed |= 1U << 6U;
        if ((buttons & DUALBOY_BUTTON_DOWN) != 0U) pressed |= 1U << 7U;
        if ((buttons & DUALBOY_BUTTON_R) != 0U) pressed |= 1U << 8U;
        if ((buttons & DUALBOY_BUTTON_L) != 0U) pressed |= 1U << 9U;
        if ((buttons & DUALBOY_BUTTON_X) != 0U) pressed |= 1U << 10U;
        if ((buttons & DUALBOY_BUTTON_Y) != 0U) pressed |= 1U << 11U;
        machine.nds->SetKeyMask((~pressed) & UINT32_C(0x0fff));

        machine.last_touch_active = machine.input.touch_active;
        machine.last_touch_x = static_cast<std::uint16_t>(
            std::min<unsigned>(machine.input.touch_x, kScreenWidth - 1U));
        machine.last_touch_y = static_cast<std::uint16_t>(
            std::min<unsigned>(machine.input.touch_y, kScreenHeight - 1U));
        if (machine.last_touch_active) {
            machine.nds->TouchScreen(machine.last_touch_x,
                                     machine.last_touch_y);
        } else {
            machine.nds->ReleaseScreen();
        }
    }

    static bool SnapshotVideo(MachineContext &machine)
    {
        void *top = nullptr;
        void *bottom = nullptr;

        if (!machine.nds->GPU.GetFramebuffers(&top, &bottom) || top == nullptr ||
            bottom == nullptr) {
            return false;
        }
        const auto *top_pixels = static_cast<const std::uint32_t *>(top);
        const auto *bottom_pixels = static_cast<const std::uint32_t *>(bottom);
        std::copy_n(top_pixels,
                    static_cast<std::size_t>(kScreenWidth) * kScreenHeight,
                    machine.video.begin());
        std::copy_n(bottom_pixels,
                    static_cast<std::size_t>(kScreenWidth) * kScreenHeight,
                    machine.video.begin() +
                        static_cast<std::ptrdiff_t>(kScreenWidth *
                                                    kScreenHeight));
        return true;
    }

    static void DrainAudio(MachineContext &machine)
    {
        int available = machine.nds->SPU.GetOutputSize();
        if (available < 0) {
            available = 0;
        }
        if (machine.id == 0U) {
            machine.audio.clear();
            machine.audio_offset_frames = 0U;
            if (available > 0) {
                machine.audio.resize(static_cast<std::size_t>(available) * 2U);
                const int read =
                    machine.nds->SPU.ReadOutput(machine.audio.data(), available);
                if (read < available) {
                    machine.audio.resize(
                        static_cast<std::size_t>(std::max(read, 0)) * 2U);
                }
            }
            return;
        }

        std::array<std::int16_t, 1024U * 2U> discard{};
        while (available > 0) {
            const int request = std::min<int>(available, 1024);
            const int read = machine.nds->SPU.ReadOutput(discard.data(), request);
            if (read <= 0) {
                break;
            }
            available -= read;
        }
    }

    struct dualboy_engine_config config{};
    melonDS::LocalMP local_mp;
    std::array<MachineContext, kMachineCount> machines{};
    std::array<std::thread, kMachineCount> workers{};
    std::array<bool, kMachineCount> worker_started{};
    mutable std::mutex frame_mutex;
    std::condition_variable start_cv;
    std::condition_variable done_cv;
    std::uint64_t generation = 0U;
    unsigned completed_workers = 0U;
    std::array<bool, kMachineCount> worker_success{{true, true}};
    std::array<std::array<char, 256U>, kMachineCount> worker_error{};
    FrameState frame_state = FrameState::Idle;
    std::atomic<bool> abort_requested{false};
    std::chrono::milliseconds frame_deadline{kFrameDeadline};
    std::array<bool, kMachineCount> debug_hold_next_frame{};
    std::array<bool, kMachineCount> debug_frame_held{};
    std::condition_variable state_cv;
    std::mutex mp_control_mutex;
    std::atomic<bool> link_enabled{false};
};

static MachineContext *ContextFromUserdata(void *userdata)
{
    auto *context = static_cast<MachineContext *>(userdata);
    if (context == nullptr || context->magic != kContextMagic ||
        context->pair == nullptr || context->id >= kMachineCount ||
        &context->pair->machines[context->id] != context) {
        return nullptr;
    }
    return context;
}

static MelonDSPair *AsPair(void *pair)
{
    return static_cast<MelonDSPair *>(pair);
}

static const MelonDSPair *AsPair(const void *pair)
{
    return static_cast<const MelonDSPair *>(pair);
}

static void SetError(char *error,
                     std::size_t error_size,
                     const char *format,
                     ...)
{
    if (error == nullptr || error_size == 0U) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    (void)std::vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static std::string CartridgeName(const char *path, unsigned machine)
{
    if (path == nullptr || path[0] == '\0') {
        return "dualboy-player" + std::to_string(machine + 1U) + ".nds";
    }
    const char *slash = std::strrchr(path, '/');
    const char *backslash = std::strrchr(path, '\\');
    const char *base = path;
    if (slash != nullptr) base = slash + 1;
    if (backslash != nullptr && backslash + 1 > base) base = backslash + 1;
    return base[0] != '\0' ? std::string(base)
                            : "dualboy-player" +
                                  std::to_string(machine + 1U) + ".nds";
}

static bool CreatePair(void **output,
                       const struct dualboy_engine_config *config,
                       char *error,
                       std::size_t error_size)
{
    if (output == nullptr || config == nullptr) {
        SetError(error, error_size, "invalid melonDS pair configuration");
        return false;
    }
    *output = nullptr;
    dualboy_melonds_platform::Configure(config->log, config->log_context,
                                        config->system_directory);
    try {
        *output = new MelonDSPair(*config);
        return true;
    } catch (const std::exception &exception) {
        SetError(error, error_size, "could not create melonDS pair: %s",
                 exception.what());
    } catch (...) {
        SetError(error, error_size,
                 "could not create melonDS pair: unknown exception");
    }
    dualboy_melonds_platform::ClearConfiguration();
    return false;
}

static bool LoadRom(void *opaque_pair,
                    unsigned machine,
                    const struct dualboy_rom *rom,
                    char *error,
                    std::size_t error_size)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount || rom == nullptr ||
        rom->platform != DUALBOY_PLATFORM_NDS || rom->data == nullptr ||
        rom->size < 0x1000U || rom->size > kMaximumRomSize ||
        rom->size > std::numeric_limits<std::uint32_t>::max()) {
        SetError(error, error_size,
                 "invalid Nintendo DS cartridge for machine %u", machine + 1U);
        return false;
    }
    MachineContext &context = pair->machines[machine];
    if (context.nds != nullptr) {
        SetError(error, error_size, "machine %u is already loaded", machine + 1U);
        return false;
    }

    try {
        melonDS::Firmware firmware(0);
        auto &mac = firmware.GetHeader().MacAddr;
        mac = machine == 0U ? kDefaultMac0 : kDefaultMac1;
        firmware.UpdateChecksums();

        melonDS::NDSArgs arguments;
        arguments.Firmware = std::move(firmware);
        arguments.OutputSampleRate =
            pair->config.audio_sample_rate != 0U
                ? static_cast<double>(pair->config.audio_sample_rate)
                : 48000.0;
        arguments.JIT = std::nullopt;

        context.nds =
            std::make_unique<melonDS::NDS>(std::move(arguments), &context);
        auto cartridge = melonDS::NDSCart::ParseROM(
            rom->data, static_cast<std::uint32_t>(rom->size), &context);
        if (cartridge == nullptr) {
            context.nds.reset();
            SetError(error, error_size,
                     "melonDS rejected machine %u cartridge", machine + 1U);
            return false;
        }
        context.rom_name = CartridgeName(rom->path, machine);
        context.nds->SetNDSCart(std::move(cartridge));
        context.nds->Reset();
        if (context.nds->NeedsDirectBoot()) {
            context.nds->SetupDirectBoot(context.rom_name);
        }
        context.nds->Start();

        const std::size_t save_size = context.nds->GetNDSSaveLength();
        const std::uint8_t *save = context.nds->GetNDSSave();
        context.save_shadow.assign(save_size, UINT8_C(0xff));
        if (save != nullptr && save_size != 0U) {
            std::memcpy(context.save_shadow.data(), save, save_size);
        }
        context.save_dirty.store(false, std::memory_order_release);
        context.firmware_dirty.store(false, std::memory_order_release);
        return true;
    } catch (const std::exception &exception) {
        context.nds.reset();
        context.save_shadow.clear();
        SetError(error, error_size, "machine %u load failed: %s", machine + 1U,
                 exception.what());
    } catch (...) {
        context.nds.reset();
        context.save_shadow.clear();
        SetError(error, error_size,
                 "machine %u load failed with an unknown exception",
                 machine + 1U);
    }
    return false;
}

static bool SetLink(void *opaque_pair,
                    bool enabled,
                    char *error,
                    std::size_t error_size)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr) {
        SetError(error, error_size, "no melonDS pair is loaded");
        return false;
    }
    return pair->SetLink(enabled);
}

static void Reset(void *opaque_pair)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr) return;
    std::lock_guard<std::mutex> lock(pair->frame_mutex);
    if (pair->frame_state != FrameState::Idle) return;

    /* melonDS resets Wifi::PowerOn directly without emitting MP_End. Clear
     * both upstream registrations and their request state before resetting so
     * a later guest power-on starts from fresh LocalMP read offsets/queues. */
    for (MachineContext &machine : pair->machines) {
        pair->MPEnd(machine);
    }
    for (MachineContext &machine : pair->machines) {
        if (machine.nds == nullptr) continue;
        machine.nds->Reset();
        if (machine.nds->NeedsDirectBoot()) {
            machine.nds->SetupDirectBoot(machine.rom_name);
        }
        machine.nds->Start();
        if (!machine.save_shadow.empty()) {
            machine.save_dirty.store(false, std::memory_order_release);
            machine.nds->SetNDSSave(machine.save_shadow.data(),
                                    static_cast<std::uint32_t>(
                                        machine.save_shadow.size()));
            machine.save_dirty.store(false, std::memory_order_release);
        }
        machine.stop_reason.store(-1, std::memory_order_release);
    }
}

static bool IsValidPairMac(const std::array<std::uint8_t, 6U> &mac)
{
    /* Pair-managed firmware uses locally administered unicast addresses. */
    return (mac[0] & UINT8_C(0x03)) == UINT8_C(0x02);
}

static void PersistentMemoryLoaded(void *opaque_pair)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr) return;

    {
        std::lock_guard<std::mutex> lock(pair->frame_mutex);
        if (pair->frame_state != FrameState::Idle) return;

        std::array<std::array<std::uint8_t, 6U>, kMachineCount> macs{};
        for (unsigned machine = 0U; machine < kMachineCount; ++machine) {
            MachineContext &context = pair->machines[machine];
            if (context.nds == nullptr) return;
            macs[machine] = context.nds->GetFirmware().GetHeader().MacAddr;
        }

        if (!IsValidPairMac(macs[0])) macs[0] = kDefaultMac0;
        if (!IsValidPairMac(macs[1])) macs[1] = kDefaultMac1;
        if (macs[1] == macs[0]) {
            macs[1] = macs[0];
            macs[1][5] ^= UINT8_C(0x01);
        }

        for (unsigned machine = 0U; machine < kMachineCount; ++machine) {
            MachineContext &context = pair->machines[machine];
            auto &firmware = context.nds->GetFirmware();
            if (firmware.GetHeader().MacAddr != macs[machine]) {
                firmware.GetHeader().MacAddr = macs[machine];
                firmware.UpdateChecksums();
                context.firmware_dirty.store(true, std::memory_order_release);
            }
        }
    }

    /* Rebuild firmware-derived emulator state only after both complete images
     * and any identity repairs have been applied. Reset is also the LocalMP
     * synchronization boundary for a newly loaded persistent session. */
    Reset(opaque_pair);
}

static void SetLegacyInput(void *opaque_pair,
                           unsigned machine,
                           std::uint16_t buttons)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount) return;
    std::lock_guard<std::mutex> lock(pair->frame_mutex);
    if (pair->frame_state != FrameState::Idle) return;
    pair->machines[machine].input.buttons = buttons;
    pair->machines[machine].input.touch_active = false;
}

static void SetMachineInput(void *opaque_pair,
                            unsigned machine,
                            const struct dualboy_machine_input *input)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount || input == nullptr) return;
    std::lock_guard<std::mutex> lock(pair->frame_mutex);
    if (pair->frame_state != FrameState::Idle) return;
    pair->machines[machine].input = *input;
}

static bool RunFrame(void *opaque_pair, char *error, std::size_t error_size)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    std::chrono::milliseconds deadline{0};
    bool deadline_exceeded = false;

    if (pair == nullptr || pair->machines[0].nds == nullptr ||
        pair->machines[1].nds == nullptr) {
        SetError(error, error_size, "both Nintendo DS machines must be loaded");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(pair->frame_mutex);
        if (pair->frame_state == FrameState::Poisoned) {
            SetError(error, error_size,
                     "Nintendo DS workers are unavailable after a frame timeout");
            return false;
        }
        if (pair->frame_state != FrameState::Idle) {
            SetError(error, error_size,
                     "a Nintendo DS frame is already running");
            return false;
        }
        deadline = pair->frame_deadline;
    }

    /* save_shadow is the complete image updated by WriteNDSSave's committed
     * ranges, not a per-frame authority. CartRetail can modify live SRAM over
     * multiple frames before SPIRelease publishes that transaction. Only
     * validate topology here; Reset owns explicit shadow-to-live loads. */
    for (const MachineContext &machine : pair->machines) {
        const std::size_t live_size = machine.nds->GetNDSSaveLength();
        const std::uint8_t *const live_save = machine.nds->GetNDSSave();
        if (live_size != machine.save_shadow.size() ||
            (live_size != 0U && live_save == nullptr)) {
            SetError(error, error_size,
                     "machine %u cartridge save extent changed unexpectedly",
                     machine.id + 1U);
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(pair->frame_mutex);
        pair->completed_workers = 0U;
        pair->worker_success = {{true, true}};
        pair->worker_error = {};
        pair->abort_requested.store(false, std::memory_order_release);
        pair->frame_state = FrameState::Running;
        ++pair->generation;
    }
    pair->start_cv.notify_all();

    {
        std::unique_lock<std::mutex> lock(pair->frame_mutex);
        const bool completed = pair->done_cv.wait_for(
            lock, deadline, [pair]() {
                return pair->completed_workers == kMachineCount;
            });
        if (!completed) {
            /* melonDS offers no thread-safe force-cancellation boundary. Mark
             * the pair unusable, but retain ownership until every dispatched
             * worker is quiescent so no caller can race exposed engine data. */
            deadline_exceeded = true;
            pair->frame_state = FrameState::DeadlineExceeded;
            pair->abort_requested.store(true, std::memory_order_release);
            pair->state_cv.notify_all();
            pair->done_cv.wait(lock, [pair]() {
                return pair->completed_workers == kMachineCount;
            });
        }
        pair->frame_state = deadline_exceeded ? FrameState::Poisoned
                                              : FrameState::Idle;
        pair->state_cv.notify_all();
        if (deadline_exceeded) {
            /* Worker-owned output may be partial even though it is now safe to
             * access. Suppress it and require a content reload for recovery. */
            pair->machines[0].audio.clear();
            pair->machines[0].audio_offset_frames = 0U;
        }
        for (unsigned machine = 0U; machine < kMachineCount; ++machine) {
            if (!deadline_exceeded && !pair->worker_success[machine]) {
                SetError(error, error_size, "machine %u frame failed: %s",
                         machine + 1U,
                         pair->worker_error[machine][0] == '\0'
                             ? "unknown worker error"
                             : pair->worker_error[machine].data());
                return false;
            }
        }
    }
    if (deadline_exceeded) {
        (void)pair->SetLink(false);
        SetError(error, error_size,
                 "Nintendo DS frame workers timed out after %lld milliseconds",
                 static_cast<long long>(deadline.count()));
        return false;
    }
    return true;
}

static bool VideoFrame(void *opaque_pair,
                       unsigned machine,
                       struct dualboy_video_frame *frame)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount || frame == nullptr ||
        pair->machines[machine].nds == nullptr) {
        return false;
    }
    frame->pixels = pair->machines[machine].video.data();
    frame->width = kScreenWidth;
    frame->height = kMachineHeight;
    frame->pitch = static_cast<std::size_t>(kScreenWidth) *
                   sizeof(std::uint32_t);
    return true;
}

static unsigned AudioSampleRate(const void *opaque_pair)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || pair->config.audio_sample_rate == 0U) return 48000U;
    return pair->config.audio_sample_rate;
}

static double FrameRate(const void *)
{
    return kFrameRate;
}

static std::size_t ReadAudio(void *opaque_pair,
                             std::int16_t *output,
                             std::size_t max_frames)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || output == nullptr || max_frames == 0U) return 0U;
    MachineContext &machine = pair->machines[0];
    const std::size_t total_frames = machine.audio.size() / 2U;
    if (machine.audio_offset_frames >= total_frames) return 0U;
    const std::size_t count = std::min(
        max_frames, total_frames - machine.audio_offset_frames);
    std::copy_n(machine.audio.data() + machine.audio_offset_frames * 2U,
                count * 2U, output);
    machine.audio_offset_frames += count;
    return count;
}

static bool MemoryInfo(void *opaque_pair,
                       unsigned machine,
                       enum dualboy_memory_kind kind,
                       void **data,
                       std::size_t *size)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (data != nullptr) *data = nullptr;
    if (size != nullptr) *size = 0U;
    if (pair == nullptr || machine >= kMachineCount || data == nullptr ||
        size == nullptr || pair->machines[machine].nds == nullptr) {
        return false;
    }
    MachineContext &context = pair->machines[machine];
    switch (kind) {
        case DUALBOY_MEMORY_SAVE_RAM:
            *data = context.save_shadow.empty() ? nullptr
                                                : context.save_shadow.data();
            *size = context.save_shadow.size();
            return true;
        case DUALBOY_MEMORY_RTC:
            return true;
        case DUALBOY_MEMORY_FIRMWARE:
            *data = context.nds->GetFirmware().Buffer();
            *size = context.nds->GetFirmware().Length();
            return *data != nullptr || *size == 0U;
        default:
            return false;
    }
}

static bool PersistentMemoryExtent(const void *opaque_pair,
                                   unsigned machine,
                                   enum dualboy_memory_kind kind,
                                   bool *known,
                                   std::size_t *size)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount || known == nullptr ||
        size == nullptr || pair->machines[machine].nds == nullptr) {
        return false;
    }
    *known = true;
    switch (kind) {
        case DUALBOY_MEMORY_SAVE_RAM:
            *size = pair->machines[machine].save_shadow.size();
            return true;
        case DUALBOY_MEMORY_RTC:
            *size = 0U;
            return true;
        case DUALBOY_MEMORY_FIRMWARE:
            *size = pair->machines[machine].nds->GetFirmware().Length();
            return true;
        default:
            return false;
    }
}

static std::uint16_t ReadLittleEndian16(const std::uint8_t *data)
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8U));
}

static bool ValidatePersistentMemory(const void *opaque_pair,
                                     unsigned machine,
                                     enum dualboy_memory_kind kind,
                                     const void *data,
                                     std::size_t size,
                                     char *error,
                                     std::size_t error_size)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    if (pair == nullptr || machine >= kMachineCount ||
        pair->machines[machine].nds == nullptr ||
        kind != DUALBOY_MEMORY_FIRMWARE || bytes == nullptr ||
        size != melonDS::DEFAULT_FIRMWARE_LENGTH) {
        SetError(error, error_size,
                 "invalid persisted NDS firmware for machine %u",
                 machine + 1U);
        return false;
    }
    if (!std::equal(melonDS::GENERATED_FIRMWARE_IDENTIFIER.begin(),
                    melonDS::GENERATED_FIRMWARE_IDENTIFIER.end(),
                    bytes + kFirmwareIdentifierOffset)) {
        SetError(error, error_size,
                 "machine %u firmware is not a DualBoy-generated NDS image",
                 machine + 1U);
        return false;
    }
    if (bytes[kFirmwareConsoleTypeOffset] !=
        static_cast<std::uint8_t>(
            melonDS::Firmware::FirmwareConsoleType::DSLite)) {
        SetError(error, error_size,
                 "machine %u firmware has an unsupported console type",
                 machine + 1U);
        return false;
    }

    const std::uint16_t encoded_user_offset =
        ReadLittleEndian16(bytes + kFirmwareUserSettingsOffset);
    const std::size_t user_offset =
        static_cast<std::size_t>(encoded_user_offset) << 3U;
    constexpr std::size_t kUserDataBytes =
        2U * sizeof(melonDS::Firmware::UserData);
    constexpr std::size_t kAccessPointPrefix = 0x400U;
    constexpr std::size_t kAccessPointBytes =
        3U * sizeof(melonDS::Firmware::WifiAccessPoint);
    if (encoded_user_offset != kGeneratedUserSettingsOffset ||
        user_offset < kAccessPointPrefix || user_offset > size ||
        kUserDataBytes > size - user_offset ||
        kAccessPointBytes > size - (user_offset - kAccessPointPrefix)) {
        SetError(error, error_size,
                 "machine %u firmware has an unsafe user-settings layout",
                 machine + 1U);
        return false;
    }

    const std::size_t wifi_length = ReadLittleEndian16(
        bytes + kFirmwareWifiConfigLengthOffset);
    if (wifi_length > size - kFirmwareWifiConfigLengthOffset) {
        SetError(error, error_size,
                 "machine %u firmware has an unsafe Wi-Fi checksum range",
                 machine + 1U);
        return false;
    }
    return true;
}

static bool MemoryDirty(const void *opaque_pair, unsigned machine)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    return pair != nullptr && machine < kMachineCount &&
           (pair->machines[machine].save_dirty.load(std::memory_order_acquire) ||
            pair->machines[machine].firmware_dirty.load(
                std::memory_order_acquire));
}

static void ClearMemoryDirty(void *opaque_pair, unsigned machine)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount) return;
    pair->machines[machine].save_dirty.store(false, std::memory_order_release);
    pair->machines[machine].firmware_dirty.store(false,
                                                 std::memory_order_release);
}

static bool LinkTransportActive(const void *opaque_pair)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    return pair != nullptr && pair->TransportActive();
}

static void DestroyPair(void *opaque_pair)
{
    delete AsPair(opaque_pair);
    dualboy_melonds_platform::ClearConfiguration();
}

} // namespace

namespace dualboy_melonds_platform {

void Configure(dualboy_log_fn log,
               void *log_context,
               const char *system_directory)
{
    std::lock_guard<std::mutex> lock(g_platform_config_mutex);
    g_platform_log = log;
    g_platform_log_context = log_context;
    g_platform_system_directory =
        system_directory != nullptr && system_directory[0] != '\0'
            ? system_directory
            : ".";
}

void ClearConfiguration()
{
    std::lock_guard<std::mutex> lock(g_platform_config_mutex);
    g_platform_log = nullptr;
    g_platform_log_context = nullptr;
    g_platform_system_directory = ".";
}

std::string SystemDirectory()
{
    std::lock_guard<std::mutex> lock(g_platform_config_mutex);
    return g_platform_system_directory;
}

void Log(melonDS::Platform::LogLevel level, const char *message)
{
    /* melonDS debug diagnostics include hot-path unknown-I/O reports that can
     * generate millions of lines per second for retail software. Forwarding
     * those through RetroArch/Steam can block both emulation workers long
     * enough to trip the pair's frame deadline. The Libretro log callback has
     * no per-message indication that verbose logging is enabled, so keep
     * operator-relevant info/warn/error messages and suppress engine debug. */
    if (level == melonDS::Platform::Debug) return;

    dualboy_log_fn callback = nullptr;
    void *context = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_platform_config_mutex);
        callback = g_platform_log;
        context = g_platform_log_context;
    }
    if (callback == nullptr) return;
    enum dualboy_log_level mapped = DUALBOY_LOG_INFO;
    switch (level) {
        case melonDS::Platform::Debug:
            mapped = DUALBOY_LOG_DEBUG;
            break;
        case melonDS::Platform::Info:
            mapped = DUALBOY_LOG_INFO;
            break;
        case melonDS::Platform::Warn:
            mapped = DUALBOY_LOG_WARN;
            break;
        case melonDS::Platform::Error:
            mapped = DUALBOY_LOG_ERROR;
            break;
    }
    callback(context, mapped, message != nullptr ? message : "");
}

void SignalStop(melonDS::Platform::StopReason reason, void *userdata)
{
    MachineContext *context = ContextFromUserdata(userdata);
    if (context != nullptr) {
        context->stop_reason.store(static_cast<int>(reason),
                                   std::memory_order_release);
        context->pair->MPEnd(*context);
    }
}

void WriteNDSSave(const std::uint8_t *data,
                  std::uint32_t size,
                  std::uint32_t offset,
                  std::uint32_t length,
                  void *userdata)
{
    MachineContext *context = ContextFromUserdata(userdata);
    if (context == nullptr || data == nullptr ||
        context->save_shadow.size() != size || size == 0U || offset >= size) {
        return;
    }
    /* CartRetail::SPIRelease masks its positive byte count by size - 1.
     * Because every supported extent is a power of two, zero therefore means
     * that a whole-device transaction completed, not that nothing changed. */
    const std::uint32_t committed_length = length == 0U ? size : length;
    const std::uint32_t tail_length =
        std::min<std::uint32_t>(committed_length, size - offset);
    if (tail_length != 0U) {
        std::uint8_t *const destination =
            context->save_shadow.data() + offset;
        const std::uint8_t *const source = data + offset;
        if (destination != source) {
            std::memmove(destination, source, tail_length);
        }
    }
    /* Match upstream SaveManager::RequestFlush: cartridge writes can wrap at
     * the physical end of SaveRAM, so one callback may commit a tail and a
     * prefix. The live buffer already contains both final ranges. */
    const std::uint32_t wrapped_length =
        std::min<std::uint32_t>(committed_length - tail_length, size);
    if (wrapped_length != 0U && context->save_shadow.data() != data) {
        std::memmove(context->save_shadow.data(), data, wrapped_length);
    }
    context->save_dirty.store(true, std::memory_order_release);
}

void WriteGBASave(const std::uint8_t *,
                  std::uint32_t,
                  std::uint32_t,
                  std::uint32_t,
                  void *)
{
}

void WriteFirmware(const melonDS::Firmware &,
                   std::uint32_t,
                   std::uint32_t,
                   void *userdata)
{
    MachineContext *context = ContextFromUserdata(userdata);
    if (context != nullptr) {
        context->firmware_dirty.store(true, std::memory_order_release);
    }
}

void WriteDateTime(int, int, int, int, int, int, void *)
{
}

void MPBegin(void *userdata)
{
    MachineContext *context = ContextFromUserdata(userdata);
    if (context != nullptr) context->pair->MPBegin(*context);
}

void MPEnd(void *userdata)
{
    MachineContext *context = ContextFromUserdata(userdata);
    if (context != nullptr) context->pair->MPEnd(*context);
}

int MPSendPacket(std::uint8_t *data,
                 int length,
                 std::uint64_t timestamp,
                 void *userdata)
{
    MachineContext *context = ContextFromUserdata(userdata);
    if (context == nullptr ||
        !context->mp_registered.load(std::memory_order_acquire)) return 0;
    return context->pair->local_mp.SendPacket(
        static_cast<int>(context->id), data, length, timestamp);
}

int MPRecvPacket(std::uint8_t *data,
                 std::uint64_t *timestamp,
                 void *userdata)
{
    MachineContext *context = ContextFromUserdata(userdata);
    if (context == nullptr ||
        !context->mp_registered.load(std::memory_order_acquire)) return 0;
    return context->pair->local_mp.RecvPacket(
        static_cast<int>(context->id), data, timestamp);
}

int MPSendCmd(std::uint8_t *data,
              int length,
              std::uint64_t timestamp,
              void *userdata)
{
    MachineContext *context = ContextFromUserdata(userdata);
    if (context == nullptr ||
        !context->mp_registered.load(std::memory_order_acquire)) return 0;
    return context->pair->local_mp.SendCmd(
        static_cast<int>(context->id), data, length, timestamp);
}

int MPSendReply(std::uint8_t *data,
                int length,
                std::uint64_t timestamp,
                std::uint16_t aid,
                void *userdata)
{
    MachineContext *context = ContextFromUserdata(userdata);
    if (context == nullptr ||
        !context->mp_registered.load(std::memory_order_acquire)) return 0;
    return context->pair->local_mp.SendReply(
        static_cast<int>(context->id), data, length, timestamp, aid);
}

int MPSendAck(std::uint8_t *data,
              int length,
              std::uint64_t timestamp,
              void *userdata)
{
    MachineContext *context = ContextFromUserdata(userdata);
    if (context == nullptr ||
        !context->mp_registered.load(std::memory_order_acquire)) return 0;
    return context->pair->local_mp.SendAck(
        static_cast<int>(context->id), data, length, timestamp);
}

int MPRecvHostPacket(std::uint8_t *data,
                     std::uint64_t *timestamp,
                     void *userdata)
{
    MachineContext *context = ContextFromUserdata(userdata);
    if (context == nullptr ||
        !context->mp_registered.load(std::memory_order_acquire)) return 0;
    return context->pair->local_mp.RecvHostPacket(
        static_cast<int>(context->id), data, timestamp);
}

std::uint16_t MPRecvReplies(std::uint8_t *data,
                            std::uint64_t timestamp,
                            std::uint16_t aidmask,
                            void *userdata)
{
    MachineContext *context = ContextFromUserdata(userdata);
    if (context == nullptr ||
        !context->mp_registered.load(std::memory_order_acquire)) return 0U;
    return context->pair->local_mp.RecvReplies(
        static_cast<int>(context->id), data, timestamp, aidmask);
}

} // namespace dualboy_melonds_platform

extern "C" const struct dualboy_engine_ops *dualboy_melonds_engine(void)
{
    static const struct dualboy_engine_ops operations = []() {
        struct dualboy_engine_ops value{};
        value.name = "melonDS";
        value.family = DUALBOY_ENGINE_MELONDS;
        value.create_pair = CreatePair;
        value.load_rom = LoadRom;
        value.set_link = SetLink;
        value.reset = Reset;
        value.set_input = SetLegacyInput;
        value.set_machine_input = SetMachineInput;
        value.run_frame = RunFrame;
        value.video_frame = VideoFrame;
        value.audio_sample_rate = AudioSampleRate;
        value.frame_rate = FrameRate;
        value.read_audio = ReadAudio;
        value.memory_info = MemoryInfo;
        value.persistent_memory_extent = PersistentMemoryExtent;
        value.validate_persistent_memory = ValidatePersistentMemory;
        value.memory_dirty = MemoryDirty;
        value.clear_memory_dirty = ClearMemoryDirty;
        /* melonDS serializes Wi-Fi hardware but not LocalMP queues.  Leave the
         * paired state surface disabled until transport-reset semantics can be
         * made transactional. */
        value.machine_state_size = nullptr;
        value.serialize_machine = nullptr;
        value.unserialize_machine = nullptr;
        value.link_state_size = nullptr;
        value.serialize_link = nullptr;
        value.unserialize_link = nullptr;
        value.link_transport_active = LinkTransportActive;
        value.destroy_pair = DestroyPair;
        value.persistent_memory_loaded = PersistentMemoryLoaded;
        return value;
    }();
    return &operations;
}

extern "C" unsigned dualboy_melonds_debug_live_instances(const void *opaque_pair)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr) return 0U;
    unsigned count = 0U;
    for (const MachineContext &machine : pair->machines) {
        if (machine.nds != nullptr) ++count;
    }
    return count;
}

extern "C" const void *dualboy_melonds_debug_userdata(const void *opaque_pair,
                                                        unsigned machine)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    return pair != nullptr && machine < kMachineCount
               ? static_cast<const void *>(&pair->machines[machine])
               : nullptr;
}

extern "C" const void *dualboy_melonds_debug_nds_object(const void *opaque_pair,
                                                         unsigned machine)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    return pair != nullptr && machine < kMachineCount
               ? static_cast<const void *>(pair->machines[machine].nds.get())
               : nullptr;
}

extern "C" std::uint8_t
dualboy_melonds_debug_registration_mask(const void *opaque_pair)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr) return 0U;
    std::uint8_t mask = 0U;
    for (unsigned machine = 0U; machine < kMachineCount; ++machine) {
        if (pair->machines[machine].mp_registered.load(
                std::memory_order_acquire)) {
            mask = static_cast<std::uint8_t>(mask | (1U << machine));
        }
    }
    return mask;
}

extern "C" std::uint64_t dualboy_melonds_debug_completed_frames(
    const void *opaque_pair, unsigned machine)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    return pair != nullptr && machine < kMachineCount
               ? pair->machines[machine].completed_frames.load(
                     std::memory_order_acquire)
               : 0U;
}

extern "C" bool dualboy_melonds_debug_last_touch(const void *opaque_pair,
                                                  unsigned machine,
                                                  bool *active,
                                                  std::uint16_t *x,
                                                  std::uint16_t *y)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount || active == nullptr ||
        x == nullptr || y == nullptr) return false;
    *active = pair->machines[machine].last_touch_active;
    *x = pair->machines[machine].last_touch_x;
    *y = pair->machines[machine].last_touch_y;
    return true;
}

extern "C" bool dualboy_melonds_debug_tsc_touch(void *opaque_pair,
                                                 unsigned machine,
                                                 std::uint16_t *x,
                                                 std::uint16_t *y)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount || x == nullptr ||
        y == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(pair->frame_mutex);
    MachineContext &context = pair->machines[machine];
    if (pair->frame_state != FrameState::Idle || context.nds == nullptr) {
        return false;
    }
    melonDS::TSC *tsc = context.nds->SPI.GetTSC();
    if (tsc == nullptr) return false;

    const auto read_axis = [tsc](std::uint8_t command) {
        tsc->Write(command);
        tsc->Write(0U);
        const std::uint8_t high = tsc->Read();
        tsc->Write(0U);
        const std::uint8_t low = tsc->Read();
        tsc->Release();
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(high) << 5U) |
            (static_cast<std::uint16_t>(low) >> 3U));
    };
    *x = static_cast<std::uint16_t>(read_axis(UINT8_C(0xd0)) >> 4U);
    *y = static_cast<std::uint16_t>(read_axis(UINT8_C(0x90)) >> 4U);
    return true;
}

extern "C" bool dualboy_melonds_debug_last_buttons(const void *opaque_pair,
                                                    unsigned machine,
                                                    std::uint16_t *buttons)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount || buttons == nullptr) {
        return false;
    }
    *buttons = pair->machines[machine].input.buttons;
    return true;
}

extern "C" bool dualboy_melonds_debug_firmware_mac(const void *opaque_pair,
                                                    unsigned machine,
                                                    std::uint8_t mac[6])
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount || mac == nullptr ||
        pair->machines[machine].nds == nullptr) return false;
    const auto &source =
        pair->machines[machine].nds->GetFirmware().GetHeader().MacAddr;
    std::copy(source.begin(), source.end(), mac);
    return true;
}

extern "C" bool dualboy_melonds_debug_set_firmware_mac(
    void *opaque_pair, unsigned machine, const std::uint8_t mac[6])
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount || mac == nullptr ||
        pair->machines[machine].nds == nullptr) return false;
    auto &firmware = pair->machines[machine].nds->GetFirmware();
    std::copy_n(mac, 6U, firmware.GetHeader().MacAddr.begin());
    firmware.UpdateChecksums();
    pair->machines[machine].firmware_dirty.store(true,
                                                 std::memory_order_release);
    return true;
}

extern "C" bool dualboy_melonds_debug_stop_machine(void *opaque_pair,
                                                    unsigned machine)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount ||
        pair->machines[machine].nds == nullptr) {
        return false;
    }
    pair->machines[machine].nds->Stop(melonDS::Platform::StopReason::External);
    return true;
}

extern "C" bool dualboy_melonds_debug_write_cart_save(
    void *opaque_pair,
    unsigned machine,
    std::uint32_t address,
    std::uint8_t value)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount) return false;
    std::lock_guard<std::mutex> lock(pair->frame_mutex);
    MachineContext &context = pair->machines[machine];
    if (pair->frame_state != FrameState::Idle || context.nds == nullptr) {
        return false;
    }

    melonDS::NDSCart::CartCommon *cart = context.nds->NDSCartSlot.GetCart();
    const std::uint32_t save_length = context.nds->GetNDSSaveLength();
    if (cart == nullptr || save_length != UINT32_C(8192) ||
        address >= save_length) {
        return false;
    }

    /* Perform the same write-enable/program/release transaction that an NDS
     * 8-KiB EEPROM transfer reaches for the generated test cartridge.
     * CartRetail::SPIRelease invokes the real Platform::WriteNDSSave callback,
     * which updates the adapter shadow. */
    cart->SPISelect();
    (void)cart->SPITransmitReceive(UINT8_C(0x06));
    cart->SPIRelease();
    cart->SPISelect();
    (void)cart->SPITransmitReceive(UINT8_C(0x02));
    (void)cart->SPITransmitReceive(static_cast<std::uint8_t>(address >> 8U));
    (void)cart->SPITransmitReceive(static_cast<std::uint8_t>(address));
    (void)cart->SPITransmitReceive(value);
    cart->SPIRelease();

    const std::uint8_t *save = context.nds->GetNDSSave();
    return save != nullptr && save[address] == value &&
           context.save_shadow.size() == save_length &&
           context.save_shadow[address] == value &&
           context.save_dirty.load(std::memory_order_acquire);
}

extern "C" bool dualboy_melonds_debug_set_frame_deadline_ms(
    void *opaque_pair, std::uint32_t milliseconds)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || milliseconds == 0U) return false;
    std::lock_guard<std::mutex> lock(pair->frame_mutex);
    if (pair->frame_state != FrameState::Idle) return false;
    pair->frame_deadline = std::chrono::milliseconds(milliseconds);
    return true;
}

extern "C" bool dualboy_melonds_debug_hold_next_frame(void *opaque_pair,
                                                       unsigned machine)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount) return false;
    std::lock_guard<std::mutex> lock(pair->frame_mutex);
    if (pair->frame_state != FrameState::Idle ||
        pair->debug_hold_next_frame[machine] ||
        pair->debug_frame_held[machine]) {
        return false;
    }
    pair->debug_hold_next_frame[machine] = true;
    return true;
}

extern "C" bool dualboy_melonds_debug_wait_for_frame_timeout(
    void *opaque_pair, std::uint32_t timeout_ms)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || timeout_ms == 0U) return false;
    std::unique_lock<std::mutex> lock(pair->frame_mutex);
    return pair->state_cv.wait_for(
        lock, std::chrono::milliseconds(timeout_ms), [pair]() {
            return pair->frame_state == FrameState::DeadlineExceeded ||
                   pair->frame_state == FrameState::Poisoned;
        });
}

extern "C" bool dualboy_melonds_debug_release_frame_hold(void *opaque_pair,
                                                          unsigned machine)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount) return false;
    bool released = false;
    {
        std::lock_guard<std::mutex> lock(pair->frame_mutex);
        released = pair->debug_hold_next_frame[machine] ||
                   pair->debug_frame_held[machine];
        pair->debug_hold_next_frame[machine] = false;
        pair->debug_frame_held[machine] = false;
    }
    pair->state_cv.notify_all();
    return released;
}

extern "C" bool dualboy_melonds_debug_is_frame_poisoned(
    const void *opaque_pair)
{
    const MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr) return false;
    std::lock_guard<std::mutex> lock(pair->frame_mutex);
    return pair->frame_state == FrameState::Poisoned;
}

extern "C" bool dualboy_melonds_debug_set_mp_requested(void *opaque_pair,
                                                        unsigned machine,
                                                        bool requested)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || machine >= kMachineCount) return false;
    if (requested) {
        pair->MPBegin(pair->machines[machine]);
    } else {
        pair->MPEnd(pair->machines[machine]);
    }
    return true;
}

extern "C" bool dualboy_melonds_debug_mp_round_trip(
    void *opaque_pair,
    std::uint64_t timestamp,
    const std::uint8_t *sent,
    std::size_t length,
    std::uint8_t *received,
    std::uint64_t *received_timestamp)
{
    MelonDSPair *pair = AsPair(opaque_pair);
    if (pair == nullptr || sent == nullptr || received == nullptr ||
        received_timestamp == nullptr || length == 0U ||
        length > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !pair->TransportActive()) {
        return false;
    }
    std::vector<std::uint8_t> packet(sent, sent + length);
    const int sent_length = pair->local_mp.SendPacket(
        0, packet.data(), static_cast<int>(packet.size()), timestamp);
    if (sent_length != static_cast<int>(packet.size())) return false;
    const int received_length =
        pair->local_mp.RecvPacket(1, received, received_timestamp);
    return received_length == sent_length;
}
