/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "engines/melonds/nds_adapter_test.h"
#include "engines/melonds/nds_platform_bridge.hpp"
#include "frontend/engine.h"

extern "C" {
#include "frontend/content.h"
#include "frontend/save_manager.h"
#include "frontend/session.h"
}

#include <NDS.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#if defined(__linux__)
#include <dlfcn.h>
#endif

namespace {

constexpr std::size_t kRomSize = 0x10000U;
constexpr std::size_t kArm9Offset = 0x8000U;
constexpr std::size_t kArm7Offset = 0x8004U;
constexpr std::size_t kFirmwareSize = 128U * 1024U;

#if defined(__linux__)
using EglBoolean = unsigned int;
using EglEnum = unsigned int;
using EglInt = int;
using EglDisplay = void *;
using EglGetPlatformDisplay = EglDisplay (*)(EglEnum, void *,
                                             const std::intptr_t *);
using EglInitialize = EglBoolean (*)(EglDisplay, EglInt *, EglInt *);
using EglQueryString = const char *(*)(EglDisplay, EglInt);
using EglTerminate = EglBoolean (*)(EglDisplay);

constexpr EglEnum kEglPlatformSurfacelessMesa = 0x31ddU;
constexpr EglInt kEglVersion = 0x3054;

template <typename Function>
bool LoadEglSymbol(void *library, const char *name, Function &function)
{
    void *symbol;

    static_assert(sizeof(function) == sizeof(symbol));
    (void)dlerror();
    symbol = dlsym(library, name);
    if (symbol == nullptr) return false;
    std::memcpy(&function, &symbol, sizeof(function));
    return true;
}

class SurfacelessEglSentinel {
public:
    SurfacelessEglSentinel() = default;
    SurfacelessEglSentinel(const SurfacelessEglSentinel &) = delete;
    SurfacelessEglSentinel &operator=(const SurfacelessEglSentinel &) = delete;

    ~SurfacelessEglSentinel()
    {
        if (display_ != nullptr && terminate_ != nullptr) {
            (void)terminate_(display_);
        }
        if (library_ != nullptr) (void)dlclose(library_);
    }

    bool Initialize()
    {
        EglInt major = 0;
        EglInt minor = 0;

        library_ = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (library_ == nullptr ||
            !LoadEglSymbol(library_, "eglGetPlatformDisplay",
                           get_platform_display_) ||
            !LoadEglSymbol(library_, "eglInitialize", initialize_) ||
            !LoadEglSymbol(library_, "eglQueryString", query_string_) ||
            !LoadEglSymbol(library_, "eglTerminate", terminate_)) {
            return false;
        }
        display_ = get_platform_display_(kEglPlatformSurfacelessMesa, nullptr,
                                         nullptr);
        return display_ != nullptr && initialize_(display_, &major, &minor) != 0U;
    }

    bool IsInitialized() const
    {
        return display_ != nullptr && query_string_ != nullptr &&
               query_string_(display_, kEglVersion) != nullptr;
    }

private:
    void *library_ = nullptr;
    EglDisplay display_ = nullptr;
    EglGetPlatformDisplay get_platform_display_ = nullptr;
    EglInitialize initialize_ = nullptr;
    EglQueryString query_string_ = nullptr;
    EglTerminate terminate_ = nullptr;
};
#endif

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,    \
                         __LINE__, #expression);                                \
            return false;                                                       \
        }                                                                       \
    } while (false)

/* The fixed cartridge-header logo required by Nintendo DS hardware. The two
 * executable payloads below are generated entirely from original ARM branch
 * instructions. No commercial program, BIOS, or firmware data is used. */
constexpr std::array<std::uint8_t, 156U> kNintendoLogo{{
    0x24U, 0xFFU, 0xAEU, 0x51U, 0x69U, 0x9AU, 0xA2U, 0x21U,
    0x3DU, 0x84U, 0x82U, 0x0AU, 0x84U, 0xE4U, 0x09U, 0xADU,
    0x11U, 0x24U, 0x8BU, 0x98U, 0xC0U, 0x81U, 0x7FU, 0x21U,
    0xA3U, 0x52U, 0xBEU, 0x19U, 0x93U, 0x09U, 0xCEU, 0x20U,
    0x10U, 0x46U, 0x4AU, 0x4AU, 0xF8U, 0x27U, 0x31U, 0xECU,
    0x58U, 0xC7U, 0xE8U, 0x33U, 0x82U, 0xE3U, 0xCEU, 0xBFU,
    0x85U, 0xF4U, 0xDFU, 0x94U, 0xCEU, 0x4BU, 0x09U, 0xC1U,
    0x94U, 0x56U, 0x8AU, 0xC0U, 0x13U, 0x72U, 0xA7U, 0xFCU,
    0x9FU, 0x84U, 0x4DU, 0x73U, 0xA3U, 0xCAU, 0x9AU, 0x61U,
    0x58U, 0x97U, 0xA3U, 0x27U, 0xFCU, 0x03U, 0x98U, 0x76U,
    0x23U, 0x1DU, 0xC7U, 0x61U, 0x03U, 0x04U, 0xAEU, 0x56U,
    0xBFU, 0x38U, 0x84U, 0x00U, 0x40U, 0xA7U, 0x0EU, 0xFDU,
    0xFFU, 0x52U, 0xFEU, 0x03U, 0x6FU, 0x95U, 0x30U, 0xF1U,
    0x97U, 0xFBU, 0xC0U, 0x85U, 0x60U, 0xD6U, 0x80U, 0x25U,
    0xA9U, 0x63U, 0xBEU, 0x03U, 0x01U, 0x4EU, 0x38U, 0xE2U,
    0xF9U, 0xA2U, 0x34U, 0xFFU, 0xBBU, 0x3EU, 0x03U, 0x44U,
    0x78U, 0x00U, 0x90U, 0xCBU, 0x88U, 0x11U, 0x3AU, 0x94U,
    0x65U, 0xC0U, 0x7CU, 0x63U, 0x87U, 0xF0U, 0x3CU, 0xAFU,
    0xD6U, 0x25U, 0xE4U, 0x8BU, 0x38U, 0x0AU, 0xACU, 0x72U,
    0x21U, 0xD4U, 0xF8U, 0x07U,
}};

void PutU16(std::uint8_t *destination, std::uint16_t value)
{
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
}

void PutU32(std::uint8_t *destination, std::uint32_t value)
{
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8U);
    destination[2] = static_cast<std::uint8_t>(value >> 16U);
    destination[3] = static_cast<std::uint8_t>(value >> 24U);
}

std::uint16_t HeaderCrc(const std::uint8_t *data, std::size_t size)
{
    std::uint16_t crc = UINT16_C(0xffff);
    for (std::size_t index = 0U; index < size; ++index) {
        crc = static_cast<std::uint16_t>(crc ^ data[index]);
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U
                      ? static_cast<std::uint16_t>((crc >> 1U) ^
                                                   UINT16_C(0xa001))
                      : static_cast<std::uint16_t>(crc >> 1U);
        }
    }
    return crc;
}

void MakeTestRom(std::array<std::uint8_t, kRomSize> &rom, char variant)
{
    rom.fill(0U);
    std::memcpy(rom.data(), "DUALBOY NDS ", 12U);
    /* Deliberately unregistered retail-style code: melonDS applies its
     * documented 64-Kibit (8-KiB) fallback save type, keeping persistence
     * testable. */
    std::memcpy(rom.data() + 0x0cU, "ZZZE", 4U);
    rom[0x0fU] = static_cast<std::uint8_t>(variant);
    std::memcpy(rom.data() + 0x10U, "DB", 2U);
    PutU32(rom.data() + 0x20U, static_cast<std::uint32_t>(kArm9Offset));
    PutU32(rom.data() + 0x24U, UINT32_C(0x02000000));
    PutU32(rom.data() + 0x28U, UINT32_C(0x02000000));
    PutU32(rom.data() + 0x2cU, UINT32_C(4));
    PutU32(rom.data() + 0x30U, static_cast<std::uint32_t>(kArm7Offset));
    PutU32(rom.data() + 0x34U, UINT32_C(0x03800000));
    PutU32(rom.data() + 0x38U, UINT32_C(0x03800000));
    PutU32(rom.data() + 0x3cU, UINT32_C(4));
    PutU32(rom.data() + 0x80U, static_cast<std::uint32_t>(rom.size()));
    PutU32(rom.data() + 0x84U, UINT32_C(0x4000));
    std::copy(kNintendoLogo.begin(), kNintendoLogo.end(),
              rom.begin() + 0xc0);
    PutU16(rom.data() + 0x15cU,
           HeaderCrc(rom.data() + 0xc0U, kNintendoLogo.size()));
    PutU16(rom.data() + 0x15eU, HeaderCrc(rom.data(), 0x15eU));
    PutU32(rom.data() + kArm9Offset, UINT32_C(0xeafffffe));
    PutU32(rom.data() + kArm7Offset, UINT32_C(0xeafffffe));
}

