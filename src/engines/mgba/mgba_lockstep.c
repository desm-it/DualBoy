/* Adapted from libretro/mgba PR #318 at fa743c965939f091350df094f57e639933bc17e3. */
/* Copyright (c) 2013-2024 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra-semi"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wsign-conversion"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "mgba_lockstep.h"

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

#define DRIVER_ID 0x6B636F4C
#define DRIVER_STATE_VERSION 1
#define LOCKSTEP_INTERVAL 4096
#define UNLOCKED_INTERVAL 4096
#define HARD_SYNC_INTERVAL 0x80000
#define TARGET(P) (1 << (P))
#define TARGET_ALL 0xF
#define TARGET_PRIMARY 0x1
#define TARGET_SECONDARY ((TARGET_ALL) & ~(TARGET_PRIMARY))

DECL_BITFIELD(DualBoyGBASIOLockstepSerializedFlags, uint32_t);
DECL_BITS(DualBoyGBASIOLockstepSerializedFlags, DriverMode, 0, 3);
DECL_BITS(DualBoyGBASIOLockstepSerializedFlags, NumEvents, 3, 4);
DECL_BIT(DualBoyGBASIOLockstepSerializedFlags, Asleep, 7);
DECL_BIT(DualBoyGBASIOLockstepSerializedFlags, DataReceived, 8);
DECL_BIT(DualBoyGBASIOLockstepSerializedFlags, EventScheduled, 9);
DECL_BITS(DualBoyGBASIOLockstepSerializedFlags, Player0Mode, 10, 3);
DECL_BITS(DualBoyGBASIOLockstepSerializedFlags, Player1Mode, 13, 3);
DECL_BITS(DualBoyGBASIOLockstepSerializedFlags, Player2Mode, 16, 3);
DECL_BITS(DualBoyGBASIOLockstepSerializedFlags, Player3Mode, 19, 3);
DECL_BIT(DualBoyGBASIOLockstepSerializedFlags, SyncArmed, 22);
DECL_BITS(DualBoyGBASIOLockstepSerializedFlags, TransferMode, 28, 3);
/* mGBA's generic DECL_BIT macro shifts a signed int. Bit 31 therefore trips
 * UBSan even though the resulting wire bit pattern is intended to be uint32. */
static inline DualBoyGBASIOLockstepSerializedFlags
DualBoyGBASIOLockstepSerializedFlagsGetTransferActive(
    DualBoyGBASIOLockstepSerializedFlags source)
{
	return (source >> 31U) & UINT32_C(1);
}

static inline DualBoyGBASIOLockstepSerializedFlags
DualBoyGBASIOLockstepSerializedFlagsSetTransferActive(
    DualBoyGBASIOLockstepSerializedFlags source,
    DualBoyGBASIOLockstepSerializedFlags bits)
{
	return (source & UINT32_C(0x7fffffff)) |
	       ((bits & UINT32_C(1)) << 31U);
}

DECL_BITFIELD(DualBoyGBASIOLockstepSerializedEventFlags, uint32_t);
DECL_BITS(DualBoyGBASIOLockstepSerializedEventFlags, Type, 0, 3);

struct DualBoyGBASIOLockstepSerializedEvent {
	int32_t timestamp;
	int32_t playerId;
	DualBoyGBASIOLockstepSerializedEventFlags flags;
	int32_t reserved[5];
	union {
		int32_t mode;
		int32_t finishCycle;
		int32_t padding[4];
	};
};
static_assert(sizeof(struct DualBoyGBASIOLockstepSerializedEvent) == 0x30, "GBA lockstep event savestate struct sized wrong");

struct DualBoyGBASIOLockstepSerializedState {
	uint32_t version;
	DualBoyGBASIOLockstepSerializedFlags flags;
	uint32_t reserved[2];

	struct {
		int32_t nextEvent;
		uint32_t reservedDriver[7];
	} driver;

	struct {
		int32_t playerId;
		int32_t cycleOffset;
		uint32_t reservedPlayer[2];
		struct DualBoyGBASIOLockstepSerializedEvent events[DUALBOY_MAX_LOCKSTEP_EVENTS];
	} player;

	// playerId 0 only
	struct {
		int32_t cycle;
		uint32_t waiting;
		int32_t nextHardSync;
		uint32_t reservedCoordinator[3];
		uint16_t multiData[4];
		uint32_t normalData[4];
	} coordinator;
};
static_assert(offsetof(struct DualBoyGBASIOLockstepSerializedState, driver) == 0x10, "GBA lockstep savestate driver offset wrong");
static_assert(offsetof(struct DualBoyGBASIOLockstepSerializedState, player) == 0x30, "GBA lockstep savestate player offset wrong");
static_assert(offsetof(struct DualBoyGBASIOLockstepSerializedState, coordinator) == 0x1C0, "GBA lockstep savestate coordinator offset wrong");
static_assert(sizeof(struct DualBoyGBASIOLockstepSerializedState) == 0x1F0, "GBA lockstep savestate struct sized wrong");

static bool DualBoyGBASIOLockstepDriverInit(struct GBASIODriver* driver);
static void DualBoyGBASIOLockstepDriverDeinit(struct GBASIODriver* driver);
static void DualBoyGBASIOLockstepDriverReset(struct GBASIODriver* driver);
static uint32_t DualBoyGBASIOLockstepDriverId(const struct GBASIODriver* driver);
static bool DualBoyGBASIOLockstepDriverLoadState(struct GBASIODriver* driver, const void* state, size_t size);
static void DualBoyGBASIOLockstepDriverSaveState(struct GBASIODriver* driver, void** state, size_t* size);
static void DualBoyGBASIOLockstepDriverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static bool DualBoyGBASIOLockstepDriverHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static int DualBoyGBASIOLockstepDriverConnectedDevices(struct GBASIODriver* driver);
static int DualBoyGBASIOLockstepDriverDeviceId(struct GBASIODriver* driver);
static uint16_t DualBoyGBASIOLockstepDriverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value);
static uint16_t DualBoyGBASIOLockstepDriverWriteRCNT(struct GBASIODriver* driver, uint16_t value);
static bool DualBoyGBASIOLockstepDriverStart(struct GBASIODriver* driver);
static void DualBoyGBASIOLockstepDriverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]);
static uint8_t DualBoyGBASIOLockstepDriverFinishNormal8(struct GBASIODriver* driver);
static uint32_t DualBoyGBASIOLockstepDriverFinishNormal32(struct GBASIODriver* driver);

static void DualBoyGBASIOLockstepCoordinatorWaitOnPlayers(struct DualBoyGBASIOLockstepCoordinator*, struct DualBoyGBASIOLockstepPlayer*);
static void DualBoyGBASIOLockstepCoordinatorAckPlayer(struct DualBoyGBASIOLockstepCoordinator*, struct DualBoyGBASIOLockstepPlayer*);
static void DualBoyGBASIOLockstepCoordinatorWakePlayers(struct DualBoyGBASIOLockstepCoordinator*);

static int32_t DualBoyGBASIOLockstepTime(struct DualBoyGBASIOLockstepPlayer*);
static void DualBoyGBASIOLockstepPlayerWake(struct DualBoyGBASIOLockstepPlayer*);
static void DualBoyGBASIOLockstepPlayerSleep(struct DualBoyGBASIOLockstepPlayer*);

