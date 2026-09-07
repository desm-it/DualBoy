/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/*
 * DualBoy's two-machine mGBA adapter. The cooperative link scheduler and
 * SIO driver are adapted from libretro/mgba PR #318, pinned at
 * fa743c965939f091350df094f57e639933bc17e3.
 */

#include "../../frontend/engine.h"
#include "mgba_adapter_internal.h"
#include "mgba_lockstep.h"

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gba/memory.h>
#include <mgba/internal/gba/savedata.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MGBA_VIDEO_WIDTH 240U
#define MGBA_VIDEO_HEIGHT 160U
#define MGBA_VIDEO_PIXELS \
    ((size_t)MGBA_VIDEO_WIDTH * (size_t)MGBA_VIDEO_HEIGHT)
#define MGBA_DEFAULT_AUDIO_RATE 48000U
#define MGBA_AUDIO_BUFFER_FRAMES 4096U
#define MGBA_RESAMPLED_AUDIO_FRAMES 16384U
#define MGBA_COOPERATIVE_WATCHDOG 4000000U
#define MGBA_MAX_SAVE_SIZE ((size_t)GBA_SIZE_FLASH1M)
#define MGBA_RTC_SIZE ((size_t)sizeof(struct GBASavedataRTCBuffer))

#define MGBA_MACHINE_STATE_HEADER_SIZE 32U
#define MGBA_MACHINE_STATE_VERSION 1U
#define MGBA_LINK_STATE_HEADER_SIZE 32U
#define MGBA_LINK_STATE_VERSION 2U
#define MGBA_LINK_STATE_FLAG_ENABLED UINT32_C(1)
#define MGBA_LOCKSTEP_DRIVER_STATE_SIZE 0xc70U
#define MGBA_LINK_STATE_CAPACITY \
    (MGBA_LINK_STATE_HEADER_SIZE + \
     (2U * (size_t)MGBA_LOCKSTEP_DRIVER_STATE_SIZE))

_Static_assert(sizeof(struct GBASavedataRTCBuffer) == 16U,
               "mGBA RTC persistence record changed size");

struct mgba_pair;

struct mgba_machine {
    struct mgba_pair *pair;
    struct mCore *core;
    unsigned index;
    bool config_initialized;
    bool core_initialized;
    bool loaded;
    bool reset_once;
    bool memory_imported;
    bool memory_dirty;
    bool persistent_restore_failed;
    bool crashed;
    uint8_t *rom_data;
    size_t rom_size;
    size_t save_size;
    size_t rtc_size;
    uint8_t save_data[GBA_SIZE_FLASH1M];
    uint8_t rtc_data[sizeof(struct GBASavedataRTCBuffer)];
    mColor native_pixels[MGBA_VIDEO_PIXELS];
    uint32_t pixels[MGBA_VIDEO_PIXELS];
    unsigned video_width;
    unsigned video_height;
    struct mCoreCallbacks callbacks;
};

struct mgba_lockstep_user {
    struct mLockstepUser d;
    struct mgba_pair *pair;
    unsigned machine;
    bool blocked;
};

struct mgba_pair {
    struct mgba_machine machine[DUALBOY_MACHINE_COUNT];
    struct DualBoyGBASIOLockstepCoordinator coordinator;
    struct DualBoyGBASIOLockstepDriver drivers[DUALBOY_MACHINE_COUNT];
    struct mgba_lockstep_user users[DUALBOY_MACHINE_COUNT];
    bool coordinator_initialized;
    bool link_configured;
    bool link_enabled;
    unsigned audio_rate;
    struct mAudioBuffer resampled_audio;
    struct mAudioResampler resampler;
    bool audio_buffer_initialized;
    bool resampler_initialized;
    dualboy_log_fn log;
    void *log_context;
    char *system_directory;
};

struct mgba_persistent_snapshot {
    enum GBASavedataType type;
    uint8_t *save_data;
    size_t save_size;
    uint8_t *stable_save_data;
    size_t stable_save_size;
    size_t rtc_size;
    uint8_t rtc_data[sizeof(struct GBASavedataRTCBuffer)];
    bool memory_dirty;
    int savedata_dirty;
    uint32_t dirt_age;
    bool live_captured;
    bool captured;
};

static void clear_error(char *error, size_t error_size)
{
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
}

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static void log_message(const struct mgba_pair *pair,
                        enum dualboy_log_level level,
                        const char *message)
{
    if (pair != NULL && pair->log != NULL) {
        pair->log(pair->log_context, level, message);
    }
}

static char *duplicate_string(const char *source)
{
    char *copy;
    size_t size;

    if (source == NULL) {
        return NULL;
    }
    size = strlen(source) + 1U;
    copy = (char *)malloc(size);
    if (copy != NULL) {
        memcpy(copy, source, size);
    }
    return copy;
}

static void put_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
}

static uint16_t get_u16_le(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] |
                      (uint16_t)((uint16_t)source[1] << 8U));
}

static void put_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

static uint32_t get_u32_le(const uint8_t *source)
{
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

static void put_u64_le(uint8_t *destination, uint64_t value)
{
    unsigned index;

    for (index = 0U; index < 8U; ++index) {
        destination[index] = (uint8_t)(value >> (index * 8U));
    }
}

static uint64_t get_u64_le(const uint8_t *source)
{
    uint64_t value = 0U;
    unsigned index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)source[index] << (index * 8U);
    }
    return value;
}

static bool bytes_are_ff(const uint8_t *data, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (data[index] != UINT8_C(0xff)) {
            return false;
        }
    }
    return true;
}

static bool pair_ready(const struct mgba_pair *pair)
{
    return pair != NULL && pair->machine[0].loaded &&
           pair->machine[1].loaded && pair->machine[0].core != NULL &&
           pair->machine[1].core != NULL;
}

static struct GBASavedata *machine_savedata(struct mgba_machine *machine)
{
    struct GBA *gba;

    if (machine == NULL || machine->core == NULL ||
        machine->core->board == NULL) {
        return NULL;
    }
    gba = (struct GBA *)machine->core->board;
    return &gba->memory.savedata;
}

static const struct GBASavedata *machine_savedata_const(
    const struct mgba_machine *machine)
{
    const struct GBA *gba;

    if (machine == NULL || machine->core == NULL ||
        machine->core->board == NULL) {
        return NULL;
    }
    gba = (const struct GBA *)machine->core->board;
    return &gba->memory.savedata;
}

static bool machine_has_rtc(const struct mgba_machine *machine)
{
    const struct GBASavedata *savedata = machine_savedata_const(machine);

    return savedata != NULL && savedata->gpio != NULL &&
           (savedata->gpio->devices & HW_RTC) != 0U;
}

static void update_memory_layout(struct mgba_machine *machine)
{
    const struct GBASavedata *savedata = machine_savedata_const(machine);

    if (savedata == NULL) {
        return;
    }
    /* Once exposed, the frontend is allowed to retain the RTC size and
     * address. A restore or reset must not make that region disappear. */
    if (machine_has_rtc(machine)) {
        machine->rtc_size = MGBA_RTC_SIZE;
    }
}