struct PairOwner {
    const dualboy_engine_ops *operations = nullptr;
    void *pair = nullptr;

    ~PairOwner()
    {
        if (operations != nullptr && pair != nullptr) {
            operations->destroy_pair(pair);
        }
    }
};

struct LogCapture {
    unsigned count = 0U;
    dualboy_log_level level = DUALBOY_LOG_DEBUG;
    std::string message;
};

void CaptureLog(void *opaque,
                dualboy_log_level level,
                const char *message)
{
    auto *capture = static_cast<LogCapture *>(opaque);
    if (capture == nullptr) return;
    ++capture->count;
    capture->level = level;
    capture->message = message != nullptr ? message : "";
}

struct PlatformConfigurationGuard {
    PlatformConfigurationGuard(dualboy_log_fn log, void *context)
    {
        dualboy_melonds_platform::Configure(log, context, ".");
    }

    PlatformConfigurationGuard(const PlatformConfigurationGuard &) = delete;
    PlatformConfigurationGuard &operator=(const PlatformConfigurationGuard &) = delete;

    ~PlatformConfigurationGuard()
    {
        dualboy_melonds_platform::ClearConfiguration();
    }
};

bool TestMelonDSDebugLogsAreSuppressed()
{
    LogCapture capture;
    const PlatformConfigurationGuard configuration(CaptureLog, &capture);
    melonDS::Platform::Log(melonDS::Platform::Debug,
                          "high-volume diagnostic %u", 7U);
    CHECK(capture.count == 0U);
    dualboy_melonds_platform::Log(melonDS::Platform::Debug,
                                  "bridge diagnostic");
    CHECK(capture.count == 0U);

    melonDS::Platform::Log(melonDS::Platform::Info,
                          "operator-relevant engine message %u", 7U);
    CHECK(capture.count == 1U);
    CHECK(capture.level == DUALBOY_LOG_INFO);
    CHECK(capture.message == "operator-relevant engine message 7");

    dualboy_melonds_platform::Log(melonDS::Platform::Warn,
                                 "operator warning");
    CHECK(capture.count == 2U);
    CHECK(capture.level == DUALBOY_LOG_WARN);
    dualboy_melonds_platform::Log(melonDS::Platform::Error,
                                 "operator error");
    CHECK(capture.count == 3U);
    CHECK(capture.level == DUALBOY_LOG_ERROR);
    return true;
}

bool LoadPair(PairOwner &owner,
              const std::array<std::uint8_t, kRomSize> &first,
              const std::array<std::uint8_t, kRomSize> &second)
{
    const dualboy_engine_config config{nullptr, nullptr, ".", 48000U};
    const dualboy_rom roms[2] = {
        {DUALBOY_PLATFORM_NDS, first.data(), first.size(), "first.nds"},
        {DUALBOY_PLATFORM_NDS, second.data(), second.size(), "second.nds"},
    };
    char error[512]{};

    owner.operations = dualboy_melonds_engine();
    CHECK(owner.operations != nullptr);
    CHECK(owner.operations->family == DUALBOY_ENGINE_MELONDS);
    CHECK(owner.operations->create_pair(&owner.pair, &config, error,
                                        sizeof(error)));
    CHECK(owner.pair != nullptr);
    CHECK(owner.operations->load_rom(owner.pair, 0U, &roms[0], error,
                                     sizeof(error)));
    CHECK(owner.operations->load_rom(owner.pair, 1U, &roms[1], error,
                                     sizeof(error)));
    return true;
}

bool TestRendererAndLocalMPExclusivity(
    const std::array<std::uint8_t, kRomSize> &first,
    const std::array<std::uint8_t, kRomSize> &second)
{
    PairOwner owner;
    char error[512]{};

    CHECK(LoadPair(owner, first, second));
    CHECK(owner.operations->set_video_renderer != nullptr);
    CHECK(owner.operations->video_renderer != nullptr);
    CHECK(owner.operations->set_link(owner.pair, true, error, sizeof(error)));
    CHECK(!owner.operations->set_video_renderer(
        owner.pair, DUALBOY_VIDEO_RENDERER_OPENGL, error, sizeof(error)));
    CHECK(owner.operations->video_renderer(owner.pair) ==
          DUALBOY_VIDEO_RENDERER_SOFTWARE);
    CHECK(owner.operations->set_link(owner.pair, false, error, sizeof(error)));

    if (std::getenv("DUALBOY_REQUIRE_OPENGL") == nullptr) return true;

    {
        PairOwner rollback_owner;
        CHECK(LoadPair(rollback_owner, first, second));
        CHECK(dualboy_melonds_debug_fail_next_gl_renderer(
            rollback_owner.pair, 1U));
        CHECK(!rollback_owner.operations->set_video_renderer(
            rollback_owner.pair, DUALBOY_VIDEO_RENDERER_OPENGL, error,
            sizeof(error)));
        CHECK(rollback_owner.operations->video_renderer(rollback_owner.pair) ==
              DUALBOY_VIDEO_RENDERER_SOFTWARE);
        CHECK(dualboy_melonds_debug_live_instances(rollback_owner.pair) == 2U);
        CHECK(rollback_owner.operations->run_frame(
            rollback_owner.pair, error, sizeof(error)));
    }

#if defined(__linux__)
    SurfacelessEglSentinel sentinel;
    CHECK(sentinel.Initialize());
    CHECK(sentinel.IsInitialized());
#endif

    CHECK(owner.operations->set_video_renderer(
        owner.pair, DUALBOY_VIDEO_RENDERER_OPENGL, error, sizeof(error)));
    /* The pinned Linux gate runs llvmpipe under x86-64 emulation, where the
     * first shader/JIT frame can legitimately exceed the production 10 s
     * soft deadline. Keep the runtime default honest and widen only this
     * deterministic accepted-path test. */
    CHECK(dualboy_melonds_debug_set_frame_deadline_ms(owner.pair, 120000U));
    CHECK(owner.operations->video_renderer(owner.pair) ==
          DUALBOY_VIDEO_RENDERER_OPENGL);
    CHECK(!owner.operations->set_link(owner.pair, true, error, sizeof(error)));
    CHECK(!owner.operations->link_transport_active(owner.pair));
    if (!owner.operations->run_frame(owner.pair, error, sizeof(error))) {
        std::fprintf(stderr, "OpenGL frame failed: %s\n", error);
        return false;
    }
    owner.operations->reset(owner.pair);
    if (!owner.operations->run_frame(owner.pair, error, sizeof(error))) {
        std::fprintf(stderr, "OpenGL frame after reset failed: %s\n", error);
        return false;
    }
    CHECK(owner.operations->set_video_renderer(
        owner.pair, DUALBOY_VIDEO_RENDERER_SOFTWARE, error, sizeof(error)));
    CHECK(owner.operations->video_renderer(owner.pair) ==
          DUALBOY_VIDEO_RENDERER_SOFTWARE);
#if defined(__linux__)
    /* EGL 1.5 returns the same display for identical platform/native/attribute
     * triples. Adapter teardown must not terminate a process-shared
     * surfaceless display that another component initialized first. */
    CHECK(sentinel.IsInitialized());
#endif
    CHECK(owner.operations->set_link(owner.pair, true, error, sizeof(error)));
    return true;
}

