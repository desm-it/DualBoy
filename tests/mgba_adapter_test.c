/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "frontend/engine.h"
#include "frontend/save_manager.h"
#include "engines/mgba/mgba_adapter_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_ROM_SIZE 0x8000U
#define TEST_CODE_OFFSET 0xC0U
#define TEST_IO_LITERAL 0x300U
#define TEST_SIO_LITERAL 0x304U
#define TEST_SRAM_LITERAL 0x308U
#define TEST_VRAM_LITERAL 0x30CU
#define TEST_SEND_PRIMARY_LITERAL 0x310U
#define TEST_SEND_SECONDARY_LITERAL 0x314U
#define TEST_GPIO_MODE_LITERAL 0x318U
#define TEST_SIGNATURE_OFFSET 0x340U
#define TEST_FRAME_LIMIT 300U
#define TEST_AUDIO_CAPACITY 20000U
#define TEST_LINK_HEADER_SIZE 32U
#define TEST_LINK_DRIVER_FLAGS_OFFSET 4U
#define TEST_LINK_FIRST_EVENT_OFFSET 0x40U
#define TEST_LINK_EVENT_PLAYER_OFFSET 4U
#define TEST_LINK_EVENT_FLAGS_OFFSET 8U
#define TEST_LINK_EVENT_PAYLOAD_OFFSET 0x20U

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return false;                                                       \
        }                                                                       \
    } while (false)

#define PERSIST_CHECK(expression)                                               \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            goto cleanup;                                                       \
        }                                                                       \
    } while (false)

/* The fixed cartridge-header logo required by GBA hardware. The executable
 * program below is generated entirely from original ARM instructions. */
static const uint8_t test_gba_logo[156] = {
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
};

struct arm_builder {
    uint8_t *rom;
    size_t cursor;
    bool valid;
};

static void put_u32le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