static void read_rtc_record(struct mgba_machine *machine)
{
    struct GBASavedata *savedata = machine_savedata(machine);
    size_t offset;

    if (savedata == NULL || savedata->vf == NULL ||
        machine->rtc_size != MGBA_RTC_SIZE) {
        return;
    }
    offset = GBASavedataSize(savedata) & ~(size_t)UINT8_MAX;
    if (savedata->vf->seek(savedata->vf, (off_t)offset, SEEK_SET) < 0) {
        return;
    }
    if (savedata->vf->read(savedata->vf,
                            machine->rtc_data,
                            MGBA_RTC_SIZE) != (ssize_t)MGBA_RTC_SIZE) {
        memset(machine->rtc_data, 0xff, MGBA_RTC_SIZE);
    }
}

static void sync_machine_storage(struct mgba_machine *machine, bool flush_rtc)
{
    void *copy = NULL;
    size_t size;

    if (machine == NULL || !machine->loaded || machine->core == NULL ||
        !machine->memory_imported) {
        return;
    }
    size = machine->core->savedataClone(machine->core, &copy);
    if (size != 0U && copy != NULL && size <= MGBA_MAX_SAVE_SIZE) {
        memcpy(machine->save_data, copy, size);
    }
    free(copy);

    update_memory_layout(machine);
    if (machine->rtc_size != 0U) {
        struct GBASavedata *savedata = machine_savedata(machine);

        if (flush_rtc && savedata != NULL) {
            GBASavedataRTCWrite(savedata);
        }
        read_rtc_record(machine);
    }
}

static bool import_machine_storage(struct mgba_machine *machine)
{
    struct GBASavedata *savedata;
    size_t offset;

    if (machine == NULL || !machine->loaded || machine->core == NULL) {
        return false;
    }
    if (machine->memory_imported) {
        return true;
    }
    update_memory_layout(machine);
    if (machine->save_size != 0U &&
        !machine->core->savedataRestore(machine->core,
                                        machine->save_data,
                                        machine->save_size,
                                        true)) {
        return false;
    }

    savedata = machine_savedata(machine);
    if (machine->rtc_size != 0U && savedata != NULL &&
        savedata->vf != NULL) {
        /* Let mGBA grow/remap the save VFile safely before replacing its tail. */
        GBASavedataRTCWrite(savedata);
        offset = GBASavedataSize(savedata) & ~(size_t)UINT8_MAX;
        if (!bytes_are_ff(machine->rtc_data, MGBA_RTC_SIZE)) {
            if (savedata->vf->seek(savedata->vf,
                                    (off_t)offset,
                                    SEEK_SET) < 0 ||
                savedata->vf->write(savedata->vf,
                                     machine->rtc_data,
                                     MGBA_RTC_SIZE) !=
                    (ssize_t)MGBA_RTC_SIZE) {
                return false;
            }
            GBASavedataRTCRead(savedata);
        }
        read_rtc_record(machine);
    }
    machine->memory_imported = true;
    return true;
}

static bool import_pair_storage(struct mgba_pair *pair)
{
    unsigned index;

    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        if (!import_machine_storage(&pair->machine[index])) {
            return false;
        }
    }
    return true;
}

/* mGBA's native state includes the save-controller type and live RTC fields.
 * Those are emulation state, but they must not make loading a rewind/runahead
 * state roll durable battery progress backwards. Preserve the live device and
 * rehydrate it after every raw machine-state load, including failed loads. */
static bool snapshot_persistent_storage(
    struct mgba_machine *machine,
    struct mgba_persistent_snapshot *snapshot)
{
    struct GBASavedata *savedata;
    void *copy = NULL;
    size_t size;

    if (snapshot == NULL) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (machine == NULL || !machine->loaded || machine->core == NULL) {
        return false;
    }
    if (machine->memory_imported) {
        sync_machine_storage(machine, true);
    }
    savedata = machine_savedata(machine);
    if (savedata == NULL) {
        return false;
    }
    snapshot->type = savedata->type;
    snapshot->rtc_size = machine->rtc_size;
    snapshot->memory_dirty = machine->memory_dirty;
    snapshot->savedata_dirty = savedata->dirty;
    snapshot->dirt_age = savedata->dirtAge;
    snapshot->captured = true;
    if (!machine->memory_imported) {
        return true;
    }

    size = GBASavedataSize(savedata);
    snapshot->save_size = machine->core->savedataClone(machine->core, &copy);
    if (snapshot->save_size != size ||
        snapshot->save_size > MGBA_MAX_SAVE_SIZE + MGBA_RTC_SIZE ||
        (snapshot->type != GBA_SAVEDATA_AUTODETECT &&
         snapshot->save_size > MGBA_MAX_SAVE_SIZE) ||
        (snapshot->save_size != 0U && copy == NULL)) {
        free(copy);
        memset(snapshot, 0, sizeof(*snapshot));
        return false;
    }
    snapshot->save_data = (uint8_t *)copy;
    if (machine->save_size != 0U) {
        snapshot->stable_save_data = (uint8_t *)malloc(machine->save_size);
        if (snapshot->stable_save_data == NULL) {
            free(snapshot->save_data);
            memset(snapshot, 0, sizeof(*snapshot));
            return false;
        }
        memcpy(snapshot->stable_save_data,
               machine->save_data,
               machine->save_size);
        snapshot->stable_save_size = machine->save_size;
    }
    if (snapshot->rtc_size != 0U) {
        if (snapshot->rtc_size != MGBA_RTC_SIZE) {
            free(snapshot->save_data);
            free(snapshot->stable_save_data);
            memset(snapshot, 0, sizeof(*snapshot));
            return false;
        }
        memcpy(snapshot->rtc_data, machine->rtc_data, MGBA_RTC_SIZE);
    }
    snapshot->live_captured = true;
    return true;
}

static enum GBASavedataType merge_savedata_type(
    enum GBASavedataType current,
    enum GBASavedataType restored)
{
    if (current == GBA_SAVEDATA_AUTODETECT) {
        return restored;
    }
    if (restored == GBA_SAVEDATA_AUTODETECT || current == restored) {
        return current;
    }
    if ((current == GBA_SAVEDATA_FLASH512 ||
         current == GBA_SAVEDATA_FLASH1M) &&
        (restored == GBA_SAVEDATA_FLASH512 ||
         restored == GBA_SAVEDATA_FLASH1M)) {
        return current == GBA_SAVEDATA_FLASH1M ||
                       restored == GBA_SAVEDATA_FLASH1M
                   ? GBA_SAVEDATA_FLASH1M
                   : GBA_SAVEDATA_FLASH512;
    }
    if ((current == GBA_SAVEDATA_EEPROM512 ||
         current == GBA_SAVEDATA_EEPROM) &&
        (restored == GBA_SAVEDATA_EEPROM512 ||
         restored == GBA_SAVEDATA_EEPROM)) {
        return current == GBA_SAVEDATA_EEPROM ||
                       restored == GBA_SAVEDATA_EEPROM
                   ? GBA_SAVEDATA_EEPROM
                   : GBA_SAVEDATA_EEPROM512;
    }
    if ((current == GBA_SAVEDATA_SRAM ||
         current == GBA_SAVEDATA_SRAM512) &&
        (restored == GBA_SAVEDATA_SRAM ||
         restored == GBA_SAVEDATA_SRAM512)) {
        return current == GBA_SAVEDATA_SRAM512 ||
                       restored == GBA_SAVEDATA_SRAM512
                   ? GBA_SAVEDATA_SRAM512
                   : GBA_SAVEDATA_SRAM;
    }
    /* Different resolved families cannot arise in an ordinary same-ROM
     * session. Keep the current battery authority if a crafted state does. */
    return current;
}