bool TestInstancesFramesInputAndTransport(
    const std::array<std::uint8_t, kRomSize> &first,
    const std::array<std::uint8_t, kRomSize> &second)
{
    PairOwner owner;
    void *save[2]{};
    void *firmware[2]{};
    std::size_t save_size[2]{};
    std::size_t firmware_size[2]{};
    dualboy_video_frame frames[2]{};
    std::uint8_t mac[2][6]{};
    char error[512]{};
    bool touch_active = false;
    std::uint16_t touch_x = 0U;
    std::uint16_t touch_y = 0U;
    std::uint16_t buttons = 0U;

    CHECK(LoadPair(owner, first, second));
    CHECK(dualboy_melonds_debug_live_instances(owner.pair) == 2U);
    CHECK(dualboy_melonds_debug_userdata(owner.pair, 0U) != nullptr);
    CHECK(dualboy_melonds_debug_userdata(owner.pair, 1U) != nullptr);
    CHECK(dualboy_melonds_debug_userdata(owner.pair, 0U) !=
          dualboy_melonds_debug_userdata(owner.pair, 1U));
    CHECK(dualboy_melonds_debug_nds_object(owner.pair, 0U) != nullptr);
    CHECK(dualboy_melonds_debug_nds_object(owner.pair, 1U) != nullptr);
    CHECK(dualboy_melonds_debug_nds_object(owner.pair, 0U) !=
          dualboy_melonds_debug_nds_object(owner.pair, 1U));

    for (unsigned machine = 0U; machine < 2U; ++machine) {
        CHECK(owner.operations->memory_info(owner.pair, machine,
                                            DUALBOY_MEMORY_SAVE_RAM,
                                            &save[machine],
                                            &save_size[machine]));
        CHECK(owner.operations->memory_info(owner.pair, machine,
                                            DUALBOY_MEMORY_FIRMWARE,
                                            &firmware[machine],
                                            &firmware_size[machine]));
        CHECK(save[machine] != nullptr && save_size[machine] == 8192U);
        CHECK(firmware[machine] != nullptr &&
              firmware_size[machine] == kFirmwareSize);
        CHECK(dualboy_melonds_debug_firmware_mac(owner.pair, machine,
                                                 mac[machine]));
        CHECK((mac[machine][0] & 0x01U) == 0U);
        CHECK((mac[machine][0] & 0x02U) != 0U);
    }
    CHECK(save[0] != save[1]);
    CHECK(firmware[0] != firmware[1]);
    CHECK(std::memcmp(mac[0], mac[1], sizeof(mac[0])) != 0);
    CHECK(owner.operations->machine_state_size == nullptr);
    CHECK(owner.operations->serialize_machine == nullptr);
    CHECK(owner.operations->unserialize_machine == nullptr);

    const dualboy_machine_input input[2] = {
        {static_cast<std::uint16_t>(DUALBOY_BUTTON_A |
                                    DUALBOY_BUTTON_RIGHT),
         true, 37U, 91U},
        {static_cast<std::uint16_t>(DUALBOY_BUTTON_B |
                                    DUALBOY_BUTTON_LEFT),
         true, 211U, 173U},
    };
    owner.operations->set_machine_input(owner.pair, 0U, &input[0]);
    owner.operations->set_machine_input(owner.pair, 1U, &input[1]);
    CHECK(owner.operations->run_frame(owner.pair, error, sizeof(error)));
    CHECK(dualboy_melonds_debug_completed_frames(owner.pair, 0U) == 1U);
    CHECK(dualboy_melonds_debug_completed_frames(owner.pair, 1U) == 1U);
    for (unsigned machine = 0U; machine < 2U; ++machine) {
        CHECK(dualboy_melonds_debug_last_buttons(owner.pair, machine,
                                                 &buttons));
        CHECK(buttons == input[machine].buttons);
        CHECK(dualboy_melonds_debug_last_touch(owner.pair, machine,
                                               &touch_active, &touch_x,
                                               &touch_y));
        CHECK(touch_active);
        CHECK(touch_x == input[machine].touch_x);
        CHECK(touch_y == input[machine].touch_y);
        CHECK(owner.operations->video_frame(owner.pair, machine,
                                            &frames[machine]));
        CHECK(frames[machine].pixels != nullptr);
        CHECK(frames[machine].width == 256U);
        CHECK(frames[machine].height == 384U);
        CHECK(frames[machine].pitch == 256U * sizeof(std::uint32_t));
    }
    CHECK(frames[0].pixels != frames[1].pixels);

    CHECK(owner.operations->set_link(owner.pair, true, error, sizeof(error)));
    CHECK(!owner.operations->link_transport_active(owner.pair));
    CHECK(dualboy_melonds_debug_set_mp_requested(owner.pair, 0U, true));
    CHECK(dualboy_melonds_debug_registration_mask(owner.pair) == 0x01U);
    CHECK(!owner.operations->link_transport_active(owner.pair));
    CHECK(dualboy_melonds_debug_set_mp_requested(owner.pair, 1U, true));
    CHECK(dualboy_melonds_debug_registration_mask(owner.pair) == 0x03U);
    CHECK(owner.operations->link_transport_active(owner.pair));
    CHECK(!dualboy_melonds_debug_set_mp_requested(owner.pair, 2U, true));
    CHECK(dualboy_melonds_debug_registration_mask(owner.pair) == 0x03U);

    constexpr std::array<std::uint8_t, 7U> sent{{
        0x44U, 0x75U, 0x61U, 0x6cU, 0x42U, 0x6fU, 0x79U,
    }};
    std::array<std::uint8_t, 32U> received{};
    std::uint64_t received_timestamp = 0U;
    CHECK(dualboy_melonds_debug_mp_round_trip(
        owner.pair, UINT64_C(0x123456789abcdef0), sent.data(), sent.size(),
        received.data(), &received_timestamp));
    CHECK(received_timestamp == UINT64_C(0x123456789abcdef0));
    CHECK(std::equal(sent.begin(), sent.end(), received.begin()));

    CHECK(owner.operations->set_link(owner.pair, false, error, sizeof(error)));
    CHECK(dualboy_melonds_debug_registration_mask(owner.pair) == 0U);
    CHECK(!owner.operations->link_transport_active(owner.pair));
    CHECK(owner.operations->set_link(owner.pair, true, error, sizeof(error)));
    CHECK(dualboy_melonds_debug_registration_mask(owner.pair) == 0x03U);
    CHECK(owner.operations->link_transport_active(owner.pair));
    CHECK(dualboy_melonds_debug_set_mp_requested(owner.pair, 1U, false));
    CHECK(dualboy_melonds_debug_registration_mask(owner.pair) == 0x01U);
    CHECK(!owner.operations->link_transport_active(owner.pair));
    CHECK(dualboy_melonds_debug_set_mp_requested(owner.pair, 1U, true));
    CHECK(dualboy_melonds_debug_registration_mask(owner.pair) == 0x03U);
    CHECK(owner.operations->link_transport_active(owner.pair));

    CHECK(dualboy_melonds_debug_stop_machine(owner.pair, 0U));
    CHECK(dualboy_melonds_debug_registration_mask(owner.pair) == 0x02U);
    CHECK(!owner.operations->link_transport_active(owner.pair));
    CHECK(!owner.operations->run_frame(owner.pair, error, sizeof(error)));
    CHECK(std::strstr(error, "machine 1 frame failed") != nullptr);
    owner.operations->reset(owner.pair);
    CHECK(dualboy_melonds_debug_registration_mask(owner.pair) == 0U);
    CHECK(!owner.operations->link_transport_active(owner.pair));
    CHECK(owner.operations->run_frame(owner.pair, error, sizeof(error)));
    CHECK(dualboy_melonds_debug_set_mp_requested(owner.pair, 0U, true));
    CHECK(dualboy_melonds_debug_set_mp_requested(owner.pair, 1U, true));
    CHECK(owner.operations->link_transport_active(owner.pair));
    owner.operations->reset(owner.pair);
    CHECK(dualboy_melonds_debug_registration_mask(owner.pair) == 0U);
    CHECK(!owner.operations->link_transport_active(owner.pair));
    CHECK(owner.operations->run_frame(owner.pair, error, sizeof(error)));
    return true;
}