static void _advanceCycle(struct DualBoyGBASIOLockstepCoordinator*, struct DualBoyGBASIOLockstepPlayer*);
static void _removePlayer(struct DualBoyGBASIOLockstepCoordinator*, struct DualBoyGBASIOLockstepPlayer*);
static void _reconfigPlayers(struct DualBoyGBASIOLockstepCoordinator*);
static int32_t _untilNextSync(struct DualBoyGBASIOLockstepCoordinator*, struct DualBoyGBASIOLockstepPlayer*);
static void _enqueueEvent(struct DualBoyGBASIOLockstepCoordinator*, const struct DualBoyGBASIOLockstepEvent*, uint32_t target);
static void _setData(struct DualBoyGBASIOLockstepCoordinator*, uint32_t id, struct GBASIO* sio);
static void _setReady(struct DualBoyGBASIOLockstepCoordinator*, struct DualBoyGBASIOLockstepPlayer* activePlayer, int playerId, enum GBASIOMode mode);
static void _hardSync(struct DualBoyGBASIOLockstepCoordinator*, struct DualBoyGBASIOLockstepPlayer*);
static int32_t _sanitizeDelay(int32_t delay, const char* source, int playerId);
static int32_t _cycleFromBits(uint32_t value);
static int32_t _cycleDelta(int32_t later, int32_t earlier);
static int32_t _cycleAdd(int32_t cycle, int32_t offset);
static int32_t _delayAddSaturated(int32_t delay, int32_t adjustment);
static int32_t _delayAfterLateness(int32_t delay, uint32_t cyclesLate);

static void _lockstepEvent(struct mTiming*, void* context, uint32_t cyclesLate);

/* mGBA's emulated clock is a wrapping 32-bit counter. Keep timestamp
 * arithmetic in uint32_t, then interpret the modular difference using the
 * usual half-range ordering rule. This avoids signed-overflow UB at the
 * INT32 sign boundary while preserving the serialized bit representation. */
static int32_t _cycleFromBits(uint32_t value) {
	if (value <= (uint32_t) INT32_MAX) {
		return (int32_t) value;
	}
	return -1 - (int32_t) (UINT32_MAX - value);
}

static int32_t _cycleDelta(int32_t later, int32_t earlier) {
	return _cycleFromBits((uint32_t) later - (uint32_t) earlier);
}

static int32_t _cycleAdd(int32_t cycle, int32_t offset) {
	return _cycleFromBits((uint32_t) cycle + (uint32_t) offset);
}

static int32_t _delayAddSaturated(int32_t delay, int32_t adjustment) {
	int64_t result = (int64_t) delay + (int64_t) adjustment;
	if (result > INT32_MAX) {
		return INT32_MAX;
	}
	if (result < INT32_MIN) {
		return INT32_MIN;
	}
	return (int32_t) result;
}

static int32_t _delayAfterLateness(int32_t delay, uint32_t cyclesLate) {
	if (delay <= 0 || cyclesLate >= (uint32_t) delay) {
		return 0;
	}
	return delay - (int32_t) cyclesLate;
}

static int32_t _sanitizeDelay(int32_t delay, const char* source, int playerId) {
	if (delay > 0) {
		return delay;
	}
	mLOG(GBA_SIO, DEBUG, "%s produced delay %d for player %d; clamping to 1",
	                      source, delay, playerId);
	return 1;
}

static void _verifyAwake(struct DualBoyGBASIOLockstepCoordinator* coordinator) {
#ifdef NDEBUG
	UNUSED(coordinator);
#else
	int i;
	int asleep = 0;
	for (i = 0; i < coordinator->nAttached; ++i) {
		if (!coordinator->attachedPlayers[i]) {
			continue;
		}
		struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		asleep += player->asleep;
	}
	mASSERT_DEBUG(!asleep || asleep < coordinator->nAttached);
#endif
}

void DualBoyGBASIOLockstepDriverCreate(struct DualBoyGBASIOLockstepDriver* driver, struct mLockstepUser* user) {
	memset(driver, 0, sizeof(*driver));
	driver->d.init = DualBoyGBASIOLockstepDriverInit;
	driver->d.deinit = DualBoyGBASIOLockstepDriverDeinit;
	driver->d.reset = DualBoyGBASIOLockstepDriverReset;
	driver->d.driverId = DualBoyGBASIOLockstepDriverId;
	driver->d.loadState = DualBoyGBASIOLockstepDriverLoadState;
	driver->d.saveState = DualBoyGBASIOLockstepDriverSaveState;
	driver->d.setMode = DualBoyGBASIOLockstepDriverSetMode;
	driver->d.handlesMode = DualBoyGBASIOLockstepDriverHandlesMode;
	driver->d.deviceId = DualBoyGBASIOLockstepDriverDeviceId;
	driver->d.connectedDevices = DualBoyGBASIOLockstepDriverConnectedDevices;
	driver->d.writeSIOCNT = DualBoyGBASIOLockstepDriverWriteSIOCNT;
	driver->d.writeRCNT = DualBoyGBASIOLockstepDriverWriteRCNT;
	driver->d.start = DualBoyGBASIOLockstepDriverStart;
	driver->d.finishMultiplayer = DualBoyGBASIOLockstepDriverFinishMultiplayer;
	driver->d.finishNormal8 = DualBoyGBASIOLockstepDriverFinishNormal8;
	driver->d.finishNormal32 = DualBoyGBASIOLockstepDriverFinishNormal32;
	driver->event.context = driver;
	driver->event.callback = _lockstepEvent;
	driver->event.name = "GBA SIO Lockstep";
	driver->event.priority = 0x80;
	driver->user = user;
}

static bool DualBoyGBASIOLockstepDriverInit(struct GBASIODriver* driver) {
	DualBoyGBASIOLockstepDriverReset(driver);
	return true;
}

static void DualBoyGBASIOLockstepDriverDeinit(struct GBASIODriver* driver) {
	struct DualBoyGBASIOLockstepDriver* lockstep = (struct DualBoyGBASIOLockstepDriver*) driver;
	struct DualBoyGBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (player) {
		_removePlayer(coordinator, player);
	}
	mTimingDeschedule(&lockstep->d.p->p->timing, &lockstep->event);
	lockstep->lockstepId = 0;
}

static void DualBoyGBASIOLockstepDriverReset(struct GBASIODriver* driver) {
	struct DualBoyGBASIOLockstepDriver* lockstep = (struct DualBoyGBASIOLockstepDriver*) driver;
	struct DualBoyGBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	coordinator->syncArmed = false;
	struct DualBoyGBASIOLockstepPlayer* player;
	if (!lockstep->lockstepId) {
		unsigned id;
		player = calloc(1, sizeof(*player));
		if (!player) {
			return;
		}
		player->driver = lockstep;
		player->mode = driver->p->mode;
		player->playerId = -1;

		int i;
		for (i = 0; i < DUALBOY_MAX_LOCKSTEP_EVENTS - 1; ++i) {
			player->buffer[i].next = &player->buffer[i + 1];
		}
		player->buffer[DUALBOY_MAX_LOCKSTEP_EVENTS - 1].next = NULL;
		player->freeList = &player->buffer[0];

		while (true) {
			if (coordinator->nextId == UINT_MAX) {
				coordinator->nextId = 0;
			}
			++coordinator->nextId;
			id = coordinator->nextId;
			if (!TableLookup(&coordinator->players, id)) {
				TableInsert(&coordinator->players, id, player);
				lockstep->lockstepId = id;
				break;
			}
		}
		_reconfigPlayers(coordinator);
		player->cycleOffset = _cycleDelta(mTimingCurrentTime(&driver->p->p->timing), coordinator->cycle);
		if (player->playerId != 0) {
			struct DualBoyGBASIOLockstepEvent event = {
				.type = DUALBOY_SIO_EV_ATTACH,
				.playerId = player->playerId,
				.timestamp = DualBoyGBASIOLockstepTime(player),
			};
			_enqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));
		}
	} else {
		player = TableLookup(&coordinator->players, lockstep->lockstepId);
		player->cycleOffset = _cycleDelta(mTimingCurrentTime(&driver->p->p->timing), coordinator->cycle);
	}

	if (mTimingIsScheduled(&lockstep->d.p->p->timing, &lockstep->event)) {
		return;
	}

	int32_t nextEvent;
	_setReady(coordinator, player, player->playerId, player->mode);
	if (TableSize(&coordinator->players) == 1) {
		coordinator->cycle = mTimingCurrentTime(&lockstep->d.p->p->timing);
		nextEvent = LOCKSTEP_INTERVAL;
	} else {
		_setReady(coordinator, player, 0, coordinator->transferMode);
		nextEvent = _untilNextSync(lockstep->coordinator, player);
	}
	nextEvent = _sanitizeDelay(nextEvent, "reset schedule", player->playerId);
	mTimingSchedule(&lockstep->d.p->p->timing, &lockstep->event, nextEvent);
}