static bool restore_persistent_storage(
    struct mgba_machine *machine,
    const struct mgba_persistent_snapshot *snapshot)
{
    struct GBASavedata *savedata;
    enum GBASavedataType selected_type;
    const uint8_t *restore_data;
    size_t restore_size;
    size_t offset;
    bool success = true;

    if (snapshot == NULL || !snapshot->captured) {
        return true;
    }
    savedata = machine_savedata(machine);
    if (savedata == NULL) {
        return false;
    }

    selected_type = merge_savedata_type(snapshot->type, savedata->type);
    GBASavedataForceType(savedata, selected_type);
    restore_data = snapshot->save_data;
    restore_size = snapshot->save_size;
    if (snapshot->live_captured &&
        selected_type != GBA_SAVEDATA_AUTODETECT) {
        const size_t selected_size = GBASavedataSize(savedata);

        if (selected_size > MGBA_MAX_SAVE_SIZE) {
            success = false;
        }
        else if (selected_size != restore_size) {
            if (selected_size > snapshot->stable_save_size ||
                (selected_size != 0U &&
                 snapshot->stable_save_data == NULL)) {
                success = false;
            }
            else {
                restore_data = snapshot->stable_save_data;
                restore_size = selected_size;
            }
        }
    }
    if (snapshot->live_captured && restore_size != 0U && success &&
        !machine->core->savedataRestore(machine->core,
                                        restore_data,
                                        restore_size,
                                        true)) {
        success = false;
    }
    if (snapshot->live_captured && snapshot->rtc_size != 0U) {
        if (savedata->gpio == NULL || savedata->vf == NULL ||
            (savedata->gpio->devices & HW_RTC) == 0U) {
            success = false;
        }
        else {
            /* Ensure the variable-sized memory VFile has an RTC tail and a
             * valid mapping before replacing the state-era record. */
            GBASavedataRTCWrite(savedata);
            offset = GBASavedataSize(savedata) & ~(size_t)UINT8_MAX;
            if (savedata->vf->seek(savedata->vf, (off_t)offset, SEEK_SET) < 0 ||
                savedata->vf->write(savedata->vf,
                                    snapshot->rtc_data,
                                    MGBA_RTC_SIZE) != (ssize_t)MGBA_RTC_SIZE) {
                success = false;
            }
            else {
                GBASavedataRTCRead(savedata);
            }
        }
    }

    machine->rtc_size = snapshot->rtc_size;
    machine->memory_dirty = snapshot->memory_dirty;
    savedata->dirty = snapshot->savedata_dirty;
    savedata->dirtAge = snapshot->dirt_age;
    sync_machine_storage(machine, false);
    return success;
}

static void free_persistent_snapshot(
    struct mgba_persistent_snapshot *snapshot)
{
    if (snapshot != NULL) {
        free(snapshot->save_data);
        free(snapshot->stable_save_data);
        memset(snapshot, 0, sizeof(*snapshot));
    }
}

static void savedata_updated(void *context)
{
    struct mgba_machine *machine = (struct mgba_machine *)context;

    if (machine == NULL) {
        return;
    }
    machine->memory_dirty = true;
    sync_machine_storage(machine, false);
}

static void core_crashed(void *context)
{
    struct mgba_machine *machine = (struct mgba_machine *)context;

    if (machine != NULL) {
        machine->crashed = true;
    }
}

static bool load_optional_bios(struct mgba_machine *machine)
{
#ifdef ENABLE_VFS
    const char suffix[] = "/gba_bios.bin";
    struct VFile *bios;
    char *path;
    size_t directory_size;

    if (machine->pair->system_directory == NULL ||
        machine->pair->system_directory[0] == '\0') {
        return false;
    }
    directory_size = strlen(machine->pair->system_directory);
    if (directory_size > SIZE_MAX - sizeof(suffix)) {
        return false;
    }
    path = (char *)malloc(directory_size + sizeof(suffix));
    if (path == NULL) {
        return false;
    }
    (void)snprintf(path,
                   directory_size + sizeof(suffix),
                   "%s%s",
                   machine->pair->system_directory,
                   suffix);
    bios = VFileOpen(path, O_RDONLY);
    free(path);
    if (bios == NULL) {
        return false;
    }
    if (!machine->core->loadBIOS(machine->core, bios, 0)) {
        bios->close(bios);
        log_message(machine->pair,
                    DUALBOY_LOG_WARN,
                    "Ignoring an invalid gba_bios.bin in the system directory");
        return false;
    }
    return true;
#else
    (void)machine;
    return false;
#endif
}

static void configure_callbacks(struct mgba_machine *machine)
{
    memset(&machine->callbacks, 0, sizeof(machine->callbacks));
    machine->callbacks.context = machine;
    machine->callbacks.coreCrashed = core_crashed;
    machine->callbacks.shutdown = core_crashed;
    machine->callbacks.savedataUpdated = savedata_updated;
    machine->core->addCoreCallbacks(machine->core, &machine->callbacks);
}

static void destroy_machine(struct mgba_machine *machine)
{
    if (machine == NULL) {
        return;
    }
    if (machine->core != NULL && machine->core_initialized) {
        sync_machine_storage(machine, true);
        machine->core->clearCoreCallbacks(machine->core);
    }
    if (machine->config_initialized && machine->core != NULL) {
        mCoreConfigDeinit(&machine->core->config);
        machine->config_initialized = false;
    }
    if (machine->core != NULL) {
        if (machine->core_initialized) {
            machine->core->deinit(machine->core);
        }
        else {
            free(machine->core);
        }
        machine->core = NULL;
    }
    free(machine->rom_data);
    machine->rom_data = NULL;
    machine->rom_size = 0U;
    machine->core_initialized = false;
    machine->loaded = false;
    machine->reset_once = false;
    machine->memory_imported = false;
    machine->memory_dirty = false;
    machine->persistent_restore_failed = false;
    machine->crashed = false;
    machine->save_size = 0U;
    machine->rtc_size = 0U;
    machine->video_width = 0U;
    machine->video_height = 0U;
}

static void lockstep_sleep(struct mLockstepUser *user)
{
    struct mgba_lockstep_user *lockstep_user =
        (struct mgba_lockstep_user *)user;

    if (lockstep_user != NULL && lockstep_user->pair != NULL &&
        pair_ready(lockstep_user->pair)) {
        lockstep_user->blocked = true;
    }
}

static void lockstep_wake(struct mLockstepUser *user)
{
    struct mgba_lockstep_user *lockstep_user =
        (struct mgba_lockstep_user *)user;

    if (lockstep_user != NULL) {
        lockstep_user->blocked = false;
    }
}

static int lockstep_requested_id(struct mLockstepUser *user)
{
    const struct mgba_lockstep_user *lockstep_user =
        (const struct mgba_lockstep_user *)user;

    return lockstep_user != NULL ? (int)lockstep_user->machine : 0;
}