bool TestIdleLocalMPPeerPollingDoesNotBlock(
    const std::array<std::uint8_t, kRomSize> &first,
    const std::array<std::uint8_t, kRomSize> &second)
{
    PairOwner owner;
    char error[512]{};

    CHECK(LoadPair(owner, first, second));
    CHECK(owner.operations->set_link(owner.pair, true, error, sizeof(error)));
    CHECK(dualboy_melonds_debug_set_mp_requested(owner.pair, 0U, true));
    CHECK(dualboy_melonds_debug_set_mp_requested(owner.pair, 1U, true));
    CHECK(owner.operations->link_transport_active(owner.pair));

    void *const host = const_cast<void *>(
        dualboy_melonds_debug_userdata(owner.pair, 0U));
    void *const client = const_cast<void *>(
        dualboy_melonds_debug_userdata(owner.pair, 1U));
    CHECK(host != nullptr && client != nullptr);

    std::array<std::uint8_t, 7U> sent{{
        0x44U, 0x75U, 0x61U, 0x6cU, 0x42U, 0x6fU, 0x79U,
    }};
    std::array<std::uint8_t, 32U> received{};
    std::uint64_t received_timestamp = 0U;
    constexpr std::uint64_t timestamp = UINT64_C(0x123456789abcdef0);
    CHECK(dualboy_melonds_platform::MPSendPacket(
              sent.data(), static_cast<int>(sent.size()), timestamp, host) ==
          static_cast<int>(sent.size()));
    CHECK(dualboy_melonds_platform::MPRecvHostPacket(
              received.data(), &received_timestamp, client) ==
          static_cast<int>(sent.size()));
    CHECK(received_timestamp == timestamp);
    CHECK(std::equal(sent.begin(), sent.end(), received.begin()));

    /* A LocalMP client that misses its expected host frame asks again on every
     * 8-us emulated WiFi tick. At a DualBoy frame boundary the peer cannot
     * produce another packet until the next outer frame, so repeated polls
     * must not each spend melonDS's 25-ms receive timeout waiting. */
    constexpr unsigned empty_polls = 20U;
    const auto polling_started = std::chrono::steady_clock::now();
    for (unsigned poll = 0U; poll < empty_polls; ++poll) {
        CHECK(dualboy_melonds_platform::MPRecvHostPacket(
                  received.data(), &received_timestamp, client) == 0);
    }
    const auto polling_elapsed = std::chrono::steady_clock::now() -
                                 polling_started;
    CHECK(polling_elapsed < std::chrono::milliseconds(250));

    CHECK(dualboy_melonds_debug_set_frame_deadline_ms(owner.pair, 5000U));

    /* Reproduce the asymmetric boundary behind the runtime stall: the client
     * is still inside its dispatched frame while the host has finished and
     * cannot send again until the next outer frame. Every empty poll must now
     * be nonblocking even though this receiver has not spent its wait budget. */
    const std::uint64_t host_frames_before =
        dualboy_melonds_debug_completed_frames(owner.pair, 0U);
    bool boundary_frame_result = false;
    char boundary_frame_error[512]{};
    CHECK(dualboy_melonds_debug_hold_next_frame(owner.pair, 1U));
    std::thread boundary_runner([&]() {
        boundary_frame_result = owner.operations->run_frame(
            owner.pair, boundary_frame_error, sizeof(boundary_frame_error));
    });

    const bool client_held = dualboy_melonds_debug_wait_for_frame_hold(
        owner.pair, 1U, 2000U);
    bool host_at_boundary = false;
    const auto boundary_wait_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (client_held &&
           std::chrono::steady_clock::now() < boundary_wait_deadline) {
        host_at_boundary =
            dualboy_melonds_debug_completed_frames(owner.pair, 0U) >
                host_frames_before &&
            !dualboy_melonds_debug_worker_frame_active(owner.pair, 0U);
        if (host_at_boundary) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool boundary_polls_empty = client_held && host_at_boundary;
    std::chrono::steady_clock::duration boundary_polling_elapsed{};
    if (boundary_polls_empty) {
        const auto boundary_polling_started =
            std::chrono::steady_clock::now();
        for (unsigned poll = 0U; poll < empty_polls; ++poll) {
            if (dualboy_melonds_platform::MPRecvHostPacket(
                    received.data(), &received_timestamp, client) != 0) {
                boundary_polls_empty = false;
                break;
            }
        }
        boundary_polling_elapsed = std::chrono::steady_clock::now() -
                                   boundary_polling_started;
    }
    const bool client_released =
        dualboy_melonds_debug_release_frame_hold(owner.pair, 1U);
    boundary_runner.join();

    CHECK(client_held);
    CHECK(host_at_boundary);
    CHECK(client_released);
    CHECK(boundary_polls_empty);
    CHECK(boundary_polling_elapsed < std::chrono::milliseconds(250));
    CHECK(boundary_frame_result);

    /* With both workers dispatched but paused at their deterministic boundary,
     * exactly one empty receive may retain upstream's scheduling wait. Run the
     * scenario twice: the second wait proves each outer-frame dispatch resets
     * the receiver's independent budget. */
    for (unsigned round = 0U; round < 2U; ++round) {
        bool frame_result = false;
        char frame_error[512]{};
        CHECK(dualboy_melonds_debug_hold_next_frame(owner.pair, 0U));
        CHECK(dualboy_melonds_debug_hold_next_frame(owner.pair, 1U));
        std::thread runner([&]() {
            frame_result = owner.operations->run_frame(
                owner.pair, frame_error, sizeof(frame_error));
        });

        const bool first_held = dualboy_melonds_debug_wait_for_frame_hold(
            owner.pair, 0U, 2000U);
        const bool second_held = dualboy_melonds_debug_wait_for_frame_hold(
            owner.pair, 1U, 2000U);
        bool active_polls_empty = first_held && second_held;
        std::chrono::steady_clock::duration active_polling_elapsed{};
        if (active_polls_empty) {
            const auto active_polling_started =
                std::chrono::steady_clock::now();
            for (unsigned poll = 0U; poll < empty_polls; ++poll) {
                if (dualboy_melonds_platform::MPRecvHostPacket(
                        received.data(), &received_timestamp, client) != 0) {
                    active_polls_empty = false;
                    break;
                }
            }
            active_polling_elapsed = std::chrono::steady_clock::now() -
                                     active_polling_started;
        }
        const bool first_released =
            dualboy_melonds_debug_release_frame_hold(owner.pair, 0U);
        const bool second_released =
            dualboy_melonds_debug_release_frame_hold(owner.pair, 1U);
        runner.join();

        CHECK(first_held && second_held);
        CHECK(first_released && second_released);
        CHECK(active_polls_empty);
        CHECK(active_polling_elapsed >= std::chrono::milliseconds(10));
        CHECK(active_polling_elapsed < std::chrono::milliseconds(250));
        CHECK(frame_result);
    }
    return true;
}

bool TestPartialAndRepeatedCleanup(
    const std::array<std::uint8_t, kRomSize> &rom)
{
    const dualboy_engine_ops *operations = dualboy_melonds_engine();
    const dualboy_engine_config config{nullptr, nullptr, ".", 48000U};
    const dualboy_rom content{
        DUALBOY_PLATFORM_NDS, rom.data(), rom.size(), "partial.nds",
    };
    char error[512]{};

    for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
        void *pair = nullptr;
        CHECK(operations->create_pair(&pair, &config, error, sizeof(error)));
        CHECK(pair != nullptr);
        CHECK(operations->load_rom(pair, 0U, &content, error, sizeof(error)));
        CHECK(!operations->run_frame(pair, error, sizeof(error)));
        operations->destroy_pair(pair);
    }
    return true;
}

bool TestGeneratedFirmwareValidation(
    const std::array<std::uint8_t, kRomSize> &first,
    const std::array<std::uint8_t, kRomSize> &second)
{
    PairOwner owner;
    void *firmware = nullptr;
    std::size_t firmware_size = 0U;
    char error[512]{};

    CHECK(LoadPair(owner, first, second));
    CHECK(owner.operations->validate_persistent_memory != nullptr);
    CHECK(owner.operations->memory_info(owner.pair, 0U,
                                        DUALBOY_MEMORY_FIRMWARE, &firmware,
                                        &firmware_size));
    CHECK(firmware != nullptr && firmware_size == kFirmwareSize);
    const std::vector<std::uint8_t> original(
        static_cast<const std::uint8_t *>(firmware),
        static_cast<const std::uint8_t *>(firmware) + firmware_size);
    CHECK(owner.operations->validate_persistent_memory(
        owner.pair, 0U, DUALBOY_MEMORY_FIRMWARE, original.data(),
        original.size(), error, sizeof(error)));

    auto malformed = original;
    malformed[0x20U] = 0xffU;
    malformed[0x21U] = 0xffU;
    CHECK(!owner.operations->validate_persistent_memory(
        owner.pair, 0U, DUALBOY_MEMORY_FIRMWARE, malformed.data(),
        malformed.size(), error, sizeof(error)));
    CHECK(std::strstr(error, "unsafe user-settings layout") != nullptr);

    malformed = original;
    malformed[0x08U] ^= 0x01U;
    CHECK(!owner.operations->validate_persistent_memory(
        owner.pair, 0U, DUALBOY_MEMORY_FIRMWARE, malformed.data(),
        malformed.size(), error, sizeof(error)));
    CHECK(std::strstr(error, "not a DualBoy-generated") != nullptr);

    malformed = original;
    malformed[0x1dU] = 0x57U;
    CHECK(!owner.operations->validate_persistent_memory(
        owner.pair, 0U, DUALBOY_MEMORY_FIRMWARE, malformed.data(),
        malformed.size(), error, sizeof(error)));
    CHECK(std::strstr(error, "unsupported console type") != nullptr);
    CHECK(std::memcmp(firmware, original.data(), original.size()) == 0);
    return true;
}