static uint32_t DualBoyGBASIOLockstepDriverId(const struct GBASIODriver* driver) {
	UNUSED(driver);
	return DRIVER_ID;
}

static unsigned _modeEnumToInt(enum GBASIOMode mode) {
	switch ((int) mode) {
	case -1:
	default:
		return 0;
	case GBA_SIO_MULTI:
		return 1;
	case GBA_SIO_NORMAL_8:
		return 2;
	case GBA_SIO_NORMAL_32:
		return 3;
	case GBA_SIO_GPIO:
		return 4;
	case GBA_SIO_UART:
		return 5;
	case GBA_SIO_JOYBUS:
		return 6;
	}
}

static enum GBASIOMode _modeIntToEnum(unsigned mode) {
	const enum GBASIOMode modes[8] = {
		-1, GBA_SIO_MULTI, GBA_SIO_NORMAL_8, GBA_SIO_NORMAL_32, GBA_SIO_GPIO, GBA_SIO_UART, GBA_SIO_JOYBUS, -1
	};
	return modes[mode & 7];
}

static bool _allZero(const void* data, size_t size) {
	const uint8_t* bytes = data;
	size_t i;
	for (i = 0; i < size; ++i) {
		if (bytes[i]) {
			return false;
		}
	}
	return true;
}

static bool _serializedModeValid(uint32_t mode) {
	return mode <= 6;
}

static bool _eventModeValid(int32_t mode) {
	switch (mode) {
	case -1:
	case GBA_SIO_NORMAL_8:
	case GBA_SIO_NORMAL_32:
	case GBA_SIO_MULTI:
	case GBA_SIO_UART:
	case GBA_SIO_GPIO:
	case GBA_SIO_JOYBUS:
		return true;
	default:
		return false;
	}
}

bool DualBoyGBASIOLockstepDriverValidateState(
    const struct DualBoyGBASIOLockstepDriver* lockstep,
    const void* data,
    size_t size) {
	const struct DualBoyGBASIOLockstepSerializedState* state = data;
	const struct DualBoyGBASIOLockstepCoordinator* coordinator;
	const struct DualBoyGBASIOLockstepPlayer* player;
	DualBoyGBASIOLockstepSerializedFlags flags;
	uint32_t value;
	uint32_t waiting;
	uint32_t attachedMask;
	unsigned eventCount;
	unsigned i;

	if (!lockstep || !data ||
	    size != sizeof(struct DualBoyGBASIOLockstepSerializedState) ||
	    !lockstep->coordinator || !lockstep->lockstepId) {
		return false;
	}
	coordinator = lockstep->coordinator;
	if (coordinator->nAttached < 1 || coordinator->nAttached > MAX_GBAS) {
		return false;
	}
	player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (!player || player->playerId < 0 ||
	    player->playerId >= coordinator->nAttached) {
		return false;
	}

	LOAD_32LE(value, 0, &state->version);
	if (value != DRIVER_STATE_VERSION ||
	    !_allZero(state->reserved, sizeof(state->reserved)) ||
	    !_allZero(state->driver.reservedDriver,
	              sizeof(state->driver.reservedDriver)) ||
	    !_allZero(state->player.reservedPlayer,
	              sizeof(state->player.reservedPlayer))) {
		return false;
	}
	LOAD_32LE(value, 0, &state->player.playerId);
	if (value != (uint32_t) player->playerId) {
		return false;
	}
	LOAD_32LE(flags, 0, &state->flags);
	/* Bits 23 through 27 are reserved in version 1. */
	if ((flags & UINT32_C(0x0F800000)) != 0 ||
	    !_serializedModeValid(
	        DualBoyGBASIOLockstepSerializedFlagsGetDriverMode(flags)) ||
	    !_serializedModeValid(
	        DualBoyGBASIOLockstepSerializedFlagsGetPlayer0Mode(flags)) ||
	    !_serializedModeValid(
	        DualBoyGBASIOLockstepSerializedFlagsGetPlayer1Mode(flags)) ||
	    !_serializedModeValid(
	        DualBoyGBASIOLockstepSerializedFlagsGetPlayer2Mode(flags)) ||
	    !_serializedModeValid(
	        DualBoyGBASIOLockstepSerializedFlagsGetPlayer3Mode(flags))) {
		return false;
	}
	eventCount = DualBoyGBASIOLockstepSerializedFlagsGetNumEvents(flags);
	if (eventCount > DUALBOY_MAX_LOCKSTEP_EVENTS) {
		return false;
	}

	for (i = 0; i < eventCount; ++i) {
		const struct DualBoyGBASIOLockstepSerializedEvent* stateEvent =
		    &state->player.events[i];
		DualBoyGBASIOLockstepSerializedEventFlags eventFlags;
		unsigned type;
		int32_t eventPlayer;
		int32_t eventMode;

		LOAD_32LE(eventFlags, 0, &stateEvent->flags);
		if ((eventFlags & ~UINT32_C(7)) != 0 ||
		    !_allZero(stateEvent->reserved, sizeof(stateEvent->reserved)) ||
		    !_allZero(&stateEvent->padding[1],
		              sizeof(stateEvent->padding) - sizeof(stateEvent->padding[0]))) {
			return false;
		}
		type = DualBoyGBASIOLockstepSerializedEventFlagsGetType(eventFlags);
		if (type > DUALBOY_SIO_EV_TRANSFER_START) {
			return false;
		}
		LOAD_32LE(eventPlayer, 0, &stateEvent->playerId);
		switch (type) {
		case DUALBOY_SIO_EV_ATTACH:
		case DUALBOY_SIO_EV_DETACH:
		case DUALBOY_SIO_EV_MODE_SET:
			if (eventPlayer < 0 ||
			    eventPlayer >= coordinator->nAttached) {
				return false;
			}
			break;
		case DUALBOY_SIO_EV_HARD_SYNC:
		case DUALBOY_SIO_EV_TRANSFER_START:
			if (eventPlayer != 0) {
				return false;
			}
			break;
		}
		if (type == DUALBOY_SIO_EV_MODE_SET) {
			LOAD_32LE(eventMode, 0, &stateEvent->mode);
			if (!_eventModeValid(eventMode)) {
				return false;
			}
		} else if (type != DUALBOY_SIO_EV_TRANSFER_START &&
		           !_allZero(&stateEvent->padding[0],
		                     sizeof(stateEvent->padding[0]))) {
			return false;
		}
	}

	if (player->playerId == 0) {
		if (!_serializedModeValid(
		        DualBoyGBASIOLockstepSerializedFlagsGetTransferMode(flags)) ||
		    !_allZero(state->coordinator.reservedCoordinator,
		              sizeof(state->coordinator.reservedCoordinator))) {
			return false;
		}
		LOAD_32LE(waiting, 0, &state->coordinator.waiting);
		attachedMask = (UINT32_C(1) << (unsigned) coordinator->nAttached) -
		               UINT32_C(1);
		if ((waiting & ~attachedMask) != 0 || (waiting & UINT32_C(1)) != 0) {
			return false;
		}
	} else if ((flags & UINT32_C(0xF0400000)) != 0 ||
	           !_allZero(&state->coordinator, sizeof(state->coordinator))) {
		return false;
	}
	return true;
}