static void detach_link(struct mgba_pair *pair)
{
    unsigned index;

    if (pair == NULL || !pair->coordinator_initialized) {
        if (pair != NULL) {
            pair->link_enabled = false;
        }
        return;
    }
    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        struct mCore *core = pair->machine[index].core;

        if (core != NULL) {
            core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, NULL);
        }
    }
    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        if (pair->drivers[index].coordinator == &pair->coordinator) {
            DualBoyGBASIOLockstepCoordinatorDetach(&pair->coordinator,
                                                    &pair->drivers[index]);
        }
        pair->users[index].blocked = false;
    }
    DualBoyGBASIOLockstepCoordinatorDeinit(&pair->coordinator);
    pair->coordinator_initialized = false;
    pair->link_enabled = false;
}

static bool attach_link(struct mgba_pair *pair)
{
    unsigned index;

    if (!pair_ready(pair)) {
        return false;
    }
    DualBoyGBASIOLockstepCoordinatorInit(&pair->coordinator);
    pair->coordinator_initialized = true;
    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        struct mgba_lockstep_user *user = &pair->users[index];

        memset(user, 0, sizeof(*user));
        user->d.sleep = lockstep_sleep;
        user->d.wake = lockstep_wake;
        user->d.requestedId = lockstep_requested_id;
        user->pair = pair;
        user->machine = index;
        DualBoyGBASIOLockstepDriverCreate(&pair->drivers[index], &user->d);
        DualBoyGBASIOLockstepCoordinatorAttach(&pair->coordinator,
                                               &pair->drivers[index]);
    }
    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        struct mCore *core = pair->machine[index].core;

        core->setPeripheral(core,
                            mPERIPH_GBA_LINK_PORT,
                            &pair->drivers[index].d);
        if (pair->drivers[index].lockstepId == 0U) {
            detach_link(pair);
            return false;
        }
    }
    pair->link_enabled = true;
    return true;
}

static void reset_audio_pipeline(struct mgba_pair *pair)
{
    unsigned index;

    if (pair->audio_buffer_initialized) {
        mAudioBufferClear(&pair->resampled_audio);
    }
    if (pair->resampler_initialized) {
        mAudioResamplerDeinit(&pair->resampler);
        mAudioResamplerInit(&pair->resampler, mINTERPOLATOR_SINC);
        mAudioResamplerSetDestination(&pair->resampler,
                                      &pair->resampled_audio,
                                      (double)pair->audio_rate);
    }
    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        if (pair->machine[index].core != NULL) {
            struct mAudioBuffer *audio =
                pair->machine[index].core->getAudioBuffer(
                    pair->machine[index].core);

            if (audio != NULL) {
                mAudioBufferClear(audio);
            }
        }
    }
}

static void reset_cores(struct mgba_pair *pair)
{
    int index;

    for (index = (int)DUALBOY_MACHINE_COUNT - 1; index >= 0; --index) {
        struct mgba_machine *machine = &pair->machine[(unsigned)index];

        machine->crashed = false;
        machine->core->reset(machine->core);
        machine->reset_once = true;
        update_memory_layout(machine);
    }
    reset_audio_pipeline(pair);
}

static bool mgba_create_pair(void **output_pair,
                             const struct dualboy_engine_config *config,
                             char *error,
                             size_t error_size)
{
    struct mgba_pair *pair;
    unsigned index;

    clear_error(error, error_size);
    if (output_pair == NULL) {
        set_error(error, error_size, "mGBA pair output is null");
        return false;
    }
    *output_pair = NULL;
    pair = (struct mgba_pair *)calloc(1U, sizeof(*pair));
    if (pair == NULL) {
        set_error(error, error_size, "Could not allocate mGBA pair");
        return false;
    }
    pair->audio_rate = config != NULL && config->audio_sample_rate != 0U
                           ? config->audio_sample_rate
                           : MGBA_DEFAULT_AUDIO_RATE;
    if (config != NULL) {
        pair->log = config->log;
        pair->log_context = config->log_context;
        pair->system_directory = duplicate_string(config->system_directory);
        if (config->system_directory != NULL &&
            pair->system_directory == NULL) {
            set_error(error, error_size, "Could not copy mGBA system path");
            free(pair);
            return false;
        }
    }
    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        pair->machine[index].pair = pair;
        pair->machine[index].index = index;
        memset(pair->machine[index].save_data,
               0xff,
               sizeof(pair->machine[index].save_data));
        memset(pair->machine[index].rtc_data,
               0xff,
               sizeof(pair->machine[index].rtc_data));
    }

    mAudioBufferInit(&pair->resampled_audio,
                     MGBA_RESAMPLED_AUDIO_FRAMES,
                     2U);
    pair->audio_buffer_initialized = true;
    if (pair->resampled_audio.data.data == NULL) {
        set_error(error, error_size, "Could not allocate mGBA audio buffer");
        mAudioBufferDeinit(&pair->resampled_audio);
        free(pair->system_directory);
        free(pair);
        return false;
    }
    mAudioResamplerInit(&pair->resampler, mINTERPOLATOR_SINC);
    pair->resampler_initialized = true;
    mAudioResamplerSetDestination(&pair->resampler,
                                  &pair->resampled_audio,
                                  (double)pair->audio_rate);
    *output_pair = pair;
    return true;
}

static bool mgba_load_rom(void *opaque_pair,
                          unsigned machine_index,
                          const struct dualboy_rom *rom,
                          char *error,
                          size_t error_size)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;
    struct mgba_machine *machine;
    struct mCoreOptions options;
    struct VFile *rom_file = NULL;
    struct VFile *save_file = NULL;
    bool bios_loaded;

    clear_error(error, error_size);
    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT) {
        set_error(error, error_size, "Invalid mGBA pair or machine index");
        return false;
    }
    if (rom == NULL || rom->platform != DUALBOY_PLATFORM_GBA ||
        rom->data == NULL || rom->size < 0xc0U ||
        rom->size > (size_t)GBA_SIZE_ROM0) {
        set_error(error, error_size, "Invalid GBA ROM buffer");
        return false;
    }
    machine = &pair->machine[machine_index];
    if (machine->loaded || machine->core != NULL) {
        set_error(error,
                  error_size,
                  "mGBA machine %u is already loaded",
                  machine_index);
        return false;
    }

    machine->rom_data = (uint8_t *)malloc(rom->size);
    if (machine->rom_data == NULL) {
        set_error(error,
                  error_size,
                  "Could not allocate ROM copy for mGBA machine %u",
                  machine_index);
        return false;
    }
    memcpy(machine->rom_data, rom->data, rom->size);
    machine->rom_size = rom->size;
    machine->core = GBACoreCreate();
    if (machine->core == NULL) {
        set_error(error,
                  error_size,
                  "Could not create mGBA machine %u",
                  machine_index);
        destroy_machine(machine);
        return false;
    }
    mCoreInitConfig(machine->core, NULL);
    machine->config_initialized = true;
    if (!machine->core->init(machine->core)) {
        set_error(error,
                  error_size,
                  "Could not initialize mGBA machine %u",
                  machine_index);
        destroy_machine(machine);
        return false;
    }
    machine->core_initialized = true;
    bios_loaded = load_optional_bios(machine);

    memset(&options, 0, sizeof(options));
    options.useBios = bios_loaded;
    options.skipBios = !bios_loaded;
    options.volume = 0x100;
    options.audioBuffers = MGBA_AUDIO_BUFFER_FRAMES;
    options.sampleRate = pair->audio_rate;
    mCoreConfigLoadDefaults(&machine->core->config, &options);
    mCoreLoadConfig(machine->core);
    machine->core->setVideoBuffer(machine->core,
                                  machine->native_pixels,
                                  MGBA_VIDEO_WIDTH);
    configure_callbacks(machine);

    rom_file = VFileFromMemory(machine->rom_data, machine->rom_size);
    if (rom_file == NULL || !machine->core->loadROM(machine->core, rom_file)) {
        if (rom_file != NULL) {
            rom_file->close(rom_file);
        }
        set_error(error,
                  error_size,
                  "mGBA rejected ROM for machine %u",
                  machine_index);
        destroy_machine(machine);
        return false;
    }
    rom_file = NULL;

    save_file = VFileMemChunk(NULL, 0U);
    if (save_file == NULL ||
        !machine->core->loadSave(machine->core, save_file)) {
        if (save_file != NULL) {
            save_file->close(save_file);
        }
        set_error(error,
                  error_size,
                  "Could not create save storage for mGBA machine %u",
                  machine_index);
        destroy_machine(machine);
        return false;
    }
    save_file = NULL;
    machine->loaded = true;
    /* The frontend may retain this pointer and size for the whole session.
     * Keep the exported region fixed even after mGBA detects a smaller chip. */
    machine->save_size = MGBA_MAX_SAVE_SIZE;
    machine->video_width = MGBA_VIDEO_WIDTH;
    machine->video_height = MGBA_VIDEO_HEIGHT;
    memset(machine->native_pixels, 0, sizeof(machine->native_pixels));
    memset(machine->pixels, 0, sizeof(machine->pixels));
    return true;
}