bool TestWorkerDeadlineQuiescesBeforeReturning(
    const std::array<std::uint8_t, kRomSize> &first,
    const std::array<std::uint8_t, kRomSize> &second)
{
    PairOwner owner;
    char frame_error[512]{};
    char second_error[512]{};
    bool frame_result = true;
    std::atomic<bool> frame_returned{false};

    CHECK(LoadPair(owner, first, second));
    CHECK(owner.operations->set_link(owner.pair, true, frame_error,
                                     sizeof(frame_error)));
    CHECK(dualboy_melonds_debug_set_mp_requested(owner.pair, 0U, true));
    CHECK(dualboy_melonds_debug_set_mp_requested(owner.pair, 1U, true));
    CHECK(dualboy_melonds_debug_registration_mask(owner.pair) == 0x03U);
    void *const host = const_cast<void *>(
        dualboy_melonds_debug_userdata(owner.pair, 0U));
    void *const client = const_cast<void *>(
        dualboy_melonds_debug_userdata(owner.pair, 1U));
    CHECK(host != nullptr && client != nullptr);

    std::array<std::uint8_t, 1U> packet{{0x5aU}};
    std::array<std::uint8_t, 1024U> received{};
    std::uint64_t received_timestamp = 0U;
    constexpr std::uint64_t packet_timestamp = 64U;
    CHECK(dualboy_melonds_platform::MPSendPacket(
              packet.data(), static_cast<int>(packet.size()), packet_timestamp,
              host) == static_cast<int>(packet.size()));
    CHECK(dualboy_melonds_platform::MPSendCmd(
              packet.data(), static_cast<int>(packet.size()), packet_timestamp,
              host) == static_cast<int>(packet.size()));
    CHECK(dualboy_melonds_platform::MPSendCmd(
              packet.data(), static_cast<int>(packet.size()), packet_timestamp,
              client) == static_cast<int>(packet.size()));
    CHECK(dualboy_melonds_platform::MPSendReply(
              packet.data(), static_cast<int>(packet.size()), packet_timestamp,
              1U, host) == static_cast<int>(packet.size()));

    CHECK(dualboy_melonds_debug_set_frame_deadline_ms(owner.pair, 25U));
    CHECK(dualboy_melonds_debug_hold_next_frame(owner.pair, 1U));

    std::thread runner([&]() {
        frame_result = owner.operations->run_frame(owner.pair, frame_error,
                                                   sizeof(frame_error));
        frame_returned.store(true, std::memory_order_release);
    });

    /* Do not use CHECK before releasing and joining: a failure must never
     * strand the deliberately held worker or destroy a joinable std::thread. */
    const bool observed_timeout =
        dualboy_melonds_debug_wait_for_frame_timeout(owner.pair, 2000U);
    const bool returned_at_deadline =
        frame_returned.load(std::memory_order_acquire);
    int send_packet_after_abort = -1;
    int receive_packet_after_abort = -1;
    int send_command_after_abort = -1;
    int send_reply_after_abort = -1;
    int send_ack_after_abort = -1;
    int receive_host_after_abort = -1;
    std::uint16_t receive_replies_after_abort = UINT16_MAX;
    std::uint8_t registration_after_end = UINT8_MAX;
    std::uint8_t registration_after_begin = UINT8_MAX;
    if (observed_timeout) {
        receive_packet_after_abort = dualboy_melonds_platform::MPRecvPacket(
            received.data(), &received_timestamp, client);
        receive_host_after_abort =
            dualboy_melonds_platform::MPRecvHostPacket(
                received.data(), &received_timestamp, client);
        receive_replies_after_abort =
            dualboy_melonds_platform::MPRecvReplies(
                received.data(), packet_timestamp, 0x0002U, client);
        send_packet_after_abort = dualboy_melonds_platform::MPSendPacket(
            packet.data(), static_cast<int>(packet.size()), packet_timestamp,
            client);
        send_command_after_abort = dualboy_melonds_platform::MPSendCmd(
            packet.data(), static_cast<int>(packet.size()), packet_timestamp,
            client);
        send_reply_after_abort = dualboy_melonds_platform::MPSendReply(
            packet.data(), static_cast<int>(packet.size()), packet_timestamp,
            1U, client);
        send_ack_after_abort = dualboy_melonds_platform::MPSendAck(
            packet.data(), static_cast<int>(packet.size()), packet_timestamp,
            client);
        dualboy_melonds_platform::MPEnd(client);
        registration_after_end =
            dualboy_melonds_debug_registration_mask(owner.pair);
        dualboy_melonds_platform::MPBegin(client);
        registration_after_begin =
            dualboy_melonds_debug_registration_mask(owner.pair);
    }
    const bool released =
        dualboy_melonds_debug_release_frame_hold(owner.pair, 1U);
    runner.join();

    CHECK(observed_timeout);
    CHECK(!returned_at_deadline);
    CHECK(released);
    CHECK(frame_returned.load(std::memory_order_acquire));
    CHECK(!frame_result);
    CHECK(receive_packet_after_abort == 0);
    CHECK(receive_host_after_abort == 0);
    CHECK(receive_replies_after_abort == 0U);
    CHECK(send_packet_after_abort == 0);
    CHECK(send_command_after_abort == 0);
    CHECK(send_reply_after_abort == 0);
    CHECK(send_ack_after_abort == 0);
    CHECK(registration_after_end == 0x01U);
    CHECK(registration_after_begin == 0x01U);
    CHECK(std::strstr(frame_error, "timed out") != nullptr);
    CHECK(dualboy_melonds_debug_is_frame_poisoned(owner.pair));
    CHECK(dualboy_melonds_debug_registration_mask(owner.pair) == 0U);
    CHECK(!owner.operations->link_transport_active(owner.pair));

    /* The false return is a quiescence boundary: engine-owned memory and audio
     * can be queried safely, while reset cannot silently reuse the poisoned
     * worker pair. */
    void *firmware = nullptr;
    std::size_t firmware_size = 0U;
    std::array<std::int16_t, 2U> audio{};
    CHECK(owner.operations->memory_info(owner.pair, 0U,
                                        DUALBOY_MEMORY_FIRMWARE, &firmware,
                                        &firmware_size));
    CHECK(firmware != nullptr && firmware_size == kFirmwareSize);
    CHECK(owner.operations->read_audio(owner.pair, audio.data(), 1U) == 0U);
    owner.operations->reset(owner.pair);
    CHECK(!owner.operations->run_frame(owner.pair, second_error,
                                       sizeof(second_error)));
    CHECK(std::strstr(second_error, "unavailable after an earlier") !=
          nullptr);

    const auto destroy_started = std::chrono::steady_clock::now();
    owner.operations->destroy_pair(owner.pair);
    owner.pair = nullptr;
    const auto destroy_elapsed = std::chrono::steady_clock::now() -
                                 destroy_started;
    CHECK(destroy_elapsed < std::chrono::seconds(2));
    return true;
}

bool BeginPendingCartSaveWrite(void *pair,
                               unsigned machine,
                               std::uint32_t address,
                               const std::uint8_t *data,
                               std::size_t size)
{
    auto *nds = const_cast<melonDS::NDS *>(
        static_cast<const melonDS::NDS *>(
            dualboy_melonds_debug_nds_object(pair, machine)));
    if (nds == nullptr || data == nullptr || size == 0U ||
        nds->GetNDSSaveLength() != 8192U || address >= 8192U ||
        size > 8192U) {
        return false;
    }
    melonDS::NDSCart::CartCommon *cart = nds->NDSCartSlot.GetCart();
    if (cart == nullptr) return false;

    cart->SPISelect();
    (void)cart->SPITransmitReceive(UINT8_C(0x06));
    cart->SPIRelease();
    cart->SPISelect();
    (void)cart->SPITransmitReceive(UINT8_C(0x02));
    (void)cart->SPITransmitReceive(
        static_cast<std::uint8_t>(address >> 8U));
    (void)cart->SPITransmitReceive(static_cast<std::uint8_t>(address));
    for (std::size_t index = 0U; index < size; ++index) {
        (void)cart->SPITransmitReceive(data[index]);
    }
    return true;
}

bool FinishPendingCartSaveWrite(void *pair, unsigned machine)
{
    auto *nds = const_cast<melonDS::NDS *>(
        static_cast<const melonDS::NDS *>(
            dualboy_melonds_debug_nds_object(pair, machine)));
    if (nds == nullptr) return false;
    melonDS::NDSCart::CartCommon *cart = nds->NDSCartSlot.GetCart();
    if (cart == nullptr) return false;
    cart->SPIRelease();
    return true;
}