static bool DualBoyGBASIOLockstepDriverLoadState(struct GBASIODriver* driver, const void* data, size_t size) {
	struct DualBoyGBASIOLockstepDriver* lockstep = (struct DualBoyGBASIOLockstepDriver*) driver;
	struct DualBoyGBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	if (!DualBoyGBASIOLockstepDriverValidateState(lockstep, data, size)) {
		mLOG(GBA_SIO, WARN, "Invalid lockstep driver state");
		return false;
	}
	const struct DualBoyGBASIOLockstepSerializedState* state = data;
	bool error = false;
	uint32_t ucheck;
	int32_t check;
	LOAD_32LE(ucheck, 0, &state->version);
	if (ucheck > DRIVER_STATE_VERSION) {
		mLOG(GBA_SIO, WARN, "Invalid or too new save state: expected %u, got %u", DRIVER_STATE_VERSION, ucheck);
		return false;
	}

	struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	LOAD_32LE(check, 0, &state->player.playerId);
	if (check != player->playerId) {
		mLOG(GBA_SIO, WARN, "State is for different player: expected %d, got %d", player->playerId, check);
		error = true;
		goto out;
	}

	DualBoyGBASIOLockstepSerializedFlags flags = 0;
	LOAD_32LE(flags, 0, &state->flags);
	LOAD_32LE(player->cycleOffset, 0, &state->player.cycleOffset);
	player->dataReceived = DualBoyGBASIOLockstepSerializedFlagsGetDataReceived(flags);
	player->mode = _modeIntToEnum(DualBoyGBASIOLockstepSerializedFlagsGetDriverMode(flags));

	player->otherModes[0] = _modeIntToEnum(DualBoyGBASIOLockstepSerializedFlagsGetPlayer0Mode(flags));
	player->otherModes[1] = _modeIntToEnum(DualBoyGBASIOLockstepSerializedFlagsGetPlayer1Mode(flags));
	player->otherModes[2] = _modeIntToEnum(DualBoyGBASIOLockstepSerializedFlagsGetPlayer2Mode(flags));
	player->otherModes[3] = _modeIntToEnum(DualBoyGBASIOLockstepSerializedFlagsGetPlayer3Mode(flags));

	mTimingDeschedule(&driver->p->p->timing, &lockstep->event);

	if (DualBoyGBASIOLockstepSerializedFlagsGetEventScheduled(flags)) {
		int32_t when;
		LOAD_32LE(when, 0, &state->driver.nextEvent);
		when = _sanitizeDelay(when, "load state schedule", player->playerId);
		mTimingSchedule(&driver->p->p->timing, &lockstep->event, when);
	}

	if (DualBoyGBASIOLockstepSerializedFlagsGetAsleep(flags)) {
		if (!player->asleep && player->driver->user->sleep) {
			player->driver->user->sleep(player->driver->user);
		}
		player->asleep = true;
	} else {
		if (player->asleep && player->driver->user->wake) {
			player->driver->user->wake(player->driver->user);
		}
		player->asleep = false;
	}

	unsigned i;
	for (i = 0; i < DUALBOY_MAX_LOCKSTEP_EVENTS - 1; ++i) {
		player->buffer[i].next = &player->buffer[i + 1];
	}
	player->buffer[DUALBOY_MAX_LOCKSTEP_EVENTS - 1].next = NULL;
	player->freeList = &player->buffer[0];
	player->queue = NULL;

	struct DualBoyGBASIOLockstepEvent** lastEvent = &player->queue;
	for (i = 0; i < DualBoyGBASIOLockstepSerializedFlagsGetNumEvents(flags) && i < DUALBOY_MAX_LOCKSTEP_EVENTS; ++i) {
		struct DualBoyGBASIOLockstepEvent* event = player->freeList;
		const struct DualBoyGBASIOLockstepSerializedEvent* stateEvent = &state->player.events[i];
		player->freeList = player->freeList->next;
		*lastEvent = event;
		lastEvent = &event->next;
		event->next = NULL;

		DualBoyGBASIOLockstepSerializedEventFlags flags;
		LOAD_32LE(flags, 0, &stateEvent->flags);
		LOAD_32LE(event->timestamp, 0, &stateEvent->timestamp);
		LOAD_32LE(event->playerId, 0, &stateEvent->playerId);
		event->type = DualBoyGBASIOLockstepSerializedEventFlagsGetType(flags);
		switch (event->type) {
		case DUALBOY_SIO_EV_ATTACH:
		case DUALBOY_SIO_EV_DETACH:
		case DUALBOY_SIO_EV_HARD_SYNC:
			break;
		case DUALBOY_SIO_EV_MODE_SET:
			LOAD_32LE(event->mode, 0, &stateEvent->mode);
			break;
		case DUALBOY_SIO_EV_TRANSFER_START:
			LOAD_32LE(event->finishCycle, 0, &stateEvent->finishCycle);
			break;
		}
	}
	*lastEvent = NULL;

	if (player->playerId == 0) {
		LOAD_32LE(coordinator->cycle, 0, &state->coordinator.cycle);
		LOAD_32LE(coordinator->waiting, 0, &state->coordinator.waiting);
		LOAD_32LE(coordinator->nextHardSync, 0, &state->coordinator.nextHardSync);
		for (i = 0; i < 4; ++i) {
			LOAD_16LE(coordinator->multiData[i], 0, &state->coordinator.multiData[i]);
			LOAD_32LE(coordinator->normalData[i], 0, &state->coordinator.normalData[i]);
		}
		coordinator->transferMode = _modeIntToEnum(DualBoyGBASIOLockstepSerializedFlagsGetTransferMode(flags));
		coordinator->transferActive = DualBoyGBASIOLockstepSerializedFlagsGetTransferActive(flags);
		coordinator->syncArmed = DualBoyGBASIOLockstepSerializedFlagsGetSyncArmed(flags);
	}
out:
	if (!error) {
		mTimingInterrupt(&driver->p->p->timing);
	}
	return !error;
}