static uint32_t get_u32le(const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

static size_t emit_arm(struct arm_builder *builder, uint32_t instruction)
{
    const size_t address = builder->cursor;

    if (!builder->valid || address > TEST_ROM_SIZE - sizeof(instruction)) {
        builder->valid = false;
        return address;
    }
    put_u32le(builder->rom + address, instruction);
    builder->cursor += sizeof(instruction);
    return address;
}

static void emit_literal_load(struct arm_builder *builder,
                              unsigned destination_register,
                              size_t literal_address)
{
    const size_t instruction_address = builder->cursor;
    size_t offset;

    if (destination_register > 15U ||
        literal_address < instruction_address + 8U) {
        builder->valid = false;
        return;
    }
    offset = literal_address - instruction_address - 8U;
    if (offset > 0xFFFU) {
        builder->valid = false;
        return;
    }
    (void)emit_arm(builder,
                   UINT32_C(0xE59F0000) |
                       ((uint32_t)destination_register << 12U) |
                       (uint32_t)offset);
}

static void patch_branch(uint8_t *rom,
                         size_t branch_address,
                         size_t target_address,
                         unsigned condition)
{
    const int32_t displacement =
        (int32_t)target_address - (int32_t)(branch_address + 8U);
    const int32_t words = displacement / 4;
    const uint32_t encoded = ((uint32_t)condition << 28U) |
                             UINT32_C(0x0A000000) |
                             ((uint32_t)words & UINT32_C(0x00FFFFFF));

    put_u32le(rom + branch_address, encoded);
}

static void emit_mode_burst(struct arm_builder *builder)
{
    size_t loop;
    size_t loop_branch;

    (void)emit_arm(builder, UINT32_C(0xE3A05028)); /* mov r5, #40 */
    loop = builder->cursor;
    (void)emit_arm(builder, UINT32_C(0xE3A02000)); /* mov r2, #0 */
    (void)emit_arm(builder, UINT32_C(0xE1C423B4)); /* strh r2, [r4,#0x34] */
    (void)emit_arm(builder, UINT32_C(0xE1C433B4)); /* strh r3, [r4,#0x34] */
    (void)emit_arm(builder, UINT32_C(0xE2555001)); /* subs r5, r5, #1 */
    loop_branch = emit_arm(builder, 0U);
    patch_branch(builder->rom, loop_branch, loop, 1U); /* bne mode burst */
}

/* Build a small ARM-state cartridge. It emits a startup SIO mode burst before
 * entering multiplayer mode, where the primary sends 0x1111 and the secondary
 * sends 0x2222. Each stores the received low byte in SRAM[1], and only writes
 * 0x5a to SRAM[2] after checking the full 16-bit word. SRAM[0] records the
 * hardware-assigned player ID; SRAM[3] records KEYINPUT. */
static bool make_test_rom(uint8_t *rom)
{
    struct arm_builder builder = {rom, TEST_CODE_OFFSET, true};
    size_t secondary_branch;
    size_t primary_ready_loop;
    size_t primary_ready_branch;
    size_t primary_to_clear_branch;
    size_t secondary_ready_loop;
    size_t secondary_ready_branch;
    size_t secondary_busy_loop;
    size_t secondary_busy_branch;
    size_t wait_clear_loop;
    size_t wait_clear_branch;
    size_t select_secondary_branch;
    size_t after_select_branch;
    size_t secondary_entry;
    size_t secondary_receive;
    size_t after_select;
    size_t frame_wait_vblank;
    size_t frame_wait_vblank_branch;
    size_t frame_wait_visible;
    size_t frame_wait_visible_branch;
    size_t frame_loop_branch;
    uint8_t checksum = 0U;
    size_t index;

    memset(rom, 0, TEST_ROM_SIZE);

    /* b 0x080000c0 */
    put_u32le(rom, UINT32_C(0xEA00002E));
    memcpy(rom + 0x04U, test_gba_logo, sizeof(test_gba_logo));
    memcpy(rom + 0xA0U, "DUALBOY LINK", 12U);
    memcpy(rom + 0xACU, "DBLT", 4U);
    memcpy(rom + 0xB0U, "00", 2U);
    rom[0xB2U] = 0x96U;
    rom[0xBCU] = 0U;

    /* Load IO, SIO, SRAM, mode-3 VRAM, and the GPIO-mode RCNT value. */
    emit_literal_load(&builder, 0U, TEST_IO_LITERAL);
    emit_literal_load(&builder, 4U, TEST_SIO_LITERAL);
    emit_literal_load(&builder, 1U, TEST_SRAM_LITERAL);
    emit_literal_load(&builder, 7U, TEST_VRAM_LITERAL);
    emit_literal_load(&builder, 3U, TEST_GPIO_MODE_LITERAL);
    emit_mode_burst(&builder);

    /* DISPCNT = mode 3 | BG2; RCNT = 0; SIOCNT = multiplayer mode. */
    (void)emit_arm(&builder, UINT32_C(0xE3A02003)); /* mov r2, #3 */
    (void)emit_arm(&builder, UINT32_C(0xE3822B01)); /* orr r2, #0x400 */
    (void)emit_arm(&builder, UINT32_C(0xE1C020B0)); /* strh r2, [r0] */
    (void)emit_arm(&builder, UINT32_C(0xE3A02000)); /* mov r2, #0 */
    (void)emit_arm(&builder, UINT32_C(0xE1C423B4)); /* strh r2, [r4,#0x34] */
    (void)emit_arm(&builder, UINT32_C(0xE3A02A02)); /* mov r2, #0x2000 */
    (void)emit_arm(&builder, UINT32_C(0xE1C422B8)); /* strh r2, [r4,#0x28] */

    /* Read the assigned multiplayer ID and expose it independently in SRAM. */
    (void)emit_arm(&builder, UINT32_C(0xE1D432B8)); /* ldrh r3, [r4,#0x28] */
    (void)emit_arm(&builder, UINT32_C(0xE1A03223)); /* mov r3, r3, lsr #4 */
    (void)emit_arm(&builder, UINT32_C(0xE2033003)); /* and r3, r3, #3 */
    (void)emit_arm(&builder, UINT32_C(0xE28320A0)); /* add r2, r3, #0xa0 */
    (void)emit_arm(&builder, UINT32_C(0xE5C12000)); /* strb r2, [r1] */

    /* Sample controls after the host has supplied each machine's input. */
    (void)emit_arm(&builder, UINT32_C(0xE1D463B0)); /* ldrh r6, [r4,#0x30] */
    (void)emit_arm(&builder, UINT32_C(0xE5C16003)); /* strb r6, [r1,#3] */

    /* Paint player 0 red and player 1 green in mode 3. */
    (void)emit_arm(&builder, UINT32_C(0xE3530000)); /* cmp r3, #0 */
    (void)emit_arm(&builder, UINT32_C(0x03A0601F)); /* moveq r6, #0x1f */
    (void)emit_arm(&builder, UINT32_C(0x13A06E3E)); /* movne r6, #0x3e0 */
    (void)emit_arm(&builder, UINT32_C(0xE1C760B0)); /* strh r6, [r7] */

    (void)emit_arm(&builder, UINT32_C(0xE3530000)); /* cmp r3, #0 */
    secondary_branch = emit_arm(&builder, 0U);       /* bne secondary */

    /* Player 0 advertises its word, waits for both endpoints to be ready, and
     * starts the multiplayer transfer. */
    emit_literal_load(&builder, 2U, TEST_SEND_PRIMARY_LITERAL);
    (void)emit_arm(&builder, UINT32_C(0xE1C422BA)); /* strh r2, [r4,#0x2a] */
    primary_ready_loop = builder.cursor;
    (void)emit_arm(&builder, UINT32_C(0xE1D422B8)); /* ldrh r2, [r4,#0x28] */
    (void)emit_arm(&builder, UINT32_C(0xE3120008)); /* tst r2, #8 */
    primary_ready_branch = emit_arm(&builder, 0U);  /* beq ready loop */
    (void)emit_arm(&builder, UINT32_C(0xE3A02A02)); /* mov r2, #0x2000 */
    (void)emit_arm(&builder, UINT32_C(0xE3822080)); /* orr r2, r2, #0x80 */
    (void)emit_arm(&builder, UINT32_C(0xE1C422B8)); /* strh r2, [r4,#0x28] */
    primary_to_clear_branch = emit_arm(&builder, 0U); /* b wait clear */

    /* Player 1 advertises a different word, waits for readiness, then waits for
     * the transfer-start event delivered by the lockstep coordinator. */
    secondary_entry = builder.cursor;
    emit_literal_load(&builder, 2U, TEST_SEND_SECONDARY_LITERAL);
    (void)emit_arm(&builder, UINT32_C(0xE1C422BA)); /* strh r2, [r4,#0x2a] */
    secondary_ready_loop = builder.cursor;
    (void)emit_arm(&builder, UINT32_C(0xE1D422B8)); /* ldrh r2, [r4,#0x28] */
    (void)emit_arm(&builder, UINT32_C(0xE3120008)); /* tst r2, #8 */
    secondary_ready_branch = emit_arm(&builder, 0U); /* beq ready loop */
    secondary_busy_loop = builder.cursor;
    (void)emit_arm(&builder, UINT32_C(0xE1D422B8)); /* ldrh r2, [r4,#0x28] */
    (void)emit_arm(&builder, UINT32_C(0xE3120080)); /* tst r2, #0x80 */
    secondary_busy_branch = emit_arm(&builder, 0U); /* beq busy loop */

    wait_clear_loop = builder.cursor;
    (void)emit_arm(&builder, UINT32_C(0xE1D422B8)); /* ldrh r2, [r4,#0x28] */
    (void)emit_arm(&builder, UINT32_C(0xE3120080)); /* tst r2, #0x80 */
    wait_clear_branch = emit_arm(&builder, 0U);     /* bne clear loop */

    /* Select the other player's receive register and expected full word. */
    (void)emit_arm(&builder, UINT32_C(0xE3530000)); /* cmp r3, #0 */
    select_secondary_branch = emit_arm(&builder, 0U); /* bne p1 receive */
    (void)emit_arm(&builder, UINT32_C(0xE1D452B2)); /* ldrh r5, [r4,#0x22] */
    emit_literal_load(&builder, 6U, TEST_SEND_SECONDARY_LITERAL);
    after_select_branch = emit_arm(&builder, 0U); /* b compare */
    secondary_receive = builder.cursor;
    (void)emit_arm(&builder, UINT32_C(0xE1D452B0)); /* ldrh r5, [r4,#0x20] */
    emit_literal_load(&builder, 6U, TEST_SEND_PRIMARY_LITERAL);
    after_select = builder.cursor;
    (void)emit_arm(&builder, UINT32_C(0xE5C15001)); /* strb r5, [r1,#1] */
    (void)emit_arm(&builder, UINT32_C(0xE1550006)); /* cmp r5, r6 */
    (void)emit_arm(&builder, UINT32_C(0x03A0605A)); /* moveq r6, #0x5a */
    (void)emit_arm(&builder, UINT32_C(0x13A060EE)); /* movne r6, #0xee */
    (void)emit_arm(&builder, UINT32_C(0xE5C16002)); /* strb r6, [r1,#2] */

    /* Change the first framebuffer pixel once per completed frame. This makes
     * machine-state restore observable through the public video API. */
    frame_wait_vblank = builder.cursor;
    (void)emit_arm(&builder, UINT32_C(0xE1D060B6)); /* ldrh r6, [r0,#6] */
    (void)emit_arm(&builder, UINT32_C(0xE35600A0)); /* cmp r6, #160 */
    frame_wait_vblank_branch = emit_arm(&builder, 0U); /* bne vblank */
    frame_wait_visible = builder.cursor;
    (void)emit_arm(&builder, UINT32_C(0xE1D060B6)); /* ldrh r6, [r0,#6] */
    (void)emit_arm(&builder, UINT32_C(0xE35600A0)); /* cmp r6, #160 */
    frame_wait_visible_branch = emit_arm(&builder, 0U); /* beq visible */
    (void)emit_arm(&builder, UINT32_C(0xE1D760B0)); /* ldrh r6, [r7] */
    (void)emit_arm(&builder, UINT32_C(0xE2866001)); /* add r6, r6, #1 */
    (void)emit_arm(&builder, UINT32_C(0xE1C760B0)); /* strh r6, [r7] */
    frame_loop_branch = emit_arm(&builder, 0U);     /* b vblank */

    if (!builder.valid || builder.cursor >= TEST_IO_LITERAL) {
        return false;
    }

    patch_branch(rom, secondary_branch, secondary_entry, 1U);
    patch_branch(rom, primary_ready_branch, primary_ready_loop, 0U);
    patch_branch(rom, primary_to_clear_branch, wait_clear_loop, 14U);
    patch_branch(rom, secondary_ready_branch, secondary_ready_loop, 0U);
    patch_branch(rom, secondary_busy_branch, secondary_busy_loop, 0U);
    patch_branch(rom, wait_clear_branch, wait_clear_loop, 1U);
    patch_branch(rom, select_secondary_branch, secondary_receive, 1U);
    patch_branch(rom, after_select_branch, after_select, 14U);
    patch_branch(rom, frame_wait_vblank_branch, frame_wait_vblank, 1U);
    patch_branch(rom, frame_wait_visible_branch, frame_wait_visible, 0U);
    patch_branch(rom, frame_loop_branch, frame_wait_vblank, 14U);

    put_u32le(rom + TEST_IO_LITERAL, UINT32_C(0x04000000));
    put_u32le(rom + TEST_SIO_LITERAL, UINT32_C(0x04000100));
    put_u32le(rom + TEST_SRAM_LITERAL, UINT32_C(0x0E000000));
    put_u32le(rom + TEST_VRAM_LITERAL, UINT32_C(0x06000000));
    put_u32le(rom + TEST_SEND_PRIMARY_LITERAL, UINT32_C(0x00001111));
    put_u32le(rom + TEST_SEND_SECONDARY_LITERAL, UINT32_C(0x00002222));
    put_u32le(rom + TEST_GPIO_MODE_LITERAL, UINT32_C(0x00008000));
    memcpy(rom + TEST_SIGNATURE_OFFSET, "SRAM_V110", 9U);

    for (index = 0xA0U; index <= 0xBCU; ++index) {
        checksum = (uint8_t)(checksum - rom[index]);
    }
    rom[0xBDU] = (uint8_t)(checksum - 0x19U);
    return true;
}

/* Build a legal source-generated cartridge that changes RCNT mode repeatedly
 * before yielding a frame. This reproduces commercial titles that emit a
 * startup burst of SIO mode changes immediately after the boot logo. */
static bool make_mode_burst_rom(uint8_t *rom)
{
    struct arm_builder builder = {rom, TEST_CODE_OFFSET, true};
    size_t loop;
    size_t loop_branch;
    uint8_t checksum = 0U;
    size_t index;

    memset(rom, 0, TEST_ROM_SIZE);
    put_u32le(rom, UINT32_C(0xEA00002E));
    memcpy(rom + 0x04U, test_gba_logo, sizeof(test_gba_logo));
    memcpy(rom + 0xA0U, "DUALBOY BURST", 13U);
    memcpy(rom + 0xACU, "DBMB", 4U);
    memcpy(rom + 0xB0U, "00", 2U);
    rom[0xB2U] = 0x96U;

    emit_literal_load(&builder, 4U, TEST_SIO_LITERAL);
    emit_literal_load(&builder, 3U, TEST_GPIO_MODE_LITERAL);
    emit_mode_burst(&builder);
    loop = builder.cursor;
    loop_branch = emit_arm(&builder, 0U);
    patch_branch(rom, loop_branch, loop, 14U);

    if (!builder.valid || builder.cursor >= TEST_IO_LITERAL) {
        return false;
    }
    put_u32le(rom + TEST_SIO_LITERAL, UINT32_C(0x04000100));
    put_u32le(rom + TEST_GPIO_MODE_LITERAL, UINT32_C(0x00008000));
    for (index = 0xA0U; index <= 0xBCU; ++index) {
        checksum = (uint8_t)(checksum - rom[index]);
    }
    rom[0xBDU] = (uint8_t)(checksum - 0x19U);
    return true;
}

static void set_test_rom_game_code(uint8_t *rom, const char game_code[4])
{
    uint8_t checksum = 0U;
    size_t index;

    memcpy(rom + 0xACU, game_code, 4U);
    for (index = 0xA0U; index <= 0xBCU; ++index) {
        checksum = (uint8_t)(checksum - rom[index]);
    }
    rom[0xBDU] = (uint8_t)(checksum - 0x19U);
}

static bool test_rom_header_is_valid(const uint8_t *rom, size_t size)
{
    uint8_t checksum = 0U;
    size_t index;

    if (rom == NULL || size <= 0xBDU || rom[0xB2U] != 0x96U ||
        memcmp(rom + 0x04U, test_gba_logo, sizeof(test_gba_logo)) != 0) {
        return false;
    }
    for (index = 0xA0U; index <= 0xBCU; ++index) {
        checksum = (uint8_t)(checksum - rom[index]);
    }
    return rom[0xBDU] == (uint8_t)(checksum - 0x19U);
}

static bool buffers_differ(const uint32_t *first,
                           const uint32_t *second,
                           size_t count)
{
    return memcmp(first, second, count * sizeof(*first)) != 0;
}

static uint64_t hash_pixels(const uint32_t *pixels, size_t count)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;

    for (index = 0U; index < count; ++index) {
        hash ^= pixels[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool file_size_is(const char *path, size_t expected)
{
    struct stat status;

    return stat(path, &status) == 0 && status.st_size >= 0 &&
           (uintmax_t)status.st_size == (uintmax_t)expected;
}

static bool test_startup_sio_mode_burst_is_scheduled_without_loss(void)
{
    const struct dualboy_engine_ops *operations = dualboy_mgba_engine();
    const struct dualboy_engine_config config = {.audio_sample_rate = 48000U};
    struct dualboy_rom content = {0};
    struct dualboy_mgba_lockstep_diagnostics diagnostics = {0};
    uint8_t *rom = NULL;
    void *pair = NULL;
    char error[256] = {0};
    unsigned index;
    bool success = false;

    rom = (uint8_t *)malloc(TEST_ROM_SIZE);
    PERSIST_CHECK(rom != NULL);
    PERSIST_CHECK(make_mode_burst_rom(rom));
    PERSIST_CHECK(test_rom_header_is_valid(rom, TEST_ROM_SIZE));
    content.platform = DUALBOY_PLATFORM_GBA;
    content.data = rom;
    content.size = TEST_ROM_SIZE;
    content.path = "generated-sio-mode-burst.gba";

    PERSIST_CHECK(operations->create_pair(&pair, &config, error, sizeof(error)));
    PERSIST_CHECK(operations->load_rom(pair, 0U, &content, error, sizeof(error)));
    PERSIST_CHECK(operations->load_rom(pair, 1U, &content, error, sizeof(error)));
    PERSIST_CHECK(operations->set_link(pair, true, error, sizeof(error)));
    for (index = 0U; index < 4U; ++index) {
        PERSIST_CHECK(operations->run_frame(pair, error, sizeof(error)));
    }
    PERSIST_CHECK(dualboy_mgba_get_lockstep_diagnostics(pair, &diagnostics));
    PERSIST_CHECK(diagnostics.max_queue_depth > 0U);
    PERSIST_CHECK(diagnostics.max_queue_depth <
                  DUALBOY_MGBA_LOCKSTEP_QUEUE_CAPACITY);
    PERSIST_CHECK(diagnostics.dropped_events == 0U);
    PERSIST_CHECK(diagnostics.queued_events == 0U);
    PERSIST_CHECK(diagnostics.modes_converged);
    success = true;

cleanup:
    if (pair != NULL) {
        operations->destroy_pair(pair);
    }
    free(rom);
    return success;
}

static bool run_linked_pair_case(void)
{
    const struct dualboy_engine_ops *operations = dualboy_mgba_engine();
    const struct dualboy_engine_config config = {
        .audio_sample_rate = 48000U,
    };
    struct dualboy_rom content = {0};
    struct dualboy_mgba_lockstep_diagnostics diagnostics = {0};
    struct dualboy_video_frame video[2];
    uint8_t *rom = NULL;
    void *pair = NULL;
    void *save[2] = {NULL, NULL};
    size_t save_size[2] = {0U, 0U};
    void *rtc[2] = {NULL, NULL};
    size_t rtc_size[2] = {0U, 0U};
    uint8_t *machine_state[2] = {NULL, NULL};
    uint8_t *machine_state_again = NULL;
    size_t machine_state_size[2] = {0U, 0U};
    uint8_t *link_state = NULL;
    uint8_t *link_state_again = NULL;
    size_t link_state_capacity;
    size_t link_state_used = 0U;
    size_t used = 0U;
    int16_t *audio = NULL;
    size_t audio_frames;
    size_t persistent_size = 0U;
    uint64_t replay_video_hash[2] = {0U, 0U};
    char error[256] = {0};
    unsigned index;
    bool exchange_complete = false;
    bool persistent_known = false;
    bool rtc_present[2];

    CHECK(operations != NULL);
    CHECK(operations->family == DUALBOY_ENGINE_MGBA);
    CHECK(strcmp(operations->name, "mGBA") == 0);
    CHECK(operations->persistent_memory_extent != NULL);

    rom = (uint8_t *)malloc(TEST_ROM_SIZE);
    CHECK(rom != NULL);
    CHECK(make_test_rom(rom));
    CHECK(test_rom_header_is_valid(rom, TEST_ROM_SIZE));
    content.platform = DUALBOY_PLATFORM_GBA;
    content.data = rom;
    content.size = TEST_ROM_SIZE;
    content.path = "generated-link-test.gba";

    CHECK(operations->create_pair(&pair, &config, error, sizeof(error)));
    CHECK(pair != NULL);
    CHECK(operations->load_rom(pair, 0U, &content, error, sizeof(error)));
    CHECK(operations->load_rom(pair, 1U, &content, error, sizeof(error)));
    CHECK(!operations->load_rom(pair, 0U, &content, error, sizeof(error)));
    CHECK(operations->set_link(pair, true, error, sizeof(error)));

    persistent_known = true;
    persistent_size = 1U;
    CHECK(!operations->persistent_memory_extent(
        NULL,
        0U,
        DUALBOY_MEMORY_SAVE_RAM,
        &persistent_known,
        &persistent_size));
    CHECK(!persistent_known && persistent_size == 0U);
    persistent_known = true;
    persistent_size = 1U;
    CHECK(!operations->persistent_memory_extent(
        pair,
        DUALBOY_MACHINE_COUNT,
        DUALBOY_MEMORY_SAVE_RAM,
        &persistent_known,
        &persistent_size));
    CHECK(!persistent_known && persistent_size == 0U);
    persistent_size = 1U;
    CHECK(!operations->persistent_memory_extent(
        pair,
        0U,
        DUALBOY_MEMORY_SAVE_RAM,
        NULL,
        &persistent_size));
    CHECK(persistent_size == 0U);
    persistent_known = true;
    CHECK(!operations->persistent_memory_extent(
        pair,
        0U,
        DUALBOY_MEMORY_SAVE_RAM,
        &persistent_known,
        NULL));
    CHECK(!persistent_known);
    persistent_known = true;
    persistent_size = 1U;
    CHECK(!operations->persistent_memory_extent(
        pair,
        0U,
        (enum dualboy_memory_kind)99,
        &persistent_known,
        &persistent_size));
    CHECK(!persistent_known && persistent_size == 0U);

    for (index = 0U; index < 2U; ++index) {
        CHECK(operations->memory_info(pair,
                                      index,
                                      DUALBOY_MEMORY_SAVE_RAM,
                                      &save[index],
                                      &save_size[index]));
        CHECK(save[index] != NULL);
        CHECK(save_size[index] == 0x20000U);
        CHECK(!operations->memory_dirty(pair, index));
        persistent_known = true;
        persistent_size = 1U;
        CHECK(operations->persistent_memory_extent(
            pair,
            index,
            DUALBOY_MEMORY_SAVE_RAM,
            &persistent_known,
            &persistent_size));
        CHECK(!persistent_known && persistent_size == 0U);

        CHECK(operations->memory_info(pair,
                                      index,
                                      DUALBOY_MEMORY_RTC,
                                      &rtc[index],
                                      &rtc_size[index]));
        rtc_present[index] = rtc[index] != NULL && rtc_size[index] != 0U;
        if (rtc_present[index]) {
            void *rtc_again = NULL;
            size_t rtc_size_again = 0U;

            CHECK(rtc[index] != NULL);
            CHECK(rtc_size[index] == 16U);
            CHECK(operations->memory_info(pair,
                                          index,
                                          DUALBOY_MEMORY_RTC,
                                          &rtc_again,
                                          &rtc_size_again));
            CHECK(rtc_again == rtc[index]);
            CHECK(rtc_size_again == rtc_size[index]);
        } else {
            CHECK(rtc[index] == NULL);
            CHECK(rtc_size[index] == 0U);
        }
        persistent_known = false;
        persistent_size = 1U;
        CHECK(operations->persistent_memory_extent(pair,
                                                   index,
                                                   DUALBOY_MEMORY_RTC,
                                                   &persistent_known,
                                                   &persistent_size));
        CHECK(persistent_known);
        CHECK(persistent_size == rtc_size[index]);
    }
    CHECK(save[0] != save[1]);
    CHECK(save_size[0] == save_size[1]);
    CHECK(rtc_present[0] == rtc_present[1]);
    if (rtc_present[0]) {
        CHECK(rtc[0] != rtc[1]);
        ((uint8_t *)rtc[0])[0] = 0x31U;
        ((uint8_t *)rtc[1])[0] = 0x72U;
        CHECK(((const uint8_t *)rtc[0])[0] == 0x31U);
        CHECK(((const uint8_t *)rtc[1])[0] == 0x72U);
    }

    /* Capture a state before the guest's first save write resolves mGBA's
     * AUTODETECT device. Restoring this state later must not roll the live
     * battery device back to an unknown extent. */
    for (index = 0U; index < 2U; ++index) {
        machine_state_size[index] = operations->machine_state_size(pair, index);
        CHECK(machine_state_size[index] > 32U);
        machine_state[index] = (uint8_t *)malloc(machine_state_size[index]);
        CHECK(machine_state[index] != NULL);
        CHECK(operations->serialize_machine(pair,
                                            index,
                                            machine_state[index],
                                            machine_state_size[index],
                                            &used));
        CHECK(used == machine_state_size[index]);
    }
    link_state_capacity = operations->link_state_size(pair);
    CHECK(link_state_capacity >= 1024U);
    link_state = (uint8_t *)malloc(link_state_capacity);
    CHECK(link_state != NULL);
    CHECK(operations->serialize_link(pair,
                                     link_state,
                                     link_state_capacity,
                                     &link_state_used));
    CHECK(link_state_used == link_state_capacity);

    operations->set_input(pair,
                          0U,
                          (uint16_t)(DUALBOY_BUTTON_A |
                                     DUALBOY_BUTTON_RIGHT));
    operations->set_input(pair,
                          1U,
                          (uint16_t)(DUALBOY_BUTTON_B |
                                     DUALBOY_BUTTON_LEFT));

    for (index = 0U; index < TEST_FRAME_LIMIT; ++index) {
        CHECK(operations->run_frame(pair, error, sizeof(error)));
        if (((const uint8_t *)save[0])[2] == 0x5AU &&
            ((const uint8_t *)save[1])[2] == 0x5AU) {
            exchange_complete = true;
            break;
        }
    }
    CHECK(exchange_complete);

    /* These values are written by the guest and prove player identity, distinct
     * controls, and a completed two-way multiplayer SIO exchange. */
    CHECK(((const uint8_t *)save[0])[0] == 0xA0U);
    CHECK(((const uint8_t *)save[1])[0] == 0xA1U);
    CHECK(((const uint8_t *)save[0])[1] == 0x22U);
    CHECK(((const uint8_t *)save[1])[1] == 0x11U);
    CHECK(((const uint8_t *)save[0])[3] == 0xEEU);
    CHECK(((const uint8_t *)save[1])[3] == 0xDDU);
    CHECK(dualboy_mgba_get_lockstep_diagnostics(pair, &diagnostics));
    CHECK(diagnostics.max_queue_depth > 0U);
    CHECK(diagnostics.max_queue_depth <
          DUALBOY_MGBA_LOCKSTEP_QUEUE_CAPACITY);
    CHECK(diagnostics.dropped_events == 0U);
    CHECK(diagnostics.queued_events == 0U);
    CHECK(diagnostics.modes_converged);

    for (index = 0U; index < 2U; ++index) {
        CHECK(operations->unserialize_machine(pair,
                                              index,
                                              machine_state[index],
                                              machine_state_size[index]));
    }
    CHECK(operations->unserialize_link(pair, link_state, link_state_used));
    for (index = 0U; index < 2U; ++index) {
        CHECK(operations->memory_info(pair,
                                      index,
                                      DUALBOY_MEMORY_SAVE_RAM,
                                      &save[index],
                                      &save_size[index]));
        CHECK(((const uint8_t *)save[index])[2] == 0x5AU);
        persistent_known = false;
        persistent_size = 0U;
        CHECK(operations->persistent_memory_extent(
            pair,
            index,
            DUALBOY_MEMORY_SAVE_RAM,
            &persistent_known,
            &persistent_size));
        CHECK(persistent_known && persistent_size == 0x8000U);
    }

    for (index = 0U; index < 2U; ++index) {
        void *save_again = NULL;
        size_t save_size_again = 0U;

        CHECK(operations->memory_info(pair,
                                      index,
                                      DUALBOY_MEMORY_SAVE_RAM,
                                      &save_again,
                                      &save_size_again));
        CHECK(save_again == save[index]);
        CHECK(save_size_again == save_size[index]);
        persistent_known = false;
        persistent_size = 0U;
        CHECK(operations->persistent_memory_extent(
            pair,
            index,
            DUALBOY_MEMORY_SAVE_RAM,
            &persistent_known,
            &persistent_size));
        CHECK(persistent_known);
        CHECK(persistent_size == 0x8000U);
    }
    CHECK(operations->memory_dirty(pair, 0U));
    CHECK(operations->memory_dirty(pair, 1U));
    operations->clear_memory_dirty(pair, 0U);
    operations->clear_memory_dirty(pair, 1U);
    CHECK(!operations->memory_dirty(pair, 0U));
    CHECK(!operations->memory_dirty(pair, 1U));

    CHECK(operations->video_frame(pair, 0U, &video[0]));
    CHECK(operations->video_frame(pair, 1U, &video[1]));
    CHECK(video[0].pixels != NULL && video[1].pixels != NULL);
    CHECK(video[0].pixels != video[1].pixels);
    CHECK(video[0].width == 240U && video[0].height == 160U);
    CHECK(video[1].width == 240U && video[1].height == 160U);
    CHECK(video[0].pitch == 240U * sizeof(uint32_t));
    CHECK(video[1].pitch == 240U * sizeof(uint32_t));
    CHECK(buffers_differ(video[0].pixels,
                         video[1].pixels,
                         (size_t)video[0].width * video[0].height));

    CHECK(operations->audio_sample_rate(pair) == 48000U);
    for (index = 0U; index < 30U; ++index) {
        CHECK(operations->run_frame(pair, error, sizeof(error)));
    }
    audio = (int16_t *)malloc((size_t)TEST_AUDIO_CAPACITY * 2U *
                              sizeof(*audio));
    CHECK(audio != NULL);
    audio_frames = operations->read_audio(pair, audio, TEST_AUDIO_CAPACITY);
    CHECK(audio_frames > 0U);
    CHECK(audio_frames <= 16384U);
    CHECK(operations->read_audio(pair, audio, TEST_AUDIO_CAPACITY) == 0U);

    for (index = 0U; index < 2U; ++index) {
        CHECK(operations->serialize_machine(pair,
                                            index,
                                            machine_state[index],
                                            machine_state_size[index],
                                            &used));
        CHECK(used == machine_state_size[index]);
        CHECK(!operations->serialize_machine(pair,
                                             index,
                                             machine_state[index],
                                             machine_state_size[index] - 1U,
                                             &used));
    }

    link_state_again = (uint8_t *)malloc(link_state_capacity);
    CHECK(link_state_again != NULL);
    CHECK(operations->serialize_link(pair,
                                     link_state,
                                     link_state_capacity,
                                     &link_state_used));
    CHECK(link_state_used == link_state_capacity);
    CHECK(!operations->unserialize_link(pair,
                                        link_state,
                                        link_state_used - 1U));

    /* A structurally corrupt but correctly sized engine payload must be
     * rejected before either live driver is mutated. */
    memcpy(link_state_again, link_state, link_state_used);
    put_u32le(link_state_again + TEST_LINK_HEADER_SIZE +
                  TEST_LINK_DRIVER_FLAGS_OFFSET,
              (get_u32le(link_state_again + TEST_LINK_HEADER_SIZE +
                         TEST_LINK_DRIVER_FLAGS_OFFSET) &
               ~UINT32_C(0x3F8)) |
                  ((uint32_t)(DUALBOY_MGBA_LOCKSTEP_QUEUE_CAPACITY + 1U)
                   << 3U));
    CHECK(!operations->unserialize_link(pair,
                                        link_state_again,
                                        link_state_used));
    CHECK(operations->serialize_link(pair,
                                     link_state_again,
                                     link_state_capacity,
                                     &used));
    CHECK(used == link_state_used);
    CHECK(memcmp(link_state_again, link_state, link_state_used) == 0);

    memcpy(link_state_again, link_state, link_state_used);
    put_u32le(link_state_again + TEST_LINK_HEADER_SIZE +
                  TEST_LINK_DRIVER_FLAGS_OFFSET,
              (get_u32le(link_state_again + TEST_LINK_HEADER_SIZE +
                         TEST_LINK_DRIVER_FLAGS_OFFSET) &
               ~UINT32_C(0x3F8)) |
                  (UINT32_C(1) << 3U));
    memset(link_state_again + TEST_LINK_HEADER_SIZE +
               TEST_LINK_FIRST_EVENT_OFFSET,
           0,
           0x30U);
    put_u32le(link_state_again + TEST_LINK_HEADER_SIZE +
                  TEST_LINK_FIRST_EVENT_OFFSET +
                  TEST_LINK_EVENT_FLAGS_OFFSET,
              UINT32_C(7));
    CHECK(!operations->unserialize_link(pair,
                                        link_state_again,
                                        link_state_used));
    CHECK(operations->serialize_link(pair,
                                     link_state_again,
                                     link_state_capacity,
                                     &used));
    CHECK(used == link_state_used);
    CHECK(memcmp(link_state_again, link_state, link_state_used) == 0);

    memcpy(link_state_again, link_state, link_state_used);
    put_u32le(link_state_again + TEST_LINK_HEADER_SIZE +
                  TEST_LINK_DRIVER_FLAGS_OFFSET,
              (get_u32le(link_state_again + TEST_LINK_HEADER_SIZE +
                         TEST_LINK_DRIVER_FLAGS_OFFSET) &
               ~UINT32_C(0x3F8)) |
                  (UINT32_C(1) << 3U));
    memset(link_state_again + TEST_LINK_HEADER_SIZE +
               TEST_LINK_FIRST_EVENT_OFFSET,
           0,
           0x30U);
    put_u32le(link_state_again + TEST_LINK_HEADER_SIZE +
                  TEST_LINK_FIRST_EVENT_OFFSET +
                  TEST_LINK_EVENT_PLAYER_OFFSET,
              UINT32_C(2));
    put_u32le(link_state_again + TEST_LINK_HEADER_SIZE +
                  TEST_LINK_FIRST_EVENT_OFFSET +
                  TEST_LINK_EVENT_FLAGS_OFFSET,
              UINT32_C(3));
    put_u32le(link_state_again + TEST_LINK_HEADER_SIZE +
                  TEST_LINK_FIRST_EVENT_OFFSET +
                  TEST_LINK_EVENT_PAYLOAD_OFFSET,
              UINT32_C(2));
    CHECK(!operations->unserialize_link(pair,
                                        link_state_again,
                                        link_state_used));
    CHECK(operations->serialize_link(pair,
                                     link_state_again,
                                     link_state_capacity,
                                     &used));
    CHECK(used == link_state_used);
    CHECK(memcmp(link_state_again, link_state, link_state_used) == 0);

    CHECK(operations->run_frame(pair, error, sizeof(error)));
    for (index = 0U; index < 2U; ++index) {
        CHECK(operations->video_frame(pair, index, &video[index]));
        replay_video_hash[index] =
            hash_pixels(video[index].pixels,
                        (size_t)video[index].width * video[index].height);
    }
    for (index = 0U; index < 2U; ++index) {
        CHECK(operations->unserialize_machine(pair,
                                              index,
                                              machine_state[index],
                                              machine_state_size[index]));
    }
    CHECK(operations->unserialize_link(pair, link_state, link_state_used));

    /* Re-serialization after restore must reproduce both opaque machine blobs
     * and the shared scheduler blob byte for byte. */
    machine_state_again = (uint8_t *)malloc(machine_state_size[0]);
    CHECK(machine_state_again != NULL);
    for (index = 0U; index < 2U; ++index) {
        CHECK(machine_state_size[index] == machine_state_size[0]);
        CHECK(operations->serialize_machine(pair,
                                            index,
                                            machine_state_again,
                                            machine_state_size[index],
                                            &used));
        CHECK(used == machine_state_size[index]);
        if (memcmp(machine_state_again,
                   machine_state[index],
                   machine_state_size[index]) != 0) {
            size_t byte;

            for (byte = 0U; byte < machine_state_size[index]; ++byte) {
                if (machine_state_again[byte] != machine_state[index][byte]) {
                    fprintf(stderr,
                            "machine %u state differs at 0x%zx: %02x != %02x\n",
                            index,
                            byte,
                            machine_state_again[byte],
                            machine_state[index][byte]);
                    break;
                }
            }
        }
        CHECK(memcmp(machine_state_again,
                     machine_state[index],
                     machine_state_size[index]) == 0);
    }
    CHECK(operations->serialize_link(pair,
                                     link_state_again,
                                     link_state_capacity,
                                     &used));
    CHECK(used == link_state_used);
    CHECK(memcmp(link_state_again, link_state, link_state_used) == 0);
    CHECK(operations->run_frame(pair, error, sizeof(error)));
    for (index = 0U; index < 2U; ++index) {
        CHECK(operations->video_frame(pair, index, &video[index]));
        CHECK(hash_pixels(video[index].pixels,
                          (size_t)video[index].width * video[index].height) ==
              replay_video_hash[index]);
    }

    /* Disabled-link state has the fixed header only. Running still exercises
     * independent cores; the guest intentionally waits for a missing peer. */
    CHECK(operations->set_link(pair, false, error, sizeof(error)));
    CHECK(operations->serialize_link(pair,
                                     link_state_again,
                                     link_state_capacity,
                                     &used));
    CHECK(used > 0U && used < link_state_capacity);
    CHECK(operations->run_frame(pair, error, sizeof(error)));

    free(machine_state_again);
    free(link_state_again);
    free(link_state);
    free(machine_state[1]);
    free(machine_state[0]);
    free(audio);
    operations->destroy_pair(pair);
    free(rom);
    return true;
}

static bool test_rejections_and_partial_teardown(void)
{
    const struct dualboy_engine_ops *operations = dualboy_mgba_engine();
    const struct dualboy_engine_config config = {0};
    struct dualboy_rom content = {0};
    uint8_t *rom = NULL;
    void *pair = NULL;
    char error[128] = {0};

    CHECK(operations != NULL);
    rom = (uint8_t *)malloc(TEST_ROM_SIZE);
    CHECK(rom != NULL);
    CHECK(make_test_rom(rom));
    CHECK(test_rom_header_is_valid(rom, TEST_ROM_SIZE));
    content.platform = DUALBOY_PLATFORM_GBA;
    content.data = rom;
    content.size = TEST_ROM_SIZE;
    content.path = "generated-partial.gba";

    CHECK(!operations->create_pair(NULL, &config, error, sizeof(error)));
    CHECK(operations->create_pair(&pair, &config, error, sizeof(error)));
    CHECK(!operations->set_link(pair, true, error, sizeof(error)));
    CHECK(operations->load_rom(pair, 1U, &content, error, sizeof(error)));
    CHECK(!operations->set_link(pair, true, error, sizeof(error)));
    operations->destroy_pair(pair);
    operations->destroy_pair(NULL);

    pair = NULL;
    CHECK(operations->create_pair(&pair, &config, error, sizeof(error)));
    content.platform = DUALBOY_PLATFORM_GB;
    CHECK(!operations->load_rom(pair, 0U, &content, error, sizeof(error)));
    content.platform = DUALBOY_PLATFORM_GBA;
    content.size = 16U;
    CHECK(!operations->load_rom(pair, 0U, &content, error, sizeof(error)));
    operations->destroy_pair(pair);
    free(rom);
    return true;
}

static bool test_save_type_override_extents(void)
{
    static const struct {
        char game_code[5];
        size_t save_size;
        size_t rtc_size;
    } cases[] = {
        {"AC8E", 0x2000U, 0U},
        {"V49E", 0x8000U, 0U},
        {"BR4J", 0x10000U, 16U},
        {"BPRE", 0x20000U, 0U},
        {"AI2E", 0U, 0U},
    };
    const struct dualboy_engine_ops *operations = dualboy_mgba_engine();
    const struct dualboy_engine_config config = {0};
    struct dualboy_rom content = {0};
    uint8_t *rom = NULL;
    void *pair = NULL;
    char error[128] = {0};
    bool success = false;
    size_t case_index;
    unsigned machine;

    PERSIST_CHECK(operations != NULL);
    PERSIST_CHECK(operations->persistent_memory_extent != NULL);
    rom = (uint8_t *)malloc(TEST_ROM_SIZE);
    PERSIST_CHECK(rom != NULL);
    content.platform = DUALBOY_PLATFORM_GBA;
    content.data = rom;
    content.size = TEST_ROM_SIZE;
    content.path = "generated-save-override.gba";

    for (case_index = 0U;
         case_index < sizeof(cases) / sizeof(cases[0]);
         ++case_index) {
        PERSIST_CHECK(make_test_rom(rom));
        set_test_rom_game_code(rom, cases[case_index].game_code);
        PERSIST_CHECK(test_rom_header_is_valid(rom, TEST_ROM_SIZE));
        PERSIST_CHECK(operations->create_pair(&pair,
                                              &config,
                                              error,
                                              sizeof(error)));
        PERSIST_CHECK(operations->load_rom(pair,
                                           0U,
                                           &content,
                                           error,
                                           sizeof(error)));
        PERSIST_CHECK(operations->load_rom(pair,
                                           1U,
                                           &content,
                                           error,
                                           sizeof(error)));

        /* Built-in cartridge overrides are applied by mGBA on reset, so the
         * extent is deliberately unknown immediately after ROM loading. */
        for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
            bool known = true;
            size_t size = 1U;

            PERSIST_CHECK(operations->persistent_memory_extent(
                pair,
                machine,
                DUALBOY_MEMORY_SAVE_RAM,
                &known,
                &size));
            PERSIST_CHECK(!known && size == 0U);
        }

        PERSIST_CHECK(operations->set_link(pair,
                                           false,
                                           error,
                                           sizeof(error)));
        for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
            void *memory = NULL;
            size_t capacity = 0U;
            bool known = false;
            size_t size = 0U;

            PERSIST_CHECK(operations->memory_info(pair,
                                                  machine,
                                                  DUALBOY_MEMORY_SAVE_RAM,
                                                  &memory,
                                                  &capacity));
            PERSIST_CHECK(memory != NULL && capacity == 0x20000U);
            PERSIST_CHECK(operations->persistent_memory_extent(
                pair,
                machine,
                DUALBOY_MEMORY_SAVE_RAM,
                &known,
                &size));
            PERSIST_CHECK(known && size == cases[case_index].save_size);

            known = false;
            size = 0U;
            PERSIST_CHECK(operations->persistent_memory_extent(
                pair,
                machine,
                DUALBOY_MEMORY_RTC,
                &known,
                &size));
            PERSIST_CHECK(known && size == cases[case_index].rtc_size);
        }
        operations->destroy_pair(pair);
        pair = NULL;
    }
    success = true;

cleanup:
    if (pair != NULL) {
        operations->destroy_pair(pair);
    }
    free(rom);
    return success;
}

static bool test_resolved_flash_state_into_fresh_pair(void)
{
    const struct dualboy_engine_ops *operations = dualboy_mgba_engine();
    const struct dualboy_engine_config config = {0};
    struct dualboy_rom content = {0};
    uint8_t *machine_state = NULL;
    uint8_t *rom = NULL;
    void *pair = NULL;
    void *save = NULL;
    size_t save_capacity = 0U;
    size_t machine_state_size;
    size_t used = 0U;
    size_t extent = 0U;
    char error[256] = {0};
    bool known = false;
    bool success = false;
    unsigned machine;

    PERSIST_CHECK(operations != NULL);
    rom = (uint8_t *)malloc(TEST_ROM_SIZE);
    PERSIST_CHECK(rom != NULL);
    PERSIST_CHECK(make_test_rom(rom));
    /* Redirect the guest's first save write to the Flash command address so
     * mGBA resolves AUTO to FLASH512 without a database override. */
    put_u32le(rom + TEST_SRAM_LITERAL, UINT32_C(0x0E005555));
    PERSIST_CHECK(test_rom_header_is_valid(rom, TEST_ROM_SIZE));
    content.platform = DUALBOY_PLATFORM_GBA;
    content.data = rom;
    content.size = TEST_ROM_SIZE;
    content.path = "generated-flash-state.gba";

    PERSIST_CHECK(operations->create_pair(&pair, &config, error,
                                          sizeof(error)));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(operations->load_rom(pair, machine, &content, error,
                                           sizeof(error)));
    }
    PERSIST_CHECK(operations->set_link(pair, false, error, sizeof(error)));
    PERSIST_CHECK(operations->run_frame(pair, error, sizeof(error)));
    PERSIST_CHECK(operations->persistent_memory_extent(
        pair, 0U, DUALBOY_MEMORY_SAVE_RAM, &known, &extent));
    PERSIST_CHECK(known && extent == 0x10000U);
    machine_state_size = operations->machine_state_size(pair, 0U);
    PERSIST_CHECK(machine_state_size > 32U);
    machine_state = (uint8_t *)malloc(machine_state_size);
    PERSIST_CHECK(machine_state != NULL);
    PERSIST_CHECK(operations->serialize_machine(pair, 0U, machine_state,
                                                machine_state_size, &used));
    PERSIST_CHECK(used == machine_state_size);
    operations->destroy_pair(pair);
    pair = NULL;

    PERSIST_CHECK(operations->create_pair(&pair, &config, error,
                                          sizeof(error)));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(operations->load_rom(pair, machine, &content, error,
                                           sizeof(error)));
    }
    PERSIST_CHECK(operations->set_link(pair, false, error, sizeof(error)));
    PERSIST_CHECK(operations->memory_info(pair, 0U,
                                          DUALBOY_MEMORY_SAVE_RAM,
                                          &save, &save_capacity));
    PERSIST_CHECK(save != NULL && save_capacity == 0x20000U);
    ((uint8_t *)save)[0x10000U] = 0x7BU;
    known = true;
    extent = 1U;
    PERSIST_CHECK(operations->persistent_memory_extent(
        pair, 0U, DUALBOY_MEMORY_SAVE_RAM, &known, &extent));
    PERSIST_CHECK(!known && extent == 0U);
    PERSIST_CHECK(operations->unserialize_machine(pair, 0U, machine_state,
                                                  machine_state_size));
    known = false;
    extent = 0U;
    PERSIST_CHECK(operations->persistent_memory_extent(
        pair, 0U, DUALBOY_MEMORY_SAVE_RAM, &known, &extent));
    PERSIST_CHECK(known && extent == 0x10000U);
    PERSIST_CHECK(operations->run_frame(pair, error, sizeof(error)));
    known = false;
    extent = 0U;
    PERSIST_CHECK(operations->persistent_memory_extent(
        pair, 0U, DUALBOY_MEMORY_SAVE_RAM, &known, &extent));
    PERSIST_CHECK(known && extent == 0x10000U);

    /* A later state may record a legitimate capacity expansion. The selected
     * live device grows, but the current save's preserved upper-bank bytes
     * remain authoritative rather than being replaced with 0xff. */
    PERSIST_CHECK(machine_state_size > 32U + 0x2E0U);
    PERSIST_CHECK(machine_state[32U + 0x2E0U] == 2U);
    machine_state[32U + 0x2E0U] = 3U;
    PERSIST_CHECK(operations->unserialize_machine(pair, 0U, machine_state,
                                                  machine_state_size));
    known = false;
    extent = 0U;
    PERSIST_CHECK(operations->persistent_memory_extent(
        pair, 0U, DUALBOY_MEMORY_SAVE_RAM, &known, &extent));
    PERSIST_CHECK(known && extent == 0x20000U);
    PERSIST_CHECK(operations->memory_info(pair, 0U,
                                          DUALBOY_MEMORY_SAVE_RAM,
                                          &save, &save_capacity));
    PERSIST_CHECK(save_capacity == 0x20000U);
    PERSIST_CHECK(((const uint8_t *)save)[0x10000U] == 0x7BU);
    success = true;