bool TestPendingSaveTransactionSurvivesFrameBoundary(
    const std::array<std::uint8_t, kRomSize> &first,
    const std::array<std::uint8_t, kRomSize> &second)
{
    constexpr std::uint32_t kAddress = 0x0120U;
    constexpr std::array<std::uint8_t, 16U> kPayload{{
        0x44U, 0x42U, 0x53U, 0x56U, 0x10U, 0x32U, 0x54U, 0x76U,
        0x98U, 0xbaU, 0xdcU, 0xfeU, 0x5aU, 0xa5U, 0xc3U, 0x3cU,
    }};
    PairOwner owner;
    void *shadow[2]{};
    std::size_t shadow_size[2]{};
    char error[512]{};

    CHECK(LoadPair(owner, first, second));
    for (unsigned machine = 0U; machine < 2U; ++machine) {
        CHECK(owner.operations->memory_info(owner.pair, machine,
                                            DUALBOY_MEMORY_SAVE_RAM,
                                            &shadow[machine],
                                            &shadow_size[machine]));
        CHECK(shadow[machine] != nullptr && shadow_size[machine] == 8192U);
        CHECK(!owner.operations->memory_dirty(owner.pair, machine));
    }

    auto *nds0 = static_cast<const melonDS::NDS *>(
        dualboy_melonds_debug_nds_object(owner.pair, 0U));
    const auto *nds1 = static_cast<const melonDS::NDS *>(
        dualboy_melonds_debug_nds_object(owner.pair, 1U));
    CHECK(nds0 != nullptr && nds0->GetNDSSave() != nullptr);
    CHECK(nds1 != nullptr && nds1->GetNDSSave() != nullptr);
    CHECK(std::memcmp(nds0->GetNDSSave() + kAddress, kPayload.data(),
                      kPayload.size()) != 0);

    /* CartRetail updates its live EEPROM bytes before SPIRelease publishes the
     * completed range through Platform::WriteNDSSave. Keep chip-select active
     * so this deliberately crosses the same frame boundary as a guest write. */
    CHECK(BeginPendingCartSaveWrite(owner.pair, 0U, kAddress,
                                    kPayload.data(), kPayload.size()));
    CHECK(std::memcmp(nds0->GetNDSSave() + kAddress, kPayload.data(),
                      kPayload.size()) == 0);
    CHECK(std::memcmp(static_cast<const std::uint8_t *>(shadow[0]) + kAddress,
                      kPayload.data(), kPayload.size()) != 0);
    CHECK(!owner.operations->memory_dirty(owner.pair, 0U));

    CHECK(owner.operations->run_frame(owner.pair, error, sizeof(error)));
    CHECK(std::memcmp(nds0->GetNDSSave() + kAddress, kPayload.data(),
                      kPayload.size()) == 0);
    CHECK(std::memcmp(static_cast<const std::uint8_t *>(shadow[0]) + kAddress,
                      kPayload.data(), kPayload.size()) != 0);
    CHECK(std::memcmp(nds1->GetNDSSave() + kAddress, kPayload.data(),
                      kPayload.size()) != 0);

    CHECK(FinishPendingCartSaveWrite(owner.pair, 0U));
    CHECK(std::memcmp(static_cast<const std::uint8_t *>(shadow[0]) + kAddress,
                      kPayload.data(), kPayload.size()) == 0);
    CHECK(owner.operations->memory_dirty(owner.pair, 0U));
    CHECK(!owner.operations->memory_dirty(owner.pair, 1U));
    return true;
}

bool TestWrappedSaveCallbackUpdatesBothRanges(
    const std::array<std::uint8_t, kRomSize> &first,
    const std::array<std::uint8_t, kRomSize> &second)
{
    constexpr std::uint32_t kAddress = 8190U;
    constexpr std::array<std::uint8_t, 8U> kPayload{{
        0xd1U, 0xd2U, 0xd3U, 0xd4U, 0xd5U, 0xd6U, 0xd7U, 0xd8U,
    }};
    PairOwner owner;
    void *shadow = nullptr;
    std::size_t shadow_size = 0U;

    CHECK(LoadPair(owner, first, second));
    CHECK(owner.operations->memory_info(owner.pair, 0U,
                                        DUALBOY_MEMORY_SAVE_RAM, &shadow,
                                        &shadow_size));
    CHECK(shadow != nullptr && shadow_size == 8192U);
    CHECK(BeginPendingCartSaveWrite(owner.pair, 0U, kAddress,
                                    kPayload.data(), kPayload.size()));
    CHECK(FinishPendingCartSaveWrite(owner.pair, 0U));

    const auto *nds = static_cast<const melonDS::NDS *>(
        dualboy_melonds_debug_nds_object(owner.pair, 0U));
    const auto *shadow_bytes = static_cast<const std::uint8_t *>(shadow);
    CHECK(nds != nullptr && nds->GetNDSSave() != nullptr);
    CHECK(std::memcmp(nds->GetNDSSave() + kAddress, kPayload.data(), 2U) == 0);
    CHECK(std::memcmp(nds->GetNDSSave(), kPayload.data() + 2U, 6U) == 0);
    CHECK(std::memcmp(shadow_bytes + kAddress, kPayload.data(), 2U) == 0);
    CHECK(std::memcmp(shadow_bytes, kPayload.data() + 2U, 6U) == 0);
    CHECK(owner.operations->memory_dirty(owner.pair, 0U));
    return true;
}

bool TestMaskedFullExtentSaveCallbackUpdatesShadow(
    const std::array<std::uint8_t, kRomSize> &first,
    const std::array<std::uint8_t, kRomSize> &second)
{
    PairOwner owner;
    std::vector<std::uint8_t> payload(8192U);
    void *shadow = nullptr;
    std::size_t shadow_size = 0U;

    for (std::size_t index = 0U; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(index * 37U + 0x29U);
    }
    CHECK(LoadPair(owner, first, second));
    CHECK(owner.operations->memory_info(owner.pair, 0U,
                                        DUALBOY_MEMORY_SAVE_RAM, &shadow,
                                        &shadow_size));
    CHECK(shadow != nullptr && shadow_size == payload.size());
    /* SPIRelease masks the positive 8192-byte count to zero. The adapter must
     * still interpret the callback as one completed full-device write. */
    CHECK(BeginPendingCartSaveWrite(owner.pair, 0U, 0U, payload.data(),
                                    payload.size()));
    CHECK(FinishPendingCartSaveWrite(owner.pair, 0U));

    const auto *nds = static_cast<const melonDS::NDS *>(
        dualboy_melonds_debug_nds_object(owner.pair, 0U));
    CHECK(nds != nullptr && nds->GetNDSSave() != nullptr);
    CHECK(std::memcmp(nds->GetNDSSave(), payload.data(), payload.size()) == 0);
    CHECK(std::memcmp(shadow, payload.data(), payload.size()) == 0);
    CHECK(owner.operations->memory_dirty(owner.pair, 0U));
    owner.operations->clear_memory_dirty(owner.pair, 0U);

    /* The zero mask is meaningful only at a valid in-buffer offset. */
    dualboy_melonds_platform::WriteNDSSave(
        nds->GetNDSSave(), static_cast<std::uint32_t>(payload.size()),
        static_cast<std::uint32_t>(payload.size()), 0U,
        const_cast<void *>(dualboy_melonds_debug_userdata(owner.pair, 0U)));
    CHECK(!owner.operations->memory_dirty(owner.pair, 0U));
    return true;
}

bool RemoveIfPresent(const std::string &path)
{
    return path.empty() || ::unlink(path.c_str()) == 0 || access(path.c_str(), F_OK) != 0;
}