static void DualBoyGBASIOLockstepDriverSaveState(struct GBASIODriver* driver, void** stateOut, size_t* size) {
	struct DualBoyGBASIOLockstepDriver* lockstep = (struct DualBoyGBASIOLockstepDriver*) driver;
	struct DualBoyGBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	if (!stateOut || !size) {
		return;
	}
	*stateOut = NULL;
	*size = 0;
	struct DualBoyGBASIOLockstepSerializedState* state = calloc(1, sizeof(*state));
	if (!state) {
		return;
	}

	STORE_32LE(DRIVER_STATE_VERSION, 0, &state->version);

	STORE_32LE(_cycleFromBits(lockstep->event.when -
	                         (uint32_t) mTimingCurrentTime(&driver->p->p->timing)),
	           0, &state->driver.nextEvent);

	struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (!player) {
		free(state);
		return;
	}
	DualBoyGBASIOLockstepSerializedFlags flags = 0;
	STORE_32LE(player->playerId, 0, &state->player.playerId);
	STORE_32LE(player->cycleOffset, 0, &state->player.cycleOffset);
	flags = DualBoyGBASIOLockstepSerializedFlagsSetAsleep(flags, player->asleep);
	flags = DualBoyGBASIOLockstepSerializedFlagsSetDataReceived(flags, player->dataReceived);
	flags = DualBoyGBASIOLockstepSerializedFlagsSetDriverMode(flags, _modeEnumToInt(player->mode));
	flags = DualBoyGBASIOLockstepSerializedFlagsSetEventScheduled(flags, mTimingIsScheduled(&driver->p->p->timing, &lockstep->event));

	flags = DualBoyGBASIOLockstepSerializedFlagsSetPlayer0Mode(flags, _modeEnumToInt(player->otherModes[0]));
	flags = DualBoyGBASIOLockstepSerializedFlagsSetPlayer1Mode(flags, _modeEnumToInt(player->otherModes[1]));
	flags = DualBoyGBASIOLockstepSerializedFlagsSetPlayer2Mode(flags, _modeEnumToInt(player->otherModes[2]));
	flags = DualBoyGBASIOLockstepSerializedFlagsSetPlayer3Mode(flags, _modeEnumToInt(player->otherModes[3]));

	struct DualBoyGBASIOLockstepEvent* event = player->queue;
	size_t i;
	for (i = 0; i < DUALBOY_MAX_LOCKSTEP_EVENTS && event; ++i, event = event->next) {
		struct DualBoyGBASIOLockstepSerializedEvent* stateEvent = &state->player.events[i];
		DualBoyGBASIOLockstepSerializedEventFlags flags = DualBoyGBASIOLockstepSerializedEventFlagsSetType(0, event->type);
		STORE_32LE(event->timestamp, 0, &stateEvent->timestamp);
		STORE_32LE(event->playerId, 0, &stateEvent->playerId);
		switch (event->type) {
		case DUALBOY_SIO_EV_ATTACH:
		case DUALBOY_SIO_EV_DETACH:
		case DUALBOY_SIO_EV_HARD_SYNC:
			break;
		case DUALBOY_SIO_EV_MODE_SET:
			STORE_32LE(event->mode, 0, &stateEvent->mode);
			break;
		case DUALBOY_SIO_EV_TRANSFER_START:
			STORE_32LE(event->finishCycle, 0, &stateEvent->finishCycle);
			break;
		}
		STORE_32LE(flags, 0, &stateEvent->flags);
	}
	flags = DualBoyGBASIOLockstepSerializedFlagsSetNumEvents(flags, i);

	if (player->playerId == 0) {
		STORE_32LE(coordinator->cycle, 0, &state->coordinator.cycle);
		STORE_32LE(coordinator->waiting, 0, &state->coordinator.waiting);
		STORE_32LE(coordinator->nextHardSync, 0, &state->coordinator.nextHardSync);
		for (i = 0; i < 4; ++i) {
			STORE_16LE(coordinator->multiData[i], 0, &state->coordinator.multiData[i]);
			STORE_32LE(coordinator->normalData[i], 0, &state->coordinator.normalData[i]);
		}
		flags = DualBoyGBASIOLockstepSerializedFlagsSetTransferMode(flags, _modeEnumToInt(coordinator->transferMode));
		flags = DualBoyGBASIOLockstepSerializedFlagsSetTransferActive(flags, coordinator->transferActive);
		flags = DualBoyGBASIOLockstepSerializedFlagsSetSyncArmed(flags, coordinator->syncArmed);
	}
	STORE_32LE(flags, 0, &state->flags);
	*stateOut = state;
	*size = sizeof(*state);
}

static void DualBoyGBASIOLockstepDriverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	struct DualBoyGBASIOLockstepDriver* lockstep = (struct DualBoyGBASIOLockstepDriver*) driver;
	/* GBAIODeserialize switches the local SIO mode while an individual
	 * machine is being restored. The peer machine and shared coordinator
	 * still have their live clocks at that point; mutating or waiting on the
	 * coordinator would mix two timelines. The serialized link state is
	 * restored after both machine states, so keep this callback observational
	 * during that narrow window. The attached driver remains available for
	 * deviceId(), which lets mGBA restore the correct multiplayer ID. */
	if (lockstep->loadingMachineState) {
		return;
	}
	struct DualBoyGBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	bool waitOnPlayers = false;
	if (mode != player->mode) {
		player->mode = mode;
		struct DualBoyGBASIOLockstepEvent event = {
			.type = DUALBOY_SIO_EV_MODE_SET,
			.playerId = player->playerId,
			.timestamp = DualBoyGBASIOLockstepTime(player),
			.mode = mode,
		};
		if (player->playerId == 0) {
			mASSERT_DEBUG(!coordinator->transferActive); // TODO
			coordinator->transferMode = mode;
			waitOnPlayers = !coordinator->waiting;
		}
		_setReady(coordinator, player, player->playerId, mode);
		_enqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));
		if (waitOnPlayers) {
			DualBoyGBASIOLockstepCoordinatorWaitOnPlayers(coordinator, player);
		} else if (player->playerId == 0) {
			mLOG(GBA_SIO, DEBUG, "Deferring mode wait while barrier %X is active", coordinator->waiting);
		}
	}
}

static bool DualBoyGBASIOLockstepDriverHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	UNUSED(mode);
	return true;
}

static int DualBoyGBASIOLockstepDriverConnectedDevices(struct GBASIODriver* driver) {
	struct DualBoyGBASIOLockstepDriver* lockstep = (struct DualBoyGBASIOLockstepDriver*) driver;
	struct DualBoyGBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	if (!lockstep->lockstepId) {
		return 0;
	}
	int attached = coordinator->nAttached - 1;
	return attached;
}

static int DualBoyGBASIOLockstepDriverDeviceId(struct GBASIODriver* driver) {
	struct DualBoyGBASIOLockstepDriver* lockstep = (struct DualBoyGBASIOLockstepDriver*) driver;
	struct DualBoyGBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	int playerId = 0;
	struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (player && player->playerId >= 0) {
		playerId = player->playerId;
	}
	return playerId;
}

static uint16_t DualBoyGBASIOLockstepDriverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	UNUSED(driver);
	mLOG(GBA_SIO, DEBUG, "Lockstep: SIOCNT <- %04X", value);
	return value;
}

static uint16_t DualBoyGBASIOLockstepDriverWriteRCNT(struct GBASIODriver* driver, uint16_t value) {
	UNUSED(driver);
	mLOG(GBA_SIO, DEBUG, "Lockstep: RCNT <- %04X", value);
	return value;
}

static bool DualBoyGBASIOLockstepDriverStart(struct GBASIODriver* driver) {
	struct DualBoyGBASIOLockstepDriver* lockstep = (struct DualBoyGBASIOLockstepDriver*) driver;
	struct DualBoyGBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	bool ret = false;
	bool waitOnPlayers = true;
	if (coordinator->transferActive) {
		mLOG(GBA_SIO, ERROR, "Transfer restarted unexpectedly");
		goto out;
	}
	struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	if (player->playerId != 0) {
		mLOG(GBA_SIO, DEBUG, "Secondary player attempted to start transfer");
		goto out;
	}
	mLOG(GBA_SIO, DEBUG, "Transfer starting at %08X", coordinator->cycle);
	memset(coordinator->multiData, 0xFF, sizeof(coordinator->multiData));
	_setData(coordinator, 0, player->driver->d.p);

	int32_t timestamp = DualBoyGBASIOLockstepTime(player);
	struct DualBoyGBASIOLockstepEvent event = {
		.type = DUALBOY_SIO_EV_TRANSFER_START,
		.timestamp = timestamp,
		.finishCycle = _cycleAdd(timestamp, GBASIOTransferCycles(player->mode, player->driver->d.p->siocnt, coordinator->nAttached - 1)),
	};
	_enqueueEvent(coordinator, &event, TARGET_SECONDARY);
	coordinator->transferActive = true;
	if (player->mode == GBA_SIO_MULTI && !coordinator->syncArmed) {
		coordinator->syncArmed = true;
		coordinator->nextHardSync = HARD_SYNC_INTERVAL;
		mLOG(GBA_SIO, DEBUG, "Arming lockstep sync/watchdog after first MULTI transfer start");
	}
	if (coordinator->waiting) {
		waitOnPlayers = false;
		mLOG(GBA_SIO, DEBUG, "Deferring transfer wait while barrier %X is active", coordinator->waiting);
	}
	if (waitOnPlayers) {
		DualBoyGBASIOLockstepCoordinatorWaitOnPlayers(coordinator, player);
	}
	ret = true;
out:
	return ret;
}

static void DualBoyGBASIOLockstepDriverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
	struct DualBoyGBASIOLockstepDriver* lockstep = (struct DualBoyGBASIOLockstepDriver*) driver;
	struct DualBoyGBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	if (coordinator->transferMode == GBA_SIO_MULTI) {
		struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
		if (!player->dataReceived) {
			mLOG(GBA_SIO, WARN, "MULTI did not receive data. Are we running behind?");
			memset(data, 0xFF, sizeof(uint16_t) * 4);
		} else {
			mLOG(GBA_SIO, INFO, "MULTI transfer finished: %04X %04X %04X %04X",
			     coordinator->multiData[0],
			     coordinator->multiData[1],
			     coordinator->multiData[2],
			     coordinator->multiData[3]);
			memcpy(data, coordinator->multiData, sizeof(uint16_t) * 4);
		}
		player->dataReceived = false;
		if (player->playerId == 0) {
			if (!coordinator->syncArmed) {
				// Sync/watchdog is intentionally inert until first MULTI transfer start.
			} else if (coordinator->waiting) {
				mLOG(GBA_SIO, DEBUG, "Deferring hard sync while barrier %X is active", coordinator->waiting);
				coordinator->nextHardSync = -1;
			} else {
				_hardSync(coordinator, player);
			}
		}
	}
}

static uint8_t DualBoyGBASIOLockstepDriverFinishNormal8(struct GBASIODriver* driver) {
	struct DualBoyGBASIOLockstepDriver* lockstep = (struct DualBoyGBASIOLockstepDriver*) driver;
	struct DualBoyGBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	uint8_t data = 0xFF;
	if (coordinator->transferMode == GBA_SIO_NORMAL_8) {
		struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
		if (player->playerId > 0) {
			if (!player->dataReceived) {
				mLOG(GBA_SIO, WARN, "NORMAL did not receive data. Are we running behind?");
			} else {
				data = coordinator->normalData[player->playerId - 1];
				mLOG(GBA_SIO, INFO, "NORMAL8 transfer finished: %02X", data);
			}
		}
		player->dataReceived = false;
		if (player->playerId == 0) {
			if (!coordinator->syncArmed) {
				// Sync/watchdog is intentionally inert until first MULTI transfer start.
			} else if (coordinator->waiting) {
				mLOG(GBA_SIO, DEBUG, "Deferring hard sync while barrier %X is active", coordinator->waiting);
				coordinator->nextHardSync = -1;
			} else {
				_hardSync(coordinator, player);
			}
		}
	}
	return data;
}

static uint32_t DualBoyGBASIOLockstepDriverFinishNormal32(struct GBASIODriver* driver) {
	struct DualBoyGBASIOLockstepDriver* lockstep = (struct DualBoyGBASIOLockstepDriver*) driver;
	struct DualBoyGBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	uint32_t data = 0xFFFFFFFF;
	if (coordinator->transferMode == GBA_SIO_NORMAL_32) {
		struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
		if (player->playerId > 0) {
			if (!player->dataReceived) {
				mLOG(GBA_SIO, WARN, "Did not receive data. Are we running behind?");
			} else {
				data = coordinator->normalData[player->playerId - 1];
				mLOG(GBA_SIO, INFO, "NORMAL32 transfer finished: %08X", data);
			}
		}
		player->dataReceived = false;
		if (player->playerId == 0) {
			if (!coordinator->syncArmed) {
				// Sync/watchdog is intentionally inert until first MULTI transfer start.
			} else if (coordinator->waiting) {
				mLOG(GBA_SIO, DEBUG, "Deferring hard sync while barrier %X is active", coordinator->waiting);
				coordinator->nextHardSync = -1;
			} else {
				_hardSync(coordinator, player);
			}
		}
	}
	return data;
}

void DualBoyGBASIOLockstepCoordinatorInit(struct DualBoyGBASIOLockstepCoordinator* coordinator) {
	memset(coordinator, 0, sizeof(*coordinator));
	TableInit(&coordinator->players, 8, free);
}

void DualBoyGBASIOLockstepCoordinatorDeinit(struct DualBoyGBASIOLockstepCoordinator* coordinator) {
	TableDeinit(&coordinator->players);
}

void DualBoyGBASIOLockstepCoordinatorAttach(struct DualBoyGBASIOLockstepCoordinator* coordinator, struct DualBoyGBASIOLockstepDriver* driver) {
	if (driver->coordinator && driver->coordinator != coordinator) {
		// TODO
		abort();
	}
	driver->coordinator = coordinator;
}

void DualBoyGBASIOLockstepCoordinatorDetach(struct DualBoyGBASIOLockstepCoordinator* coordinator, struct DualBoyGBASIOLockstepDriver* driver) {
	if (driver->coordinator != coordinator) {
		// TODO
		abort();
		return;
	}
	struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, driver->lockstepId);
	if (player) {
		_removePlayer(coordinator, player);
	}
	driver->coordinator = NULL;
}

int32_t _untilNextSync(struct DualBoyGBASIOLockstepCoordinator* coordinator, struct DualBoyGBASIOLockstepPlayer* player) {
	if (!coordinator->syncArmed) {
		return UNLOCKED_INTERVAL;
	}
	int32_t cycle = _cycleDelta(coordinator->cycle, DualBoyGBASIOLockstepTime(player));
	if (player->playerId == 0) {
		if (coordinator->nAttached < 2) {
			cycle = _delayAddSaturated(cycle, UNLOCKED_INTERVAL);
		} else {
			cycle = _delayAddSaturated(cycle, LOCKSTEP_INTERVAL);
		}
	}
	return cycle;
}

void _advanceCycle(struct DualBoyGBASIOLockstepCoordinator* coordinator, struct DualBoyGBASIOLockstepPlayer* player) {
	int32_t newCycle = DualBoyGBASIOLockstepTime(player);
	int32_t elapsed = _cycleDelta(newCycle, coordinator->cycle);
	mASSERT_DEBUG(elapsed >= 0);
	if (elapsed < 0) {
		return;
	}
	coordinator->nextHardSync = _delayAddSaturated(coordinator->nextHardSync, -elapsed);
	coordinator->cycle = newCycle;
}

void _removePlayer(struct DualBoyGBASIOLockstepCoordinator* coordinator, struct DualBoyGBASIOLockstepPlayer* player) {
	struct DualBoyGBASIOLockstepEvent event = {
		.type = DUALBOY_SIO_EV_DETACH,
		.playerId = player->playerId,
		.timestamp = DualBoyGBASIOLockstepTime(player),
	};
	_enqueueEvent(coordinator, &event, TARGET_ALL & ~TARGET(player->playerId));

	coordinator->waiting = 0;
	coordinator->transferActive = false;
	coordinator->syncArmed = false;

	TableRemove(&coordinator->players, player->driver->lockstepId);
	_reconfigPlayers(coordinator);

	struct DualBoyGBASIOLockstepPlayer* runner = TableLookup(&coordinator->players, coordinator->attachedPlayers[0]);
	if (runner) {
		DualBoyGBASIOLockstepPlayerWake(runner);
	}
	_verifyAwake(coordinator);
}