cleanup:
    if (pair != NULL) {
        operations->destroy_pair(pair);
    }
    free(machine_state);
    free(rom);
    return success;
}

static void wrap_live_pair(struct dualboy_session *session,
                           const struct dualboy_engine_ops *operations,
                           void *pair,
                           const struct dualboy_rom content[2])
{
    unsigned machine;

    memset(session, 0, sizeof(*session));
    session->engine = operations;
    session->pair = pair;
    session->load_kind = DUALBOY_LOAD_PLAYLIST;
    session->loaded = true;
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        session->roms[machine].rom = content[machine];
    }
}

static bool test_disk_persistence_round_trip(void)
{
    const struct dualboy_engine_ops *operations = dualboy_mgba_engine();
    const struct dualboy_engine_config config = {
        .audio_sample_rate = 48000U,
    };
    const uint8_t expected_guest[2][4] = {
        {0xA0U, 0x22U, 0x5AU, 0xEEU},
        {0xA1U, 0x11U, 0x5AU, 0xDDU},
    };
    const uint8_t expected_sentinel[2] = {0x37U, 0xD4U};
    struct dualboy_rom content[2] = {{0}};
    struct dualboy_session session = {0};
    struct dualboy_save_manager manager = {0};
    uint8_t *rom = NULL;
    void *pair = NULL;
    void *sram[2] = {NULL, NULL};
    size_t sram_size[2] = {0U, 0U};
    char cleanup_paths[4][DUALBOY_PATH_CAPACITY] = {{0}};
    char temporary_directory[] = "/tmp/dualboy-mgba-save-XXXXXX";
    char error[256] = {0};
    bool directory_created = false;
    bool guest_wrote_both = false;
    bool success = false;
    unsigned machine;
    unsigned frame;

    rom = (uint8_t *)malloc(TEST_ROM_SIZE);
    PERSIST_CHECK(rom != NULL);
    PERSIST_CHECK(make_test_rom(rom));
    PERSIST_CHECK(test_rom_header_is_valid(rom, TEST_ROM_SIZE));
    content[0].platform = DUALBOY_PLATFORM_GBA;
    content[0].data = rom;
    content[0].size = TEST_ROM_SIZE;
    content[0].path = "/virtual/left/collision.gba";
    content[1] = content[0];
    content[1].path = "/virtual/right/collision.gba";

    PERSIST_CHECK(mkdtemp(temporary_directory) != NULL);
    directory_created = true;
    PERSIST_CHECK(operations->create_pair(&pair, &config, error,
                                          sizeof(error)));
    PERSIST_CHECK(operations->load_rom(pair, 0U, &content[0], error,
                                       sizeof(error)));
    PERSIST_CHECK(operations->load_rom(pair, 1U, &content[1], error,
                                       sizeof(error)));
    PERSIST_CHECK(operations->set_link(pair, true, error, sizeof(error)));
    wrap_live_pair(&session, operations, pair, content);

    PERSIST_CHECK(dualboy_save_manager_init(&manager, &session,
                                            temporary_directory, error,
                                            sizeof(error)));
    PERSIST_CHECK(manager.paths.second_uses_collision_suffix);
    PERSIST_CHECK(strcmp(manager.paths.sram[0], manager.paths.sram[1]) != 0);
    PERSIST_CHECK(strstr(manager.paths.sram[1], ".srm.2") != NULL);
    memcpy(cleanup_paths[0], manager.paths.sram[0], DUALBOY_PATH_CAPACITY);
    memcpy(cleanup_paths[1], manager.paths.sram[1], DUALBOY_PATH_CAPACITY);
    memcpy(cleanup_paths[2], manager.paths.rtc[0], DUALBOY_PATH_CAPACITY);
    memcpy(cleanup_paths[3], manager.paths.rtc[1], DUALBOY_PATH_CAPACITY);
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(operations->memory_info(pair, machine,
                                              DUALBOY_MEMORY_SAVE_RAM,
                                              &sram[machine],
                                              &sram_size[machine]));
        PERSIST_CHECK(sram[machine] != NULL && sram_size[machine] > 0x100U);
        /* This enters mGBA with the manager-loaded buffer on first run. */
        ((uint8_t *)sram[machine])[0x100U] = expected_sentinel[machine];
    }

    operations->set_input(pair,
                          0U,
                          (uint16_t)(DUALBOY_BUTTON_A |
                                     DUALBOY_BUTTON_RIGHT));
    operations->set_input(pair,
                          1U,
                          (uint16_t)(DUALBOY_BUTTON_B |
                                     DUALBOY_BUTTON_LEFT));
    for (frame = 0U; frame < TEST_FRAME_LIMIT; ++frame) {
        PERSIST_CHECK(operations->run_frame(pair, error, sizeof(error)));
        PERSIST_CHECK(operations->memory_info(pair, 0U,
                                              DUALBOY_MEMORY_SAVE_RAM,
                                              &sram[0], &sram_size[0]));
        PERSIST_CHECK(operations->memory_info(pair, 1U,
                                              DUALBOY_MEMORY_SAVE_RAM,
                                              &sram[1], &sram_size[1]));
        if (((const uint8_t *)sram[0])[2] == 0x5AU &&
            ((const uint8_t *)sram[1])[2] == 0x5AU) {
            guest_wrote_both = true;
            break;
        }
    }
    PERSIST_CHECK(guest_wrote_both);
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(memcmp(sram[machine], expected_guest[machine], 4U) == 0);
        PERSIST_CHECK(((const uint8_t *)sram[machine])[0x100U] ==
                      expected_sentinel[machine]);
    }
    PERSIST_CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                             sizeof(error)));
    PERSIST_CHECK(file_size_is(manager.paths.sram[0], 0x8000U));
    PERSIST_CHECK(file_size_is(manager.paths.sram[1], 0x8000U));

    dualboy_save_manager_deinit(&manager);
    operations->destroy_pair(pair);
    pair = NULL;
    session.pair = NULL;
    session.loaded = false;

    PERSIST_CHECK(operations->create_pair(&pair, &config, error,
                                          sizeof(error)));
    PERSIST_CHECK(operations->load_rom(pair, 0U, &content[0], error,
                                       sizeof(error)));
    PERSIST_CHECK(operations->load_rom(pair, 1U, &content[1], error,
                                       sizeof(error)));
    PERSIST_CHECK(operations->set_link(pair, true, error, sizeof(error)));
    wrap_live_pair(&session, operations, pair, content);
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(operations->memory_info(pair, machine,
                                              DUALBOY_MEMORY_SAVE_RAM,
                                              &sram[machine],
                                              &sram_size[machine]));
        PERSIST_CHECK(sram[machine] != NULL && sram_size[machine] > 0x100U);
        memset(sram[machine], 0, 4U);
        ((uint8_t *)sram[machine])[0x100U] = 0U;
    }
    PERSIST_CHECK(dualboy_save_manager_init(&manager, &session,
                                            temporary_directory, error,
                                            sizeof(error)));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(memcmp(sram[machine], expected_guest[machine], 4U) == 0);
        PERSIST_CHECK(((const uint8_t *)sram[machine])[0x100U] ==
                      expected_sentinel[machine]);
    }

    /* A frame imports the restored shadow buffers into fresh mGBA cores. */
    PERSIST_CHECK(operations->run_frame(pair, error, sizeof(error)));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(operations->memory_info(pair, machine,
                                              DUALBOY_MEMORY_SAVE_RAM,
                                              &sram[machine],
                                              &sram_size[machine]));
        PERSIST_CHECK(((const uint8_t *)sram[machine])[0x100U] ==
                      expected_sentinel[machine]);
    }
    success = true;