static bool mgba_set_link(void *opaque_pair,
                          bool enabled,
                          char *error,
                          size_t error_size)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;

    clear_error(error, error_size);
    if (!pair_ready(pair)) {
        set_error(error,
                  error_size,
                  "Both mGBA machines must be loaded before configuring link");
        return false;
    }
    if (pair->link_configured && pair->link_enabled == enabled) {
        return true;
    }
    if (pair->coordinator_initialized) {
        detach_link(pair);
    }
    if (enabled && !attach_link(pair)) {
        set_error(error, error_size, "Could not attach the mGBA link driver");
        return false;
    }
    pair->link_enabled = enabled;
    reset_cores(pair);
    pair->link_configured = true;
    return true;
}

static void mgba_reset(void *opaque_pair)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;

    if (!pair_ready(pair) || !pair->link_configured) {
        return;
    }
    if (!import_pair_storage(pair)) {
        log_message(pair,
                    DUALBOY_LOG_ERROR,
                    "Could not import mGBA persistent memory before reset");
        return;
    }
    reset_cores(pair);
}

static void mgba_set_input(void *opaque_pair,
                           unsigned machine_index,
                           uint16_t buttons)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;
    uint32_t keys = 0U;

    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT ||
        !pair->machine[machine_index].loaded ||
        pair->machine[machine_index].core == NULL) {
        return;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_A) != 0U) {
        keys |= UINT32_C(1) << GBA_KEY_A;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_B) != 0U) {
        keys |= UINT32_C(1) << GBA_KEY_B;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_SELECT) != 0U) {
        keys |= UINT32_C(1) << GBA_KEY_SELECT;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_START) != 0U) {
        keys |= UINT32_C(1) << GBA_KEY_START;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_RIGHT) != 0U) {
        keys |= UINT32_C(1) << GBA_KEY_RIGHT;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_LEFT) != 0U) {
        keys |= UINT32_C(1) << GBA_KEY_LEFT;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_UP) != 0U) {
        keys |= UINT32_C(1) << GBA_KEY_UP;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_DOWN) != 0U) {
        keys |= UINT32_C(1) << GBA_KEY_DOWN;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_R) != 0U) {
        keys |= UINT32_C(1) << GBA_KEY_R;
    }
    if ((buttons & (uint16_t)DUALBOY_BUTTON_L) != 0U) {
        keys |= UINT32_C(1) << GBA_KEY_L;
    }
    pair->machine[machine_index].core->setKeys(
        pair->machine[machine_index].core,
        keys);
}

static bool run_linked_frame(struct mgba_pair *pair,
                             char *error,
                             size_t error_size)
{
    uint32_t start_frames[DUALBOY_MACHINE_COUNT];
    bool done[DUALBOY_MACHINE_COUNT] = {false, false};
    unsigned watchdog = MGBA_COOPERATIVE_WATCHDOG;
    unsigned index;

    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        start_frames[index] =
            pair->machine[index].core->frameCounter(pair->machine[index].core);
    }
    while (watchdog != 0U) {
        bool all_done = true;
        bool ran_any = false;
        unsigned blocked_count = 0U;

        for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
            if (pair->users[index].blocked) {
                ++blocked_count;
            }
        }
        for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
            struct mgba_machine *machine = &pair->machine[index];

            if (pair->users[index].blocked ||
                (done[index] && blocked_count == 0U)) {
                continue;
            }
            machine->core->runLoop(machine->core);
            --watchdog;
            ran_any = true;
            if (machine->crashed) {
                set_error(error,
                          error_size,
                          "mGBA machine %u crashed",
                          index);
                return false;
            }
            if (!done[index] &&
                machine->core->frameCounter(machine->core) !=
                    start_frames[index]) {
                done[index] = true;
            }
            if (watchdog == 0U) {
                break;
            }
        }
        if (!ran_any) {
            set_error(error,
                      error_size,
                      "All linked mGBA machines blocked simultaneously");
            return false;
        }
        for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
            if (!done[index]) {
                all_done = false;
                break;
            }
        }
        if (all_done) {
            return true;
        }
    }
    set_error(error,
              error_size,
              "mGBA cooperative scheduling watchdog expired");
    return false;
}

static uint32_t native_color_to_xrgb(mColor color)
{
#ifndef COLOR_16_BIT
    uint32_t value = (uint32_t)color;

    return (value & UINT32_C(0x0000ff00)) |
           ((value & UINT32_C(0x000000ff)) << 16U) |
           ((value & UINT32_C(0x00ff0000)) >> 16U);
#else
    uint32_t value = (uint32_t)color;
    uint32_t red;
    uint32_t green;
    uint32_t blue;

#ifdef COLOR_5_6_5
    red = ((value & UINT32_C(0x1f)) * UINT32_C(255)) / UINT32_C(31);
    green = (((value >> 5U) & UINT32_C(0x3f)) * UINT32_C(255)) /
            UINT32_C(63);
    blue = (((value >> 11U) & UINT32_C(0x1f)) * UINT32_C(255)) /
           UINT32_C(31);
#else
    red = ((value & UINT32_C(0x1f)) * UINT32_C(255)) / UINT32_C(31);
    green = (((value >> 5U) & UINT32_C(0x1f)) * UINT32_C(255)) /
            UINT32_C(31);
    blue = (((value >> 10U) & UINT32_C(0x1f)) * UINT32_C(255)) /
           UINT32_C(31);
#endif
    return (red << 16U) | (green << 8U) | blue;
#endif
}

static void update_video(struct mgba_machine *machine)
{
    unsigned width = MGBA_VIDEO_WIDTH;
    unsigned height = MGBA_VIDEO_HEIGHT;
    size_t pixels;
    size_t index;

    machine->core->currentVideoSize(machine->core, &width, &height);
    if (width > MGBA_VIDEO_WIDTH || height > MGBA_VIDEO_HEIGHT) {
        width = MGBA_VIDEO_WIDTH;
        height = MGBA_VIDEO_HEIGHT;
    }
    machine->video_width = width;
    machine->video_height = height;
    pixels = (size_t)width * (size_t)height;
    for (index = 0U; index < pixels; ++index) {
        machine->pixels[index] = native_color_to_xrgb(machine->native_pixels[index]);
    }
}