void _reconfigPlayers(struct DualBoyGBASIOLockstepCoordinator* coordinator) {
	size_t players = TableSize(&coordinator->players);
	memset(coordinator->attachedPlayers, 0, sizeof(coordinator->attachedPlayers));
	if (players == 0) {
		mLOG(GBA_SIO, WARN, "Reconfiguring player IDs with no players attached somehow?");
	} else if (players == 1) {
		struct TableIterator iter;
		mASSERT(TableIteratorStart(&coordinator->players, &iter));
		unsigned p0 = TableIteratorGetKey(&coordinator->players, &iter);
		coordinator->attachedPlayers[0] = p0;

		struct DualBoyGBASIOLockstepPlayer* player = TableIteratorGetValue(&coordinator->players, &iter);
		coordinator->cycle = mTimingCurrentTime(&player->driver->d.p->p->timing);
		coordinator->nextHardSync = HARD_SYNC_INTERVAL;

		if (player->playerId != 0) {
			player->playerId = 0;
			if (player->driver->user->playerIdChanged) {
				player->driver->user->playerIdChanged(player->driver->user, player->playerId);
			}
		}

		if (!coordinator->transferActive) {
			coordinator->transferMode = player->mode;
		}
	} else {
		struct UIntList playerPreferences[MAX_GBAS];

		int i;
		for (i = 0; i < MAX_GBAS; ++i) {
			UIntListInit(&playerPreferences[i], 4);
		}

		// Collect the first four players' requested player IDs so we can sort through them later
		int seen = 0;
		struct TableIterator iter;
		mASSERT(TableIteratorStart(&coordinator->players, &iter));
		do {
			unsigned pid = TableIteratorGetKey(&coordinator->players, &iter);
			struct DualBoyGBASIOLockstepPlayer* player = TableIteratorGetValue(&coordinator->players, &iter);
			int requested = MAX_GBAS - 1;
			if (player->driver->user->requestedId) {
				requested = player->driver->user->requestedId(player->driver->user);
			}
			if (requested < 0) {
				continue;
			}
			if (requested >= MAX_GBAS) {
				requested = MAX_GBAS - 1;
			}

			*UIntListAppend(&playerPreferences[requested]) = pid;
			++seen;
		} while (TableIteratorNext(&coordinator->players, &iter) && seen < MAX_GBAS);

		// Now sort each requested player ID to figure out who gets which ID
		seen = 0;
		for (i = 0; i < MAX_GBAS; ++i) {
			int j;
			for (j = 0; j <= i; ++j) {
				while (UIntListSize(&playerPreferences[j]) && seen < MAX_GBAS) {
					unsigned pid = *UIntListGetPointer(&playerPreferences[j], 0);
					UIntListShift(&playerPreferences[j], 0, 1);
					struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, pid);
					if (!player) {
						mLOG(GBA_SIO, ERROR, "Player list appears to have changed unexpectedly. PID %u missing.", pid);
						continue;
					}
					coordinator->attachedPlayers[seen] = pid;
					if (player->playerId != seen) {
						player->playerId = seen;
						if (player->driver->user->playerIdChanged) {
							player->driver->user->playerIdChanged(player->driver->user, player->playerId);
						}
					}
					++seen;
				}
			}
		}

		for (i = 0; i < MAX_GBAS; ++i) {
			UIntListDeinit(&playerPreferences[i]);
		}
	}

	int nAttached = 0;
	size_t i;
	for (i = 0; i < MAX_GBAS; ++i) {
		unsigned pid = coordinator->attachedPlayers[i];
		if (!pid) {
			continue;
		}
		struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, pid);
		if (!player) {
			coordinator->attachedPlayers[i] = 0;
		} else {
			++nAttached;
		}
	}
	coordinator->nAttached = nAttached;
}

static void _setData(struct DualBoyGBASIOLockstepCoordinator* coordinator, uint32_t id, struct GBASIO* sio) {
	switch (coordinator->transferMode) {
	case GBA_SIO_MULTI:
		coordinator->multiData[id] = sio->p->memory.io[GBA_REG(SIOMLT_SEND)];
		break;
	case GBA_SIO_NORMAL_8:
		coordinator->normalData[id] = sio->p->memory.io[GBA_REG(SIODATA8)];
		break;
	case GBA_SIO_NORMAL_32:
		coordinator->normalData[id] = sio->p->memory.io[GBA_REG(SIODATA32_LO)];
		coordinator->normalData[id] |=
		    (uint32_t) sio->p->memory.io[GBA_REG(SIODATA32_HI)] << 16U;
		break;
	case GBA_SIO_UART:
	case GBA_SIO_GPIO:
	case GBA_SIO_JOYBUS:
		mLOG(GBA_SIO, ERROR, "Unsupported mode %i in lockstep", coordinator->transferMode);
		// TODO: Should we handle this or just abort?
		break;
	}
}

void _setReady(struct DualBoyGBASIOLockstepCoordinator* coordinator, struct DualBoyGBASIOLockstepPlayer* activePlayer, int playerId, enum GBASIOMode mode) {
	activePlayer->otherModes[playerId] = mode;
	bool ready = true;
	int i;
	for (i = 0; ready && i < coordinator->nAttached; ++i) {
		ready = activePlayer->otherModes[i] == activePlayer->mode;
	}
	if (activePlayer->mode == GBA_SIO_MULTI) {
		struct GBASIO* sio = activePlayer->driver->d.p;
		sio->siocnt = GBASIOMultiplayerSetReady(sio->siocnt, ready);
		sio->rcnt = GBASIORegisterRCNTSetSd(sio->rcnt, ready);
	}
}

void _hardSync(struct DualBoyGBASIOLockstepCoordinator* coordinator, struct DualBoyGBASIOLockstepPlayer* player) {
	mASSERT_DEBUG(player->playerId == 0);
	struct DualBoyGBASIOLockstepEvent event = {
		.type = DUALBOY_SIO_EV_HARD_SYNC,
		.playerId = 0,
		.timestamp = DualBoyGBASIOLockstepTime(player),
	};
	_enqueueEvent(coordinator, &event, TARGET_SECONDARY);
	DualBoyGBASIOLockstepCoordinatorWaitOnPlayers(coordinator, player);
}

void _enqueueEvent(struct DualBoyGBASIOLockstepCoordinator* coordinator, const struct DualBoyGBASIOLockstepEvent* event, uint32_t target) {
	mLOG(GBA_SIO, DEBUG, "Enqueuing event of type %X from %i for target %X at timestamp %X",
	                      event->type, event->playerId, target, event->timestamp);

	int i;
	for (i = 0; i < coordinator->nAttached; ++i) {
		if (!(target & TARGET(i))) {
			continue;
		}
		struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		mASSERT_LOG(GBA_SIO, player->freeList, "No free events");
		struct DualBoyGBASIOLockstepEvent* newEvent = player->freeList;
		player->freeList = newEvent->next;

		memcpy(newEvent, event, sizeof(*event));
		struct DualBoyGBASIOLockstepEvent** previous = &player->queue;
		struct DualBoyGBASIOLockstepEvent* next = player->queue;
		while (next) {
			int32_t until = _cycleDelta(newEvent->timestamp, next->timestamp);
			if (until < 0) {
				break;
			}
			previous = &next->next;
			next = next->next;
		}
		newEvent->next = next;
		*previous = newEvent;
	}
}