cleanup:
    dualboy_save_manager_deinit(&manager);
    if (pair != NULL) {
        operations->destroy_pair(pair);
    }
    free(rom);
    if (directory_created) {
        for (machine = 0U; machine < 4U; ++machine) {
            if (cleanup_paths[machine][0] != '\0') {
                char lock_path[DUALBOY_PATH_CAPACITY];
                const int length = snprintf(lock_path, sizeof(lock_path),
                                            "%s.dualboy.lock",
                                            cleanup_paths[machine]);

                (void)unlink(cleanup_paths[machine]);
                if (length > 0 && (size_t)length < sizeof(lock_path)) {
                    (void)unlink(lock_path);
                }
            }
        }
        (void)rmdir(temporary_directory);
    }
    return success;
}

static bool test_rtc_survives_machine_state_restore(void)
{
    static const uint8_t rtc_seed_a[16] = {
        0x24U, 0x01U, 0x02U, 0x02U, 0x03U, 0x04U, 0x05U, 0x40U,
        0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x00U, 0x00U,
    };
    static const uint8_t rtc_seed_b[16] = {
        0x26U, 0x09U, 0x07U, 0x01U, 0x12U, 0x34U, 0x56U, 0x00U,
        0x88U, 0x77U, 0x66U, 0x55U, 0x44U, 0x33U, 0x00U, 0x00U,
    };
    const struct dualboy_engine_ops *operations = dualboy_mgba_engine();
    const struct dualboy_engine_config config = {.audio_sample_rate = 48000U};
    struct dualboy_rom content[2] = {{0}};
    struct dualboy_session session = {0};
    struct dualboy_save_manager manager = {0};
    uint8_t expected_rtc[16];
    uint8_t disk_rtc[16];
    uint8_t *machine_state = NULL;
    uint8_t *rom = NULL;
    void *pair = NULL;
    void *rtc = NULL;
    size_t rtc_size = 0U;
    size_t machine_state_size;
    size_t used = 0U;
    size_t disk_size = 0U;
    char cleanup_paths[4][DUALBOY_PATH_CAPACITY] = {{0}};
    char temporary_directory[] = "/tmp/dualboy-mgba-rtc-XXXXXX";
    char error[256] = {0};
    bool directory_created = false;
    bool success = false;
    unsigned machine;
    unsigned frame;

    PERSIST_CHECK(operations != NULL);
    rom = (uint8_t *)malloc(TEST_ROM_SIZE);
    PERSIST_CHECK(rom != NULL);
    PERSIST_CHECK(make_test_rom(rom));
    set_test_rom_game_code(rom, "BR4J");
    PERSIST_CHECK(test_rom_header_is_valid(rom, TEST_ROM_SIZE));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        content[machine].platform = DUALBOY_PLATFORM_GBA;
        content[machine].data = rom;
        content[machine].size = TEST_ROM_SIZE;
        content[machine].path = machine == 0U ? "rtc-a.gba" : "rtc-b.gba";
    }

    /* Produce a legitimate machine blob containing RTC record A. */
    PERSIST_CHECK(operations->create_pair(&pair, &config, error,
                                          sizeof(error)));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(operations->load_rom(pair, machine, &content[machine],
                                           error, sizeof(error)));
    }
    PERSIST_CHECK(operations->set_link(pair, false, error, sizeof(error)));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(operations->memory_info(pair, machine,
                                              DUALBOY_MEMORY_RTC,
                                              &rtc, &rtc_size));
        PERSIST_CHECK(rtc != NULL && rtc_size == sizeof(rtc_seed_a));
        memcpy(rtc, rtc_seed_a, sizeof(rtc_seed_a));
    }
    PERSIST_CHECK(operations->run_frame(pair, error, sizeof(error)));
    machine_state_size = operations->machine_state_size(pair, 0U);
    PERSIST_CHECK(machine_state_size > 32U);
    machine_state = (uint8_t *)malloc(machine_state_size);
    PERSIST_CHECK(machine_state != NULL);
    PERSIST_CHECK(operations->serialize_machine(pair, 0U, machine_state,
                                                machine_state_size, &used));
    PERSIST_CHECK(used == machine_state_size);
    operations->destroy_pair(pair);
    pair = NULL;

    /* A fresh current session owns RTC record B. Loading A must preserve B in
     * both the live mGBA GPIO and the core-managed file. */
    PERSIST_CHECK(mkdtemp(temporary_directory) != NULL);
    directory_created = true;
    PERSIST_CHECK(operations->create_pair(&pair, &config, error,
                                          sizeof(error)));
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(operations->load_rom(pair, machine, &content[machine],
                                           error, sizeof(error)));
    }
    PERSIST_CHECK(operations->set_link(pair, false, error, sizeof(error)));
    wrap_live_pair(&session, operations, pair, content);
    PERSIST_CHECK(dualboy_save_manager_init(&manager, &session,
                                            temporary_directory, error,
                                            sizeof(error)));
    memcpy(cleanup_paths[0], manager.paths.sram[0], DUALBOY_PATH_CAPACITY);
    memcpy(cleanup_paths[1], manager.paths.sram[1], DUALBOY_PATH_CAPACITY);
    memcpy(cleanup_paths[2], manager.paths.rtc[0], DUALBOY_PATH_CAPACITY);
    memcpy(cleanup_paths[3], manager.paths.rtc[1], DUALBOY_PATH_CAPACITY);
    for (machine = 0U; machine < DUALBOY_MACHINE_COUNT; ++machine) {
        PERSIST_CHECK(operations->memory_info(pair, machine,
                                              DUALBOY_MEMORY_RTC,
                                              &rtc, &rtc_size));
        PERSIST_CHECK(rtc != NULL && rtc_size == sizeof(rtc_seed_b));
        memcpy(rtc, rtc_seed_b, sizeof(rtc_seed_b));
    }
    PERSIST_CHECK(operations->run_frame(pair, error, sizeof(error)));
    PERSIST_CHECK(operations->memory_info(pair, 0U, DUALBOY_MEMORY_RTC,
                                          &rtc, &rtc_size));
    PERSIST_CHECK(rtc_size == sizeof(expected_rtc));
    memcpy(expected_rtc, rtc, sizeof(expected_rtc));

    PERSIST_CHECK(operations->unserialize_machine(pair, 0U, machine_state,
                                                  machine_state_size));
    PERSIST_CHECK(operations->memory_info(pair, 0U, DUALBOY_MEMORY_RTC,
                                          &rtc, &rtc_size));
    PERSIST_CHECK(rtc_size == sizeof(expected_rtc));
    PERSIST_CHECK(memcmp(rtc, expected_rtc, sizeof(expected_rtc)) == 0);

    for (frame = 0U; frame <= DUALBOY_SAVE_FLUSH_INTERVAL_FRAMES; ++frame) {
        PERSIST_CHECK(operations->run_frame(pair, error, sizeof(error)));
    }
    PERSIST_CHECK(operations->memory_info(pair, 0U, DUALBOY_MEMORY_RTC,
                                          &rtc, &rtc_size));
    PERSIST_CHECK(rtc_size == sizeof(expected_rtc));
    memcpy(expected_rtc, rtc, sizeof(expected_rtc));
    PERSIST_CHECK(dualboy_save_manager_flush(&manager, &session, true, error,
                                             sizeof(error)));
    PERSIST_CHECK(dualboy_read_file_filled(manager.paths.rtc[0],
                                           disk_rtc,
                                           sizeof(disk_rtc),
                                           0U,
                                           &disk_size) ==
                  DUALBOY_PERSISTENCE_OK);
    PERSIST_CHECK(disk_size == sizeof(disk_rtc));
    PERSIST_CHECK(memcmp(disk_rtc, expected_rtc, sizeof(disk_rtc)) == 0);
    success = true;