std::uint32_t SaveRecordHash(const std::uint8_t *data, std::size_t size)
{
    std::uint32_t hash = UINT32_C(2166136261);
    for (std::size_t index = 0U; index < size; ++index) {
        hash ^= data[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

std::array<std::uint8_t, 32U> MakeSaveRecord(std::uint32_t sequence,
                                             std::uint8_t machine_tag)
{
    std::array<std::uint8_t, 32U> record{};
    record[0] = 'D';
    record[1] = 'B';
    record[2] = 'S';
    record[3] = 'V';
    PutU32(record.data() + 4U, sequence);
    record[8] = machine_tag;
    for (std::size_t index = 9U; index < 28U; ++index) {
        record[index] = static_cast<std::uint8_t>(
            machine_tag + static_cast<std::uint8_t>(index * 13U));
    }
    PutU32(record.data() + 28U, SaveRecordHash(record.data(), 28U));
    return record;
}

bool SaveRecordValid(const std::uint8_t *record)
{
    if (record == nullptr || std::memcmp(record, "DBSV", 4U) != 0) {
        return false;
    }
    const std::uint32_t stored =
        static_cast<std::uint32_t>(record[28]) |
        (static_cast<std::uint32_t>(record[29]) << 8U) |
        (static_cast<std::uint32_t>(record[30]) << 16U) |
        (static_cast<std::uint32_t>(record[31]) << 24U);
    return stored == SaveRecordHash(record, 28U);
}

bool FileMatches(const char *path, const std::vector<std::uint8_t> &expected)
{
    std::vector<std::uint8_t> observed(expected.size());
    std::size_t loaded_size = 0U;
    return path != nullptr &&
           dualboy_read_file_filled(path, observed.data(), observed.size(),
                                    0U, &loaded_size) ==
               DUALBOY_PERSISTENCE_OK &&
           loaded_size == expected.size() && observed == expected;
}

bool FileIsMissing(const char *path)
{
    return path != nullptr && access(path, F_OK) != 0;
}

bool TestPersistenceAcrossDestruction(
    const std::array<std::uint8_t, kRomSize> &rom)
{
    constexpr std::uint32_t kFirstAddress = 0x0120U;
    constexpr std::uint32_t kSecondAddress = 0x0240U;
    constexpr std::uint32_t kPlayerTwoAddress = 0x0360U;
    char directory_template[] = "/tmp/dualboy-melonds-XXXXXX";
    char *directory = ::mkdtemp(directory_template);
    CHECK(directory != nullptr);
    const dualboy_engine_config config{nullptr, nullptr, directory, 48000U};
    const dualboy_rom content[2] = {
        {DUALBOY_PLATFORM_NDS, rom.data(), rom.size(), "persistent.nds"},
        {DUALBOY_PLATFORM_NDS, rom.data(), rom.size(), "persistent.nds"},
    };
    dualboy_save_paths saved_paths{};
    std::array<std::vector<std::uint8_t>, 2U> expected_save{{
        std::vector<std::uint8_t>(8192U, UINT8_C(0xff)),
        std::vector<std::uint8_t>(8192U, UINT8_C(0xff)),
    }};
    std::array<std::vector<std::uint8_t>, 2U> expected_firmware;
    const auto first_record = MakeSaveRecord(UINT32_C(0x10203040), 0x11U);
    const auto second_record = MakeSaveRecord(UINT32_C(0x50607080), 0x21U);
    const auto player_two_record =
        MakeSaveRecord(UINT32_C(0x90a0b0c0), 0x32U);
    const std::uint8_t expected_mac[2][6] = {
        {0x02U, 0x44U, 0x42U, 0xa5U, 0x00U, 0x00U},
        {0x02U, 0x44U, 0x42U, 0xa5U, 0x00U, 0x01U},
    };
    char error[512]{};

    {
        dualboy_session session{};
        dualboy_save_manager manager{};
        dualboy_session_init(&session);
        CHECK(dualboy_session_load(&session, dualboy_melonds_engine(), &config,
                                   content, DUALBOY_LOAD_NORMAL, false, error,
                                   sizeof(error)));
        CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                        sizeof(error)));
        saved_paths = manager.paths;
        CHECK(manager.write_owner);
        CHECK(manager.write_lock_count == 4U);
        CHECK(std::strcmp(saved_paths.sram[0], saved_paths.sram[1]) != 0);
        CHECK(std::strcmp(saved_paths.firmware[0],
                          saved_paths.firmware[1]) != 0);
        CHECK(std::strstr(saved_paths.sram[1], ".srm.2") != nullptr);
        CHECK(std::strstr(saved_paths.firmware[1], ".firmware.bin.2") !=
              nullptr);
        for (unsigned machine = 0U; machine < 2U; ++machine) {
            void *save = nullptr;
            void *firmware = nullptr;
            void *frontend_memory = reinterpret_cast<void *>(UINTPTR_MAX);
            std::size_t save_size = 0U;
            std::size_t firmware_size = 0U;
            std::size_t frontend_size = SIZE_MAX;
            CHECK(manager.core_managed[machine][DUALBOY_MEMORY_SAVE_RAM]);
            CHECK(!manager.core_managed[machine][DUALBOY_MEMORY_RTC]);
            CHECK(manager.core_managed[machine][DUALBOY_MEMORY_FIRMWARE]);
            CHECK(!dualboy_save_manager_frontend_memory(
                &manager, &session, machine, DUALBOY_MEMORY_SAVE_RAM,
                &frontend_memory, &frontend_size));
            CHECK(frontend_memory == nullptr && frontend_size == 0U);
            CHECK(session.engine->memory_info(session.pair, machine,
                                              DUALBOY_MEMORY_SAVE_RAM, &save,
                                              &save_size));
            CHECK(session.engine->memory_info(session.pair, machine,
                                              DUALBOY_MEMORY_FIRMWARE,
                                              &firmware, &firmware_size));
            CHECK(save_size == 8192U && firmware_size == kFirmwareSize);
            CHECK(std::all_of(static_cast<std::uint8_t *>(save),
                              static_cast<std::uint8_t *>(save) + save_size,
                              [](std::uint8_t value) {
                                  return value == UINT8_C(0xff);
                              }));
            static_cast<std::uint8_t *>(firmware)[0x1000U] ^=
                static_cast<std::uint8_t>(0x41U + machine);
            CHECK(dualboy_melonds_debug_set_firmware_mac(
                session.pair, machine, expected_mac[machine]));
            expected_firmware[machine].assign(
                static_cast<std::uint8_t *>(firmware),
                static_cast<std::uint8_t *>(firmware) + firmware_size);
        }

        CHECK(BeginPendingCartSaveWrite(session.pair, 0U, kFirstAddress,
                                        first_record.data(),
                                        first_record.size()));
        CHECK(session.engine->run_frame(session.pair, error, sizeof(error)));
        const auto *live = static_cast<const melonDS::NDS *>(
            dualboy_melonds_debug_nds_object(session.pair, 0U));
        CHECK(live != nullptr && live->GetNDSSave() != nullptr);
        CHECK(std::memcmp(live->GetNDSSave() + kFirstAddress,
                          first_record.data(), first_record.size()) == 0);
        CHECK(FinishPendingCartSaveWrite(session.pair, 0U));
        std::memcpy(expected_save[0].data() + kFirstAddress,
                    first_record.data(), first_record.size());

        CHECK(FileIsMissing(saved_paths.sram[0]));
        CHECK(FileIsMissing(saved_paths.sram[1]));
        for (unsigned frame = 1U;
             frame < DUALBOY_SAVE_FLUSH_INTERVAL_FRAMES; ++frame) {
            CHECK(dualboy_save_manager_tick(&manager, &session, error,
                                            sizeof(error)));
        }
        CHECK(FileIsMissing(saved_paths.sram[0]));
        CHECK(FileIsMissing(saved_paths.sram[1]));
        CHECK(dualboy_save_manager_tick(&manager, &session, error,
                                        sizeof(error)));
        CHECK(FileMatches(saved_paths.sram[0], expected_save[0]));
        CHECK(FileIsMissing(saved_paths.sram[1]));
        CHECK(FileMatches(saved_paths.firmware[0], expected_firmware[0]));
        CHECK(FileMatches(saved_paths.firmware[1], expected_firmware[1]));
        dualboy_save_manager_deinit(&manager);
        dualboy_session_unload(&session);
    }

    CHECK(FileIsMissing(saved_paths.sram[1]));
    CHECK(access((std::string(saved_paths.sram[1]) + ".dualboy.lock").c_str(),
                 F_OK) == 0);

    {
        dualboy_session session{};
        dualboy_save_manager manager{};
        dualboy_session_init(&session);
        CHECK(dualboy_session_load(&session, dualboy_melonds_engine(), &config,
                                   content, DUALBOY_LOAD_NORMAL, false, error,
                                   sizeof(error)));
        CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                        sizeof(error)));
        for (unsigned machine = 0U; machine < 2U; ++machine) {
            void *save = nullptr;
            void *firmware = nullptr;
            std::size_t save_size = 0U;
            std::size_t firmware_size = 0U;
            CHECK(session.engine->memory_info(session.pair, machine,
                                              DUALBOY_MEMORY_SAVE_RAM, &save,
                                              &save_size));
            CHECK(session.engine->memory_info(session.pair, machine,
                                              DUALBOY_MEMORY_FIRMWARE,
                                              &firmware, &firmware_size));
            CHECK(save_size == expected_save[machine].size());
            CHECK(std::memcmp(save, expected_save[machine].data(), save_size) ==
                  0);
            const auto *nds = static_cast<const melonDS::NDS *>(
                dualboy_melonds_debug_nds_object(session.pair, machine));
            CHECK(nds != nullptr && nds->GetNDSSave() != nullptr);
            CHECK(nds->GetNDSSaveLength() == save_size);
            CHECK(std::memcmp(nds->GetNDSSave(), expected_save[machine].data(),
                              save_size) == 0);
            CHECK(firmware_size == expected_firmware[machine].size());
            CHECK(std::memcmp(firmware, expected_firmware[machine].data(),
                              firmware_size) == 0);
            std::uint8_t firmware_mac[6]{};
            CHECK(dualboy_melonds_debug_firmware_mac(
                session.pair, machine, firmware_mac));
            CHECK(std::memcmp(firmware_mac, expected_mac[machine],
                              sizeof(firmware_mac)) == 0);
        }

        CHECK(SaveRecordValid(expected_save[0].data() + kFirstAddress));
        CHECK(BeginPendingCartSaveWrite(session.pair, 0U, kSecondAddress,
                                        second_record.data(),
                                        second_record.size()));
        CHECK(BeginPendingCartSaveWrite(session.pair, 1U, kPlayerTwoAddress,
                                        player_two_record.data(),
                                        player_two_record.size()));
        CHECK(session.engine->run_frame(session.pair, error, sizeof(error)));
        for (unsigned machine = 0U; machine < 2U; ++machine) {
            const auto *nds = static_cast<const melonDS::NDS *>(
                dualboy_melonds_debug_nds_object(session.pair, machine));
            const std::uint32_t address =
                machine == 0U ? kSecondAddress : kPlayerTwoAddress;
            const auto &record =
                machine == 0U ? second_record : player_two_record;
            CHECK(nds != nullptr && nds->GetNDSSave() != nullptr);
            CHECK(std::memcmp(nds->GetNDSSave() + address, record.data(),
                              record.size()) == 0);
            CHECK(FinishPendingCartSaveWrite(session.pair, machine));
            std::memcpy(expected_save[machine].data() + address,
                        record.data(), record.size());
        }
        CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                         sizeof(error)));
        CHECK(FileMatches(saved_paths.sram[0], expected_save[0]));
        CHECK(FileMatches(saved_paths.sram[1], expected_save[1]));
        dualboy_save_manager_deinit(&manager);
        dualboy_session_unload(&session);
    }

    {
        dualboy_session session{};
        dualboy_save_manager manager{};
        dualboy_session_init(&session);
        CHECK(dualboy_session_load(&session, dualboy_melonds_engine(), &config,
                                   content, DUALBOY_LOAD_NORMAL, false, error,
                                   sizeof(error)));
        CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                        sizeof(error)));
        for (unsigned machine = 0U; machine < 2U; ++machine) {
            void *save = nullptr;
            void *firmware = nullptr;
            std::size_t save_size = 0U;
            std::size_t firmware_size = 0U;
            CHECK(session.engine->memory_info(session.pair, machine,
                                              DUALBOY_MEMORY_SAVE_RAM, &save,
                                              &save_size));
            CHECK(session.engine->memory_info(session.pair, machine,
                                              DUALBOY_MEMORY_FIRMWARE,
                                              &firmware, &firmware_size));
            const auto *nds = static_cast<const melonDS::NDS *>(
                dualboy_melonds_debug_nds_object(session.pair, machine));
            CHECK(save_size == expected_save[machine].size());
            CHECK(std::memcmp(save, expected_save[machine].data(), save_size) ==
                  0);
            CHECK(nds != nullptr && nds->GetNDSSaveLength() == save_size);
            CHECK(std::memcmp(nds->GetNDSSave(), expected_save[machine].data(),
                              save_size) == 0);
            CHECK(firmware_size == expected_firmware[machine].size());
            CHECK(std::memcmp(firmware, expected_firmware[machine].data(),
                              firmware_size) == 0);
        }
        CHECK(SaveRecordValid(expected_save[0].data() + kFirstAddress));
        CHECK(SaveRecordValid(expected_save[0].data() + kSecondAddress));
        CHECK(SaveRecordValid(expected_save[1].data() + kPlayerTwoAddress));
        CHECK(!session.engine->memory_dirty(session.pair, 0U));
        CHECK(!session.engine->memory_dirty(session.pair, 1U));
        dualboy_save_manager_deinit(&manager);
        dualboy_session_unload(&session);
    }

    for (unsigned machine = 0U; machine < 2U; ++machine) {
        const std::array<std::string, 3U> paths{{
            saved_paths.sram[machine], saved_paths.firmware[machine],
            std::string(saved_paths.sram[machine]) + ".dualboy.lock",
        }};
        for (const std::string &path : paths) CHECK(RemoveIfPresent(path));
        CHECK(RemoveIfPresent(std::string(saved_paths.firmware[machine]) +
                              ".dualboy.lock"));
    }
    CHECK(::rmdir(directory) == 0);
    return true;
}