static void process_audio(struct mgba_pair *pair)
{
    struct mAudioBuffer *primary_audio =
        pair->machine[0].core->getAudioBuffer(pair->machine[0].core);
    struct mAudioBuffer *secondary_audio =
        pair->machine[1].core->getAudioBuffer(pair->machine[1].core);

    if (secondary_audio != NULL) {
        (void)mAudioBufferRead(secondary_audio,
                               NULL,
                               mAudioBufferAvailable(secondary_audio));
    }
    if (primary_audio == NULL || mAudioBufferAvailable(primary_audio) == 0U) {
        return;
    }
    mAudioResamplerSetSource(&pair->resampler,
                             primary_audio,
                             (double)pair->machine[0].core->audioSampleRate(
                                 pair->machine[0].core),
                             true);
    (void)mAudioResamplerProcess(&pair->resampler);
}

static bool mgba_run_frame(void *opaque_pair,
                           char *error,
                           size_t error_size)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;
    unsigned index;

    clear_error(error, error_size);
    if (!pair_ready(pair) || !pair->link_configured) {
        set_error(error, error_size, "mGBA pair is not fully configured");
        return false;
    }
    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        if (pair->machine[index].persistent_restore_failed) {
            set_error(error,
                      error_size,
                      "mGBA persistent-memory recovery failed");
            return false;
        }
    }
    if (!import_pair_storage(pair)) {
        set_error(error, error_size, "Could not import mGBA persistent memory");
        return false;
    }
    if (pair->link_enabled) {
        if (!run_linked_frame(pair, error, error_size)) {
            return false;
        }
    }
    else {
        for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
            pair->machine[index].core->runFrame(pair->machine[index].core);
            if (pair->machine[index].crashed) {
                set_error(error,
                          error_size,
                          "mGBA machine %u crashed",
                          index);
                return false;
            }
        }
    }
    for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
        update_video(&pair->machine[index]);
        if (pair->machine[index].rtc_size != 0U) {
            pair->machine[index].memory_dirty = true;
        }
    }
    process_audio(pair);
    return true;
}

static bool mgba_video_frame(void *opaque_pair,
                             unsigned machine_index,
                             struct dualboy_video_frame *frame)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;
    const struct mgba_machine *machine;

    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT ||
        frame == NULL) {
        return false;
    }
    machine = &pair->machine[machine_index];
    if (!machine->loaded || machine->video_width == 0U ||
        machine->video_height == 0U) {
        return false;
    }
    frame->pixels = machine->pixels;
    frame->width = machine->video_width;
    frame->height = machine->video_height;
    frame->pitch = (size_t)MGBA_VIDEO_WIDTH * sizeof(uint32_t);
    return true;
}

static unsigned mgba_audio_sample_rate(const void *opaque_pair)
{
    const struct mgba_pair *pair = (const struct mgba_pair *)opaque_pair;

    return pair != NULL ? pair->audio_rate : MGBA_DEFAULT_AUDIO_RATE;
}

static size_t mgba_read_audio(void *opaque_pair,
                              int16_t *interleaved_stereo,
                              size_t max_frames)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;

    if (pair == NULL ||
        (interleaved_stereo == NULL && max_frames != 0U)) {
        return 0U;
    }
    return mAudioBufferRead(&pair->resampled_audio,
                            interleaved_stereo,
                            max_frames);
}

static bool mgba_memory_info(void *opaque_pair,
                             unsigned machine_index,
                             enum dualboy_memory_kind kind,
                             void **data,
                             size_t *size)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;
    struct mgba_machine *machine;

    if (data == NULL || size == NULL) {
        return false;
    }
    *data = NULL;
    *size = 0U;
    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT) {
        return false;
    }
    machine = &pair->machine[machine_index];
    if (!machine->loaded || machine->core == NULL) {
        return false;
    }
    sync_machine_storage(machine, true);
    switch (kind) {
        case DUALBOY_MEMORY_SAVE_RAM:
            if (machine->save_size != 0U) {
                *data = machine->save_data;
                *size = machine->save_size;
            }
            return true;
        case DUALBOY_MEMORY_RTC:
            if (machine->rtc_size != 0U) {
                *data = machine->rtc_data;
                *size = machine->rtc_size;
            }
            return true;
        default:
            return false;
    }
}

static bool mgba_persistent_memory_extent(
    const void *opaque_pair,
    unsigned machine_index,
    enum dualboy_memory_kind kind,
    bool *known,
    size_t *size)
{
    const struct mgba_pair *pair = (const struct mgba_pair *)opaque_pair;
    const struct mgba_machine *machine;
    const struct GBASavedata *savedata;
    size_t detected_size;

    if (known != NULL) {
        *known = false;
    }
    if (size != NULL) {
        *size = 0U;
    }
    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT ||
        known == NULL || size == NULL) {
        return false;
    }
    machine = &pair->machine[machine_index];
    if (!machine->loaded || machine->core == NULL) {
        return false;
    }

    switch (kind) {
        case DUALBOY_MEMORY_SAVE_RAM:
            savedata = machine_savedata_const(machine);
            if (savedata == NULL) {
                return false;
            }
            switch (savedata->type) {
                case GBA_SAVEDATA_AUTODETECT:
                    /* The stable frontend buffer remains available at its
                     * maximum capacity, but no disk length is safe yet. */
                    return true;
                case GBA_SAVEDATA_FORCE_NONE:
                case GBA_SAVEDATA_SRAM:
                case GBA_SAVEDATA_FLASH512:
                case GBA_SAVEDATA_FLASH1M:
                case GBA_SAVEDATA_EEPROM:
                case GBA_SAVEDATA_EEPROM512:
                case GBA_SAVEDATA_SRAM512:
                    break;
                default:
                    return false;
            }
            detected_size = GBASavedataSize(savedata);
            if (detected_size > MGBA_MAX_SAVE_SIZE) {
                return false;
            }
            *known = true;
            *size = detected_size;
            return true;
        case DUALBOY_MEMORY_RTC:
            /* Cartridge hardware overrides are not applied until reset. */
            if (!machine->reset_once) {
                return true;
            }
            if (machine->rtc_size != 0U &&
                machine->rtc_size != MGBA_RTC_SIZE) {
                return false;
            }
            *known = true;
            *size = machine->rtc_size;
            return true;
        default:
            return false;
    }
}

static bool mgba_memory_dirty(const void *opaque_pair,
                              unsigned machine_index)
{
    const struct mgba_pair *pair = (const struct mgba_pair *)opaque_pair;
    const struct mgba_machine *machine;
    const struct GBASavedata *savedata;

    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT) {
        return false;
    }
    machine = &pair->machine[machine_index];
    if (!machine->loaded || machine->core == NULL) {
        return false;
    }
    savedata = machine_savedata_const(machine);
    /* mGBA deliberately waits for a quiet period before firing its savedata
     * callback. Surface its pending dirty state as well so an early forced
     * flush cannot miss the first write that also resolves autodetection. */
    return machine->memory_dirty ||
           (savedata != NULL && savedata->dirty != 0);
}