cleanup:
    dualboy_save_manager_deinit(&manager);
    if (pair != NULL) {
        operations->destroy_pair(pair);
    }
    free(machine_state);
    free(rom);
    if (directory_created) {
        for (machine = 0U; machine < 4U; ++machine) {
            if (cleanup_paths[machine][0] != '\0') {
                char lock_path[DUALBOY_PATH_CAPACITY];
                const int length = snprintf(lock_path, sizeof(lock_path),
                                            "%s.dualboy.lock",
                                            cleanup_paths[machine]);

                (void)unlink(cleanup_paths[machine]);
                if (length > 0 && (size_t)length < sizeof(lock_path)) {
                    (void)unlink(lock_path);
                }
            }
        }
        (void)rmdir(temporary_directory);
    }
    return success;
}

int main(void)
{
    if (!test_rejections_and_partial_teardown() ||
        !test_save_type_override_extents() ||
        !test_resolved_flash_state_into_fresh_pair() ||
        !test_startup_sio_mode_burst_is_scheduled_without_loss() ||
        !run_linked_pair_case() ||
        !test_disk_persistence_round_trip() ||
        !test_rtc_survives_machine_state_restore()) {
        return EXIT_FAILURE;
    }
    puts("mGBA adapter tests passed (guest SIO, state-safe RTC, and disk reload verified)");
    return EXIT_SUCCESS;
}