bool TestDuplicatePersistedFirmwareMacRepair(
    const std::array<std::uint8_t, kRomSize> &first,
    const std::array<std::uint8_t, kRomSize> &second)
{
    char directory_template[] = "/tmp/dualboy-melonds-mac-XXXXXX";
    char *directory = ::mkdtemp(directory_template);
    CHECK(directory != nullptr);
    const dualboy_engine_config config{nullptr, nullptr, directory, 48000U};
    const dualboy_rom content[2] = {
        {DUALBOY_PLATFORM_NDS, first.data(), first.size(), "first-mac.nds"},
        {DUALBOY_PLATFORM_NDS, second.data(), second.size(), "second-mac.nds"},
    };
    const std::uint8_t duplicate_mac[6] = {
        0x02U, 0x44U, 0x42U, 0x7aU, 0x33U, 0x70U,
    };
    const std::uint8_t repaired_mac[6] = {
        0x02U, 0x44U, 0x42U, 0x7aU, 0x33U, 0x71U,
    };
    dualboy_save_paths saved_paths{};
    char error[512]{};

    {
        dualboy_session session{};
        dualboy_save_manager manager{};
        dualboy_session_init(&session);
        CHECK(dualboy_session_load(&session, dualboy_melonds_engine(), &config,
                                   content, DUALBOY_LOAD_SUBSYSTEM, false,
                                   error, sizeof(error)));
        CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                        sizeof(error)));
        saved_paths = manager.paths;
        CHECK(!saved_paths.second_uses_collision_suffix);
        CHECK(dualboy_melonds_debug_set_firmware_mac(
            session.pair, 0U, duplicate_mac));
        CHECK(dualboy_melonds_debug_set_firmware_mac(
            session.pair, 1U, duplicate_mac));
        CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                         sizeof(error)));
        dualboy_save_manager_deinit(&manager);
        dualboy_session_unload(&session);
    }

    for (unsigned pass = 0U; pass < 2U; ++pass) {
        dualboy_session session{};
        dualboy_save_manager manager{};
        std::uint8_t observed[2][6]{};
        dualboy_session_init(&session);
        CHECK(dualboy_session_load(&session, dualboy_melonds_engine(), &config,
                                   content, DUALBOY_LOAD_SUBSYSTEM, false,
                                   error, sizeof(error)));
        CHECK(dualboy_save_manager_init(&manager, &session, directory, error,
                                        sizeof(error)));
        CHECK(dualboy_melonds_debug_firmware_mac(session.pair, 0U,
                                                 observed[0]));
        CHECK(dualboy_melonds_debug_firmware_mac(session.pair, 1U,
                                                 observed[1]));
        CHECK(std::memcmp(observed[0], duplicate_mac, sizeof(duplicate_mac)) ==
              0);
        CHECK(std::memcmp(observed[1], repaired_mac, sizeof(repaired_mac)) ==
              0);
        CHECK(std::memcmp(observed[0], observed[1], sizeof(observed[0])) != 0);
        CHECK((observed[0][0] & 0x03U) == 0x02U);
        CHECK((observed[1][0] & 0x03U) == 0x02U);
        CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                         sizeof(error)));
        dualboy_save_manager_deinit(&manager);
        dualboy_session_unload(&session);
    }

    for (unsigned machine = 0U; machine < 2U; ++machine) {
        CHECK(RemoveIfPresent(saved_paths.firmware[machine]));
        CHECK(RemoveIfPresent(std::string(saved_paths.sram[machine]) +
                              ".dualboy.lock"));
        CHECK(RemoveIfPresent(std::string(saved_paths.firmware[machine]) +
                              ".dualboy.lock"));
    }
    CHECK(::rmdir(directory) == 0);
    return true;
}

} // namespace

int main()
{
    std::array<std::uint8_t, kRomSize> first{};
    std::array<std::uint8_t, kRomSize> second{};
    MakeTestRom(first, 'E');
    MakeTestRom(second, 'F');

    const dualboy_detection first_detection =
        dualboy_detect_content(first.data(), first.size());
    const dualboy_detection second_detection =
        dualboy_detect_content(second.data(), second.size());
    if (!TestMelonDSDebugLogsAreSuppressed() ||
        first_detection.platform != DUALBOY_PLATFORM_NDS ||
        second_detection.platform != DUALBOY_PLATFORM_NDS ||
        !TestRendererAndLocalMPExclusivity(first, second) ||
        !TestInstancesFramesInputAndTransport(first, second) ||
        !TestIdleLocalMPPeerPollingDoesNotBlock(first, second) ||
        !TestPartialAndRepeatedCleanup(first) ||
        !TestGeneratedFirmwareValidation(first, second) ||
        !TestWorkerDeadlineQuiescesBeforeReturning(first, second) ||
        !TestPendingSaveTransactionSurvivesFrameBoundary(first, second) ||
        !TestWrappedSaveCallbackUpdatesBothRanges(first, second) ||
        !TestMaskedFullExtentSaveCallbackUpdatesShadow(first, second) ||
        !TestPersistenceAcrossDestruction(first) ||
        !TestDuplicatePersistedFirmwareMacRepair(first, second)) {
        return EXIT_FAILURE;
    }
    std::puts("melonDS adapter tests passed");
    return EXIT_SUCCESS;
}
