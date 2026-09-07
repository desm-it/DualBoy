/* Adapted from libretro/mgba PR #318 at fa743c965939f091350df094f57e639933bc17e3. */
/* Copyright (c) 2013-2024 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef DUALBOY_MGBA_LOCKSTEP_H
#define DUALBOY_MGBA_LOCKSTEP_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/lockstep.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gba/sio.h>
#include <mgba-util/circle-buffer.h>
#include <mgba-util/table.h>
#include <mgba-util/threading.h>

#define DUALBOY_MAX_LOCKSTEP_EVENTS 8

enum DualBoyGBASIOLockstepEventType {
	DUALBOY_SIO_EV_ATTACH,
	DUALBOY_SIO_EV_DETACH,
	DUALBOY_SIO_EV_HARD_SYNC,
	DUALBOY_SIO_EV_MODE_SET,
	DUALBOY_SIO_EV_TRANSFER_START,
};

struct DualBoyGBASIOLockstepCoordinator {
	struct Table players;
	Mutex mutex;

	unsigned nextId;

	unsigned attachedPlayers[MAX_GBAS];
	int nAttached;
	uint32_t waiting;

	bool transferActive;
	bool syncArmed;
	enum GBASIOMode transferMode;

	int32_t cycle;
	int32_t nextHardSync;

	uint16_t multiData[4];
	uint32_t normalData[4];
};

struct DualBoyGBASIOLockstepEvent {
	enum DualBoyGBASIOLockstepEventType type;
	int32_t timestamp;
	struct DualBoyGBASIOLockstepEvent* next;
	int playerId;
	union {
		enum GBASIOMode mode;
		int32_t finishCycle;
	};
};

struct DualBoyGBASIOLockstepPlayer {
	struct DualBoyGBASIOLockstepDriver* driver;
	int playerId;
	enum GBASIOMode mode;
	enum GBASIOMode otherModes[MAX_GBAS];
	bool asleep;
	int32_t cycleOffset;
	struct DualBoyGBASIOLockstepEvent* queue;
	bool dataReceived;

	struct DualBoyGBASIOLockstepEvent buffer[DUALBOY_MAX_LOCKSTEP_EVENTS];
	struct DualBoyGBASIOLockstepEvent* freeList;
};

struct DualBoyGBASIOLockstepDriver {
	struct GBASIODriver d;
	struct DualBoyGBASIOLockstepCoordinator* coordinator;
	struct mTimingEvent event;
	unsigned lockstepId;
	bool loadingMachineState;

	struct mLockstepUser* user;
};

void DualBoyGBASIOLockstepCoordinatorInit(struct DualBoyGBASIOLockstepCoordinator*);
void DualBoyGBASIOLockstepCoordinatorDeinit(struct DualBoyGBASIOLockstepCoordinator*);

void DualBoyGBASIOLockstepCoordinatorAttach(struct DualBoyGBASIOLockstepCoordinator*, struct DualBoyGBASIOLockstepDriver*);
void DualBoyGBASIOLockstepCoordinatorDetach(struct DualBoyGBASIOLockstepCoordinator*, struct DualBoyGBASIOLockstepDriver*);
size_t DualBoyGBASIOLockstepCoordinatorAttached(struct DualBoyGBASIOLockstepCoordinator*);

void DualBoyGBASIOLockstepDriverCreate(struct DualBoyGBASIOLockstepDriver*, struct mLockstepUser*);
bool DualBoyGBASIOLockstepDriverValidateState(const struct DualBoyGBASIOLockstepDriver*, const void*, size_t);

CXX_GUARD_END

#endif