static void mgba_clear_memory_dirty(void *opaque_pair,
                                    unsigned machine_index)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;

    if (pair != NULL && machine_index < DUALBOY_MACHINE_COUNT) {
        pair->machine[machine_index].memory_dirty = false;
    }
}

static size_t mgba_machine_state_size(const void *opaque_pair,
                                      unsigned machine_index)
{
    const struct mgba_pair *pair = (const struct mgba_pair *)opaque_pair;
    size_t raw_size;

    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT ||
        !pair->machine[machine_index].loaded ||
        pair->machine[machine_index].core == NULL) {
        return 0U;
    }
    raw_size = pair->machine[machine_index].core->stateSize(
        pair->machine[machine_index].core);
    if (raw_size > SIZE_MAX - MGBA_MACHINE_STATE_HEADER_SIZE) {
        return 0U;
    }
    return MGBA_MACHINE_STATE_HEADER_SIZE + raw_size;
}

static bool mgba_serialize_machine(void *opaque_pair,
                                   unsigned machine_index,
                                   void *data,
                                   size_t capacity,
                                   size_t *used)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;
    struct mgba_machine *machine;
    uint8_t *bytes = (uint8_t *)data;
    uint8_t *raw;
    size_t raw_size;
    size_t required;

    if (used != NULL) {
        *used = 0U;
    }
    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT ||
        data == NULL || used == NULL) {
        return false;
    }
    machine = &pair->machine[machine_index];
    if (!machine->loaded || machine->core == NULL) {
        return false;
    }
    raw_size = machine->core->stateSize(machine->core);
    if (raw_size > SIZE_MAX - MGBA_MACHINE_STATE_HEADER_SIZE) {
        return false;
    }
    required = MGBA_MACHINE_STATE_HEADER_SIZE + raw_size;
    if (capacity < required) {
        return false;
    }
    raw = (uint8_t *)malloc(raw_size);
    if (raw == NULL) {
        return false;
    }
    memset(raw, 0, raw_size);
    if (!machine->core->saveState(machine->core, raw)) {
        free(raw);
        return false;
    }
    memset(bytes, 0, MGBA_MACHINE_STATE_HEADER_SIZE);
    bytes[0] = (uint8_t)'D';
    bytes[1] = (uint8_t)'B';
    bytes[2] = (uint8_t)'G';
    bytes[3] = (uint8_t)'M';
    put_u16_le(bytes + 4U, MGBA_MACHINE_STATE_VERSION);
    put_u16_le(bytes + 6U, MGBA_MACHINE_STATE_HEADER_SIZE);
    put_u64_le(bytes + 8U, (uint64_t)raw_size);
    put_u32_le(bytes + 16U, (uint32_t)machine->core->rtc.override);
    put_u64_le(bytes + 24U, (uint64_t)machine->core->rtc.value);
    memcpy(bytes + MGBA_MACHINE_STATE_HEADER_SIZE, raw, raw_size);
    free(raw);
    *used = required;
    return true;
}

static bool mgba_unserialize_machine(void *opaque_pair,
                                     unsigned machine_index,
                                     const void *data,
                                     size_t size)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;
    struct mgba_machine *machine;
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t *raw;
    uint64_t encoded_rtc;
    int64_t rtc_value;
    size_t raw_size;
    uint32_t rtc_type;
    bool success;
    bool storage_restored;
    struct mgba_persistent_snapshot persistent;

    if (pair == NULL || machine_index >= DUALBOY_MACHINE_COUNT ||
        data == NULL) {
        return false;
    }
    machine = &pair->machine[machine_index];
    if (!machine->loaded || machine->core == NULL ||
        machine->persistent_restore_failed ||
        size < MGBA_MACHINE_STATE_HEADER_SIZE ||
        bytes[0] != (uint8_t)'D' || bytes[1] != (uint8_t)'B' ||
        bytes[2] != (uint8_t)'G' || bytes[3] != (uint8_t)'M' ||
        get_u16_le(bytes + 4U) != MGBA_MACHINE_STATE_VERSION ||
        get_u16_le(bytes + 6U) != MGBA_MACHINE_STATE_HEADER_SIZE ||
        get_u32_le(bytes + 20U) != 0U) {
        return false;
    }
    raw_size = machine->core->stateSize(machine->core);
    if (get_u64_le(bytes + 8U) != (uint64_t)raw_size ||
        raw_size > SIZE_MAX - MGBA_MACHINE_STATE_HEADER_SIZE ||
        size != MGBA_MACHINE_STATE_HEADER_SIZE + raw_size) {
        return false;
    }
    rtc_type = get_u32_le(bytes + 16U);
    if (rtc_type > (uint32_t)RTC_WALLCLOCK_OFFSET) {
        return false;
    }
    if (!snapshot_persistent_storage(machine, &persistent)) {
        return false;
    }
    raw = (uint8_t *)malloc(raw_size);
    if (raw == NULL) {
        free_persistent_snapshot(&persistent);
        return false;
    }
    memcpy(raw, bytes + MGBA_MACHINE_STATE_HEADER_SIZE, raw_size);
    pair->drivers[machine_index].loadingMachineState = true;
    success = machine->core->loadState(machine->core, raw);
    pair->drivers[machine_index].loadingMachineState = false;
    free(raw);
    storage_restored = restore_persistent_storage(machine, &persistent);
    free_persistent_snapshot(&persistent);
    if (!storage_restored) {
        machine->persistent_restore_failed = true;
    }
    if (!success || !storage_restored) {
        return false;
    }
    encoded_rtc = get_u64_le(bytes + 24U);
    memcpy(&rtc_value, &encoded_rtc, sizeof(rtc_value));
    machine->core->rtc.override = (enum mRTCGenericType)rtc_type;
    machine->core->rtc.value = rtc_value;
    machine->crashed = false;
    machine->reset_once = true;
    update_memory_layout(machine);
    if (machine_index == 0U) {
        reset_audio_pipeline(pair);
    }
    return true;
}

static size_t mgba_link_state_size(const void *opaque_pair)
{
    (void)opaque_pair;
    return MGBA_LINK_STATE_CAPACITY;
}