void _lockstepEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct DualBoyGBASIOLockstepDriver* lockstep = context;
	struct DualBoyGBASIOLockstepCoordinator* coordinator = lockstep->coordinator;
	struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);
	struct GBASIO* sio = player->driver->d.p;
	mASSERT(player->playerId >= 0 && player->playerId < 4);

	bool wasDetach = false;
	if (player->queue && player->queue->type == DUALBOY_SIO_EV_DETACH) {
		mLOG(GBA_SIO, DEBUG, "Player %i detached at timestamp %X, picking up the pieces",
		                      player->queue->playerId, player->queue->timestamp);
		wasDetach = true;
	}
	if (player->playerId == 0 && _cycleDelta(DualBoyGBASIOLockstepTime(player), coordinator->cycle) >= 0) {
		// We are the clock owner; advance the shared clock. However, if we just became
		// the clock owner (by the previous one disconnecting) we might be slightly
		// behind the shared clock. We should wait a bit if needed in that case.
		_advanceCycle(coordinator, player);
		if (!coordinator->transferActive) {
			DualBoyGBASIOLockstepCoordinatorWakePlayers(coordinator);
		}
		if (coordinator->syncArmed && coordinator->nextHardSync < 0) {
			if (!coordinator->waiting) {
				_hardSync(coordinator, player);
			}
			coordinator->nextHardSync = _delayAddSaturated(coordinator->nextHardSync, HARD_SYNC_INTERVAL);
		}
	}

	int32_t nextEvent = _untilNextSync(coordinator, player);
	while (true) {
		struct DualBoyGBASIOLockstepEvent* event = player->queue;
		if (!event) {
			break;
		}
		if (_cycleDelta(event->timestamp, DualBoyGBASIOLockstepTime(player)) > 0) {
			break;
		}
		player->queue = event->next;
		struct DualBoyGBASIOLockstepEvent reply = {
			.playerId = player->playerId,
			.timestamp = DualBoyGBASIOLockstepTime(player),
		};
		mLOG(GBA_SIO, DEBUG, "Got event of type %X from %i at timestamp %X",
		                      event->type, event->playerId, event->timestamp);
		switch (event->type) {
		case DUALBOY_SIO_EV_ATTACH:
			_setReady(coordinator, player, event->playerId, -1);
			if (player->playerId == 0) {
				struct GBASIO* sio = player->driver->d.p;
				sio->siocnt = GBASIOMultiplayerClearSlave(sio->siocnt);
			}
			reply.mode = player->mode;
			reply.type = DUALBOY_SIO_EV_MODE_SET;
			_enqueueEvent(coordinator, &reply, TARGET(event->playerId));
			break;
		case DUALBOY_SIO_EV_HARD_SYNC:
			DualBoyGBASIOLockstepCoordinatorAckPlayer(coordinator, player);
			break;
		case DUALBOY_SIO_EV_TRANSFER_START:
			_setData(coordinator, player->playerId, sio);
			nextEvent = _cycleDelta(event->finishCycle, DualBoyGBASIOLockstepTime(player));
			nextEvent = _delayAfterLateness(nextEvent, cyclesLate);
			nextEvent = _sanitizeDelay(nextEvent, "transfer completion", player->playerId);
			player->driver->d.p->siocnt |= 0x80;
			mTimingDeschedule(&sio->p->timing, &sio->completeEvent);
			mTimingSchedule(&sio->p->timing, &sio->completeEvent, nextEvent);
			DualBoyGBASIOLockstepCoordinatorAckPlayer(coordinator, player);
			break;
		case DUALBOY_SIO_EV_MODE_SET:
			_setReady(coordinator, player, event->playerId, event->mode);
			if (event->playerId == 0) {
				DualBoyGBASIOLockstepCoordinatorAckPlayer(coordinator, player);
			}
			break;
		case DUALBOY_SIO_EV_DETACH:
			_setReady(coordinator, player, event->playerId, -1);
			_setReady(coordinator, player, player->playerId, player->mode);
			reply.mode = player->mode;
			reply.type = DUALBOY_SIO_EV_MODE_SET;
			_enqueueEvent(coordinator, &reply, ~TARGET(event->playerId));
			if (player->mode == GBA_SIO_MULTI) {
				sio->siocnt = GBASIOMultiplayerSetId(sio->siocnt, player->playerId);
				sio->siocnt = GBASIOMultiplayerSetSlave(sio->siocnt, player->playerId || coordinator->nAttached < 2);
			}
			wasDetach = true;
			break;
		}
		event->next = player->freeList;
		player->freeList = event;
	}
	if (player->queue) {
		int32_t queuedDelay = _cycleDelta(player->queue->timestamp, DualBoyGBASIOLockstepTime(player));
		if (queuedDelay < nextEvent) {
			nextEvent = queuedDelay;
		}
	}

	if (coordinator->syncArmed && player->playerId != 0 && nextEvent <= LOCKSTEP_INTERVAL) {
		if (!player->queue || wasDetach) {
			DualBoyGBASIOLockstepPlayerSleep(player);
			// XXX: Is there a better way to gain sync lock at the beginning?
			if (nextEvent < 4) {
				nextEvent = 4;
			}
			_verifyAwake(coordinator);
		}
	}
	nextEvent = _sanitizeDelay(nextEvent, "lockstep wake", player->playerId);
	mASSERT_DEBUG(nextEvent > 0);
	mTimingSchedule(timing, &lockstep->event, nextEvent);
}

int32_t DualBoyGBASIOLockstepTime(struct DualBoyGBASIOLockstepPlayer* player) {
	return _cycleDelta(mTimingCurrentTime(&player->driver->d.p->p->timing), player->cycleOffset);
}

void DualBoyGBASIOLockstepCoordinatorWaitOnPlayers(struct DualBoyGBASIOLockstepCoordinator* coordinator, struct DualBoyGBASIOLockstepPlayer* player) {
	mASSERT(!coordinator->waiting);
	mASSERT(!player->asleep);
	mASSERT(player->playerId == 0);
	if (coordinator->nAttached < 2) {
		return;
	}

	_advanceCycle(coordinator, player);
	mLOG(GBA_SIO, DEBUG, "Primary waiting for players to ack");
	coordinator->waiting = ((1 << coordinator->nAttached) - 1) & ~TARGET(player->playerId);
	DualBoyGBASIOLockstepPlayerSleep(player);
	DualBoyGBASIOLockstepCoordinatorWakePlayers(coordinator);

	_verifyAwake(coordinator);
}

void DualBoyGBASIOLockstepCoordinatorWakePlayers(struct DualBoyGBASIOLockstepCoordinator* coordinator) {
	int i;
	for (i = 1; i < coordinator->nAttached; ++i) {
		if (!coordinator->attachedPlayers[i]) {
			continue;
		}
		struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
		DualBoyGBASIOLockstepPlayerWake(player);
	}
}

void DualBoyGBASIOLockstepPlayerWake(struct DualBoyGBASIOLockstepPlayer* player) {
	if (!player->asleep) {
		return;
	}
	player->asleep = false;
	if (player->driver->user && player->driver->user->wake) {
		player->driver->user->wake(player->driver->user);
	}
}

void DualBoyGBASIOLockstepCoordinatorAckPlayer(struct DualBoyGBASIOLockstepCoordinator* coordinator, struct DualBoyGBASIOLockstepPlayer* player) {
	if (player->playerId == 0) {
		return;
	}
	if (!(coordinator->waiting & TARGET(player->playerId))) {
		mLOG(GBA_SIO, DEBUG, "Ignoring stray ack from player %d with no pending wait bit", player->playerId);
		return;
	}
	coordinator->waiting &= ~TARGET(player->playerId);
	if (!coordinator->waiting) {
		mLOG(GBA_SIO, DEBUG, "All players acked, waking primary");
		if (coordinator->transferActive) {
			int i;
			for (i = 0; i < coordinator->nAttached; ++i) {
				if (!coordinator->attachedPlayers[i]) {
					continue;
				}
				struct DualBoyGBASIOLockstepPlayer* player = TableLookup(&coordinator->players, coordinator->attachedPlayers[i]);
				player->dataReceived = true;
			}

			coordinator->transferActive = false;
		}

		struct DualBoyGBASIOLockstepPlayer* runner = TableLookup(&coordinator->players, coordinator->attachedPlayers[0]);
		DualBoyGBASIOLockstepPlayerWake(runner);
	}
	DualBoyGBASIOLockstepPlayerSleep(player);
}

void DualBoyGBASIOLockstepPlayerSleep(struct DualBoyGBASIOLockstepPlayer* player) {
	if (player->asleep) {
		return;
	}
	player->asleep = true;
	if (player->driver->user && player->driver->user->sleep) {
		player->driver->user->sleep(player->driver->user);
	}
	player->driver->d.p->p->cpu->nextEvent = 0;
	player->driver->d.p->p->earlyExit = true;
}

size_t DualBoyGBASIOLockstepCoordinatorAttached(struct DualBoyGBASIOLockstepCoordinator* coordinator) {
	return TableSize(&coordinator->players);
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
