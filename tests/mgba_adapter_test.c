/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frontend/engine.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ROM_SIZE 0x8000U
#define TEST_CODE_OFFSET 0xC0U
#define TEST_IO_LITERAL 0x300U
#define TEST_SIO_LITERAL 0x304U
#define TEST_SRAM_LITERAL 0x308U
#define TEST_VRAM_LITERAL 0x30CU
#define TEST_SEND_PRIMARY_LITERAL 0x310U
#define TEST_SEND_SECONDARY_LITERAL 0x314U
#define TEST_SIGNATURE_OFFSET 0x340U
#define TEST_FRAME_LIMIT 300U
#define TEST_AUDIO_CAPACITY 20000U

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            return false;                                                       \
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

/* Build a small ARM-state cartridge. In multiplayer mode the primary sends
 * 0x1111 and the secondary sends 0x2222. Each stores the received low byte in
 * SRAM[1], and only writes 0x5a to SRAM[2] after checking the full 16-bit word.
 * SRAM[0] records the hardware-assigned player ID; SRAM[3] records KEYINPUT. */
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

    /* Load IO, SIO, SRAM, and mode-3 VRAM base addresses. */
    emit_literal_load(&builder, 0U, TEST_IO_LITERAL);
    emit_literal_load(&builder, 4U, TEST_SIO_LITERAL);
    emit_literal_load(&builder, 1U, TEST_SRAM_LITERAL);
    emit_literal_load(&builder, 7U, TEST_VRAM_LITERAL);

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
    memcpy(rom + TEST_SIGNATURE_OFFSET, "SRAM_V110", 9U);

    for (index = 0xA0U; index <= 0xBCU; ++index) {
        checksum = (uint8_t)(checksum - rom[index]);
    }
    rom[0xBDU] = (uint8_t)(checksum - 0x19U);
    return true;
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

static bool run_linked_pair_case(void)
{
    const struct dualboy_engine_ops *operations = dualboy_mgba_engine();
    const struct dualboy_engine_config config = {
        .audio_sample_rate = 48000U,
    };
    struct dualboy_rom content = {0};
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
    uint64_t replay_video_hash[2] = {0U, 0U};
    char error[256] = {0};
    unsigned index;
    bool exchange_complete = false;
    bool rtc_present[2];

    CHECK(operations != NULL);
    CHECK(operations->family == DUALBOY_ENGINE_MGBA);
    CHECK(strcmp(operations->name, "mGBA") == 0);

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

    for (index = 0U; index < 2U; ++index) {
        CHECK(operations->memory_info(pair,
                                      index,
                                      DUALBOY_MEMORY_SAVE_RAM,
                                      &save[index],
                                      &save_size[index]));
        CHECK(save[index] != NULL);
        CHECK(save_size[index] >= 0x8000U);
        CHECK(!operations->memory_dirty(pair, index));

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
        CHECK(!operations->serialize_machine(pair,
                                             index,
                                             machine_state[index],
                                             machine_state_size[index] - 1U,
                                             &used));
    }

    link_state_capacity = operations->link_state_size(pair);
    CHECK(link_state_capacity >= 1024U);
    link_state = (uint8_t *)malloc(link_state_capacity);
    link_state_again = (uint8_t *)malloc(link_state_capacity);
    CHECK(link_state != NULL && link_state_again != NULL);
    CHECK(operations->serialize_link(pair,
                                     link_state,
                                     link_state_capacity,
                                     &link_state_used));
    CHECK(link_state_used == link_state_capacity);
    CHECK(!operations->unserialize_link(pair,
                                        link_state,
                                        link_state_used - 1U));

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

int main(void)
{
    if (!test_rejections_and_partial_teardown() || !run_linked_pair_case()) {
        return EXIT_FAILURE;
    }
    puts("mGBA adapter tests passed (guest SIO exchange verified)");
    return EXIT_SUCCESS;
}