static bool mgba_serialize_link(const void *opaque_pair,
                                void *data,
                                size_t capacity,
                                size_t *used)
{
    const struct mgba_pair *pair = (const struct mgba_pair *)opaque_pair;
    uint8_t *bytes = (uint8_t *)data;
    void *driver_states[DUALBOY_MACHINE_COUNT] = {NULL, NULL};
    size_t driver_sizes[DUALBOY_MACHINE_COUNT] = {0U, 0U};
    size_t required = MGBA_LINK_STATE_HEADER_SIZE;
    unsigned index;

    if (used != NULL) {
        *used = 0U;
    }
    if (pair == NULL || data == NULL || used == NULL ||
        capacity < MGBA_LINK_STATE_HEADER_SIZE) {
        return false;
    }
    if (pair->link_enabled) {
        if (!pair->coordinator_initialized) {
            return false;
        }
        for (index = 0U; index < DUALBOY_MACHINE_COUNT; ++index) {
            pair->drivers[index].d.saveState(
                (struct GBASIODriver *)&pair->drivers[index].d,
                &driver_states[index],
                &driver_sizes[index]);
            if (driver_states[index] == NULL ||
                driver_sizes[index] != MGBA_LOCKSTEP_DRIVER_STATE_SIZE) {
                free(driver_states[0]);
                free(driver_states[1]);
                return false;
            }
            required += driver_sizes[index];
        }
    }
    if (capacity < required) {
        free(driver_states[0]);
        free(driver_states[1]);
        return false;
    }
    memset(bytes, 0, required);
    bytes[0] = (uint8_t)'D';
    bytes[1] = (uint8_t)'B';
    bytes[2] = (uint8_t)'G';
    bytes[3] = (uint8_t)'L';
    put_u16_le(bytes + 4U, MGBA_LINK_STATE_VERSION);
    put_u16_le(bytes + 6U, MGBA_LINK_STATE_HEADER_SIZE);
    put_u32_le(bytes + 8U,
               pair->link_enabled ? MGBA_LINK_STATE_FLAG_ENABLED : 0U);
    put_u32_le(bytes + 12U,
               pair->link_enabled ? DUALBOY_MACHINE_COUNT : 0U);
    put_u32_le(bytes + 16U, (uint32_t)driver_sizes[0]);
    put_u32_le(bytes + 20U, (uint32_t)driver_sizes[1]);
    if (pair->link_enabled) {
        memcpy(bytes + MGBA_LINK_STATE_HEADER_SIZE,
               driver_states[0],
               driver_sizes[0]);
        memcpy(bytes + MGBA_LINK_STATE_HEADER_SIZE + driver_sizes[0],
               driver_states[1],
               driver_sizes[1]);
    }
    free(driver_states[0]);
    free(driver_states[1]);
    *used = required;
    return true;
}

static bool lockstep_payload_valid(const struct mgba_pair *pair,
                                   const uint8_t *payload,
                                   size_t size,
                                   unsigned player)
{
    if (pair == NULL || payload == NULL ||
        player >= DUALBOY_MACHINE_COUNT ||
        size != MGBA_LOCKSTEP_DRIVER_STATE_SIZE) {
        return false;
    }
    return DualBoyGBASIOLockstepDriverValidateState(&pair->drivers[player],
                                                    payload,
                                                    size);
}

static bool mgba_unserialize_link(void *opaque_pair,
                                  const void *data,
                                  size_t size)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;
    const uint8_t *bytes = (const uint8_t *)data;
    const uint8_t *payload;
    uint32_t flags;
    uint32_t length0;
    uint32_t length1;
    bool enabled;
    unsigned index;

    if (pair == NULL || data == NULL ||
        size < MGBA_LINK_STATE_HEADER_SIZE ||
        bytes[0] != (uint8_t)'D' || bytes[1] != (uint8_t)'B' ||
        bytes[2] != (uint8_t)'G' || bytes[3] != (uint8_t)'L' ||
        get_u16_le(bytes + 4U) != MGBA_LINK_STATE_VERSION ||
        get_u16_le(bytes + 6U) != MGBA_LINK_STATE_HEADER_SIZE ||
        get_u32_le(bytes + 24U) != 0U ||
        get_u32_le(bytes + 28U) != 0U) {
        return false;
    }
    flags = get_u32_le(bytes + 8U);
    if ((flags & ~MGBA_LINK_STATE_FLAG_ENABLED) != 0U) {
        return false;
    }
    enabled = (flags & MGBA_LINK_STATE_FLAG_ENABLED) != 0U;
    length0 = get_u32_le(bytes + 16U);
    length1 = get_u32_le(bytes + 20U);
    if (!enabled) {
        return !pair->link_enabled && get_u32_le(bytes + 12U) == 0U &&
               length0 == 0U && length1 == 0U &&
               size == MGBA_LINK_STATE_HEADER_SIZE;
    }
    if (!pair->link_enabled || !pair->coordinator_initialized ||
        get_u32_le(bytes + 12U) != DUALBOY_MACHINE_COUNT ||
        length0 != MGBA_LOCKSTEP_DRIVER_STATE_SIZE ||
        length1 != MGBA_LOCKSTEP_DRIVER_STATE_SIZE ||
        size != MGBA_LINK_STATE_HEADER_SIZE + (size_t)length0 +
                    (size_t)length1) {
        return false;
    }
    payload = bytes + MGBA_LINK_STATE_HEADER_SIZE;
    if (!lockstep_payload_valid(pair, payload, length0, 0U) ||
        !lockstep_payload_valid(pair, payload + length0, length1, 1U)) {
        return false;
    }

    /* Player 0 owns coordinator state, so restore the secondary first. */
    for (index = DUALBOY_MACHINE_COUNT; index > 0U; --index) {
        unsigned player = index - 1U;
        const uint8_t *player_payload =
            payload + (player == 0U ? 0U : (size_t)length0);
        size_t player_size = player == 0U ? (size_t)length0 : (size_t)length1;

        if (!pair->drivers[player].d.loadState(&pair->drivers[player].d,
                                               player_payload,
                                               player_size)) {
            return false;
        }
    }
    return true;
}

static void mgba_destroy_pair(void *opaque_pair)
{
    struct mgba_pair *pair = (struct mgba_pair *)opaque_pair;

    if (pair == NULL) {
        return;
    }
    detach_link(pair);
    destroy_machine(&pair->machine[1]);
    destroy_machine(&pair->machine[0]);
    if (pair->resampler_initialized) {
        mAudioResamplerDeinit(&pair->resampler);
        pair->resampler_initialized = false;
    }
    if (pair->audio_buffer_initialized) {
        mAudioBufferDeinit(&pair->resampled_audio);
        pair->audio_buffer_initialized = false;
    }
    free(pair->system_directory);
    pair->system_directory = NULL;
    free(pair);
}

bool dualboy_mgba_get_lockstep_diagnostics(
    const void *opaque_pair,
    struct dualboy_mgba_lockstep_diagnostics *diagnostics)
{
    const struct mgba_pair *pair = (const struct mgba_pair *)opaque_pair;

    if (pair == NULL || diagnostics == NULL) {
        return false;
    }
    diagnostics->max_queue_depth = pair->coordinator.maxQueueDepth;
    diagnostics->dropped_events = pair->coordinator.droppedEvents;
    return true;
}

const struct dualboy_engine_ops *dualboy_mgba_engine(void)
{
    static const struct dualboy_engine_ops operations = {
        .name = "mGBA",
        .family = DUALBOY_ENGINE_MGBA,
        .create_pair = mgba_create_pair,
        .load_rom = mgba_load_rom,
        .set_link = mgba_set_link,
        .reset = mgba_reset,
        .set_input = mgba_set_input,
        .run_frame = mgba_run_frame,
        .video_frame = mgba_video_frame,
        .audio_sample_rate = mgba_audio_sample_rate,
        .read_audio = mgba_read_audio,
        .memory_info = mgba_memory_info,
        .persistent_memory_extent = mgba_persistent_memory_extent,
        .memory_dirty = mgba_memory_dirty,
        .clear_memory_dirty = mgba_clear_memory_dirty,
        .machine_state_size = mgba_machine_state_size,
        .serialize_machine = mgba_serialize_machine,
        .unserialize_machine = mgba_unserialize_machine,
        .link_state_size = mgba_link_state_size,
        .serialize_link = mgba_serialize_link,
        .unserialize_link = mgba_unserialize_link,
        .destroy_pair = mgba_destroy_pair,
    };

    return &operations;
}
