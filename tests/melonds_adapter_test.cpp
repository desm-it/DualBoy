/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "engines/melonds/nds_adapter_test.h"
#include "frontend/engine.h"

extern "C" {
#include "frontend/content.h"
#include "frontend/save_manager.h"
#include "frontend/session.h"
}

#include <NDS.h>

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

namespace {

constexpr std::size_t kRomSize = 0x10000U;
constexpr std::size_t kArm9Offset = 0x8000U;
constexpr std::size_t kArm7Offset = 0x8004U;
constexpr std::size_t kFirmwareSize = 128U * 1024U;

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
    CHECK(dualboy_melonds_debug_set_frame_deadline_ms(owner.pair, 25U));
    CHECK(dualboy_melonds_debug_hold_next_frame(owner.pair, 0U));

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
    const bool released =
        dualboy_melonds_debug_release_frame_hold(owner.pair, 0U);
    runner.join();

    CHECK(observed_timeout);
    CHECK(!returned_at_deadline);
    CHECK(released);
    CHECK(frame_returned.load(std::memory_order_acquire));
    CHECK(!frame_result);
    CHECK(std::strstr(frame_error, "timed out") != nullptr);
    CHECK(dualboy_melonds_debug_is_frame_poisoned(owner.pair));
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
    CHECK(std::strstr(second_error, "unavailable after a frame timeout") !=
          nullptr);

    const auto destroy_started = std::chrono::steady_clock::now();
    owner.operations->destroy_pair(owner.pair);
    owner.pair = nullptr;
    const auto destroy_elapsed = std::chrono::steady_clock::now() -
                                 destroy_started;
    CHECK(destroy_elapsed < std::chrono::seconds(2));
    return true;
}

bool RemoveIfPresent(const std::string &path)
{
    return path.empty() || ::unlink(path.c_str()) == 0 || access(path.c_str(), F_OK) != 0;
}

bool TestPersistenceAcrossDestruction(
    const std::array<std::uint8_t, kRomSize> &rom)
{
    char directory_template[] = "/tmp/dualboy-melonds-XXXXXX";
    char *directory = ::mkdtemp(directory_template);
    CHECK(directory != nullptr);
    const dualboy_engine_config config{nullptr, nullptr, directory, 48000U};
    const dualboy_rom content[2] = {
        {DUALBOY_PLATFORM_NDS, rom.data(), rom.size(), "persistent.nds"},
        {DUALBOY_PLATFORM_NDS, rom.data(), rom.size(), "persistent.nds"},
    };
    dualboy_save_paths saved_paths{};
    std::uint8_t expected_firmware[2]{};
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
            CHECK(save_size == 8192U && firmware_size == kFirmwareSize);
            CHECK(dualboy_melonds_debug_write_cart_save(
                session.pair, machine, 17U,
                static_cast<std::uint8_t>(0x31U + machine)));
            CHECK(static_cast<std::uint8_t *>(save)[17U] ==
                  static_cast<std::uint8_t>(0x31U + machine));
            CHECK(session.engine->memory_dirty(session.pair, machine));
            expected_firmware[machine] = static_cast<std::uint8_t>(
                static_cast<std::uint8_t *>(firmware)[0x1000U] ^
                static_cast<std::uint8_t>(0x41U + machine));
            static_cast<std::uint8_t *>(firmware)[0x1000U] =
                expected_firmware[machine];
            CHECK(dualboy_melonds_debug_set_firmware_mac(
                session.pair, machine, expected_mac[machine]));
        }
        CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                         sizeof(error)));
        dualboy_save_manager_deinit(&manager);
        dualboy_session_unload(&session);
    }

    CHECK(std::strcmp(saved_paths.sram[0], saved_paths.sram[1]) != 0);
    CHECK(std::strcmp(saved_paths.firmware[0], saved_paths.firmware[1]) != 0);
    CHECK(std::strstr(saved_paths.sram[1], ".srm.2") != nullptr);
    CHECK(std::strstr(saved_paths.firmware[1], ".firmware.bin.2") != nullptr);

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
            CHECK(static_cast<std::uint8_t *>(save)[17U] ==
                  static_cast<std::uint8_t>(0x31U + machine));
            const auto *nds = static_cast<const melonDS::NDS *>(
                dualboy_melonds_debug_nds_object(session.pair, machine));
            CHECK(nds != nullptr && nds->GetNDSSave() != nullptr);
            CHECK(nds->GetNDSSave()[17U] ==
                  static_cast<std::uint8_t>(0x31U + machine));
            CHECK(static_cast<std::uint8_t *>(firmware)[0x1000U] ==
                  expected_firmware[machine]);
            std::uint8_t firmware_mac[6]{};
            CHECK(dualboy_melonds_debug_firmware_mac(
                session.pair, machine, firmware_mac));
            CHECK(std::memcmp(firmware_mac, expected_mac[machine],
                              sizeof(firmware_mac)) == 0);
        }
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
    if (first_detection.platform != DUALBOY_PLATFORM_NDS ||
        second_detection.platform != DUALBOY_PLATFORM_NDS ||
        !TestInstancesFramesInputAndTransport(first, second) ||
        !TestPartialAndRepeatedCleanup(first) ||
        !TestGeneratedFirmwareValidation(first, second) ||
        !TestWorkerDeadlineQuiescesBeforeReturning(first, second) ||
        !TestPersistenceAcrossDestruction(first) ||
        !TestDuplicatePersistedFirmwareMacRepair(first, second)) {
        return EXIT_FAILURE;
    }
    std::puts("melonDS adapter tests passed");
    return EXIT_SUCCESS;
}
