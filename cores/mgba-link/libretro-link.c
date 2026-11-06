/* Network link cable for the mGBA libretro core. See libretro-link.h.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * How it works
 * ------------
 * A real link cable clocks bits between consoles within microseconds; two phones are milliseconds
 * apart and run their frames independently. Three rules keep the games' protocols intact:
 *
 * 1. The console that clocks a transfer (GB internal clock, GBA multiplayer parent, GBA normal
 *    mode internal clock) sends START and blocks until the partners REPLY. Its emulated time does
 *    not advance while blocked, so to the game the transfer looks instant.
 * 2. START carries "gap": the emulated cycles the sender ran since its previous transfer. The
 *    receiver answers only once it has itself run that many cycles since its previous transfer
 *    (measured from the start of a GB or GBA normal mode transfer, and from the completion of a
 *    GBA multiplayer transfer, which a child only learns about from the parent's FINISH), so
 *    its game has had exactly as much time as on hardware to prepare the next value (serial IRQ
 *    handler, refilling SB/SIOMLT_SEND). If it is not there yet it schedules a timing event at
 *    that point and runs early (catch-up), instead of answering with stale data.
 * 3. While transfers are happening ("hot"), consoles exchange frame counters and none runs more
 *    than a couple of frames ahead of another, so blocking on one side can't make the other drift
 *    away and trip the games' link timeouts.
 * 4. Every transfer costs a network round trip, so answers must go out the moment they're due:
 *    while hot we poll the network every ~quarter millisecond of emulated time, and when the
 *    frontend runs us early just to answer (LINK_RUN_SOON) we only run up to that point.
 *
 * Message (little endian): u8 type, u8 mode, u16 seq, payload.
 *   START  u32 gap (UINT32_MAX = no previous transfer), u32 data,
 *          u8 has_previous, u16 previous[4]  (GBA multiplayer: the previous FINISH again, so a
 *          child can recover if that FINISH was lost or arrives after this START)
 *   REPLY  u32 data, u8 player
 *   FINISH u16 data[4]                  (GBA multiplayer: parent -> children)
 *   SYNC   u32 frame
 */
#include "libretro-link.h"

#include <mgba/core/core.h>
#include <mgba/core/timing.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio.h>
#endif
#ifdef M_CORE_GB
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>
#include <mgba/internal/gb/sio.h>
#endif

#include <string.h>
#include <time.h>

#ifdef LINK_DEBUG
#include <stdio.h>
#define LDBG(fmt, ...) fprintf(stderr, "[P%u %10llu] " fmt "\n", (unsigned) gLink.selfId + 1, \
                               (unsigned long long) _emuTime(), ##__VA_ARGS__)
#else
#define LDBG(...) do { } while (0)
#endif

enum { MSG_START = 1, MSG_REPLY = 2, MSG_FINISH = 3, MSG_SYNC = 4 };
enum { MODE_GB = 1, MODE_MULTI = 2, MODE_NORMAL8 = 3, MODE_NORMAL32 = 4 };

#define MAX_PLAYERS 4
#define NO_GAP 0xFFFFFFFFu
#define REPLY_TIMEOUT_MS 400
#define RESEND_MS 40
#define THROTTLE_TIMEOUT_MS 120
#define THROTTLE_FRAMES 2
#define COOL_DOWN_FRAMES 180

static struct {
	retro_environment_t env;
	retro_netpacket_send_t send;
	retro_netpacket_poll_receive_t poll;
	bool active;
	uint16_t selfId;
	unsigned peerMask; /* players we're linked with: host sees its clients, a client sees the host */

	struct mCore* core;
	struct mTiming* timing;
	int platform;
#ifdef M_CORE_GB
	struct GBSIODriver gbDriver;
	struct GBSIO* gbSio;
#endif
#ifdef M_CORE_GBA
	struct GBASIODriver gbaDriver;
	struct GBASIO* gbaSio;
#endif

	/* Our own clocked transfer */
	uint16_t seq;
	bool waiting;
	uint8_t waitMode;
	uint16_t waitSeq;
	uint32_t ownData;
	uint32_t replies[MAX_PLAYERS];
	unsigned replyMask;
	unsigned replyNeeded;

	/* Emulated time of the last transfer start (ours or a partner's that we answered). */
	bool haveLastTransfer;
	uint64_t lastTransfer;

	/* A partner's START we still have to answer */
	bool pending;
	uint8_t pendingMode;
	uint16_t pendingSeq;
	uint16_t pendingFrom;
	uint32_t pendingGap;
	uint32_t pendingData;
	uint64_t pendingDue;
	struct mTimingEvent serviceEvent;
	struct mTimingEvent pollEvent;
	int32_t pollInterval;
	bool runSoonRequested;

	/* Last answered START, to answer resent STARTs identically */
	bool haveAnswered;
	uint16_t answeredSeq;
	uint16_t answeredFrom;
	uint8_t answeredMode;
	uint32_t answeredData;

	/* GBA multiplayer parent: the last FINISH, repeated in the next START */
	bool haveLastFinish;
	uint16_t lastFinishSeq;
	uint16_t lastFinish[4];

	/* GBA multiplayer child: answered, waiting for the parent's FINISH */
	bool multiBusy;
	uint16_t multiSeq;

	uint64_t timeAccum;
	uint32_t timeLast;

	/* Frame throttling */
	bool hot;
	uint32_t lastActivityFrame;
	bool havePeerFrame[MAX_PLAYERS];
	bool haveBaseline[MAX_PLAYERS];
	uint32_t peerFrame[MAX_PLAYERS];
	int32_t baseline[MAX_PLAYERS];
} gLink;

/* ---- helpers ------------------------------------------------------------------------------- */

static uint64_t _nowMs(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void _nap(void) {
	struct timespec ts = { 0, 50000 }; /* 50 us */
	nanosleep(&ts, NULL);
}

/* Emulated cycles as a 64-bit count. mTimingGlobalTime only advances in debugger builds, so
 * extend the 32-bit mTimingCurrentTime instead (called at least once per frame). */
static uint64_t _emuTime(void) {
	if (!gLink.timing) {
		return gLink.timeAccum;
	}
	uint32_t t = (uint32_t) mTimingCurrentTime(gLink.timing);
	gLink.timeAccum += (uint32_t) (t - gLink.timeLast);
	gLink.timeLast = t;
	return gLink.timeAccum;
}

static uint32_t _frame(void) {
	return gLink.core ? gLink.core->frameCounter(gLink.core) : 0;
}

static void _put16(uint8_t* p, uint16_t v) {
	p[0] = v;
	p[1] = v >> 8;
}

static void _put32(uint8_t* p, uint32_t v) {
	_put16(p, v);
	_put16(p + 2, v >> 16);
}

static uint16_t _get16(const uint8_t* p) {
	return p[0] | (p[1] << 8);
}

static uint32_t _get32(const uint8_t* p) {
	return _get16(p) | ((uint32_t) _get16(p + 2) << 16);
}

static void _send(const uint8_t* buf, size_t len, uint16_t to) {
	if (gLink.active && gLink.send) {
		gLink.send(RETRO_NETPACKET_UNRELIABLE | RETRO_NETPACKET_UNSEQUENCED | RETRO_NETPACKET_FLUSH_HINT, buf, len, to);
	}
}

static void _markActivity(void) {
	if (!gLink.hot) {
		memset(gLink.haveBaseline, 0, sizeof(gLink.haveBaseline));
	}
	gLink.hot = true;
	gLink.lastActivityFrame = _frame();
}

static void _sendStart(uint8_t mode, uint16_t seq, uint32_t gap, uint32_t data) {
	LDBG("START  seq=%u mode=%u gap=%u data=%04X", seq, mode, gap, data);
	uint8_t msg[21];
	msg[0] = MSG_START;
	msg[1] = mode;
	_put16(&msg[2], seq);
	_put32(&msg[4], gap);
	_put32(&msg[8], data);
	msg[12] = mode == MODE_MULTI && gLink.haveLastFinish && gLink.lastFinishSeq == (uint16_t) (seq - 1);
	int i;
	for (i = 0; i < 4; ++i) {
		_put16(&msg[13 + i * 2], gLink.lastFinish[i]);
	}
	_send(msg, sizeof(msg), RETRO_NETPACKET_BROADCAST);
}

static void _sendReply(uint8_t mode, uint16_t seq, uint32_t data, uint16_t to) {
	LDBG("REPLY  seq=%u mode=%u data=%04X -> P%u", seq, mode, data, to + 1);
	uint8_t msg[9];
	msg[0] = MSG_REPLY;
	msg[1] = mode;
	_put16(&msg[2], seq);
	_put32(&msg[4], data);
	msg[8] = (uint8_t) gLink.selfId;
	_send(msg, sizeof(msg), to);
}

static bool _canLink(void) {
	return gLink.active && gLink.peerMask && gLink.timing;
}

static int _peerCount(void) {
	int n = 0;
	unsigned m;
	for (m = gLink.peerMask; m; m &= m - 1) {
		++n;
	}
	return n;
}

/* The other console of a two-console link (GB, GBA normal mode). */
static unsigned _partnerId(void) {
	if (gLink.selfId) {
		return 0;
	}
	unsigned i;
	for (i = 1; i < MAX_PLAYERS; ++i) {
		if (gLink.peerMask & (1u << i)) {
			return i;
		}
	}
	return 1;
}

/* Who answers a transfer we clock: the host waits for all its clients, a client for the host. */
static unsigned _partners(void) {
	return gLink.selfId ? 1u : gLink.peerMask;
}

/* Starts a clocked transfer and returns the gap to send with it. */
static uint32_t _beginTransfer(void) {
	uint64_t now = _emuTime();
	uint32_t gap = NO_GAP;
	if (gLink.haveLastTransfer && now - gLink.lastTransfer < NO_GAP) {
		gap = (uint32_t) (now - gLink.lastTransfer);
	}
	gLink.haveLastTransfer = true;
	gLink.lastTransfer = now;
	_markActivity();
	return gap;
}

/* Blocks (polling the network) until all needed replies are in. Returns false on timeout. */
static bool _awaitReplies(void) {
	uint64_t start = _nowMs();
	uint64_t lastSend = start;
	while ((gLink.replyMask & gLink.replyNeeded) != gLink.replyNeeded) {
		if (!gLink.active) {
			return false;
		}
		gLink.poll();
		uint64_t now = _nowMs();
		if (now - start > REPLY_TIMEOUT_MS) {
			return false;
		}
		if (now - lastSend > RESEND_MS) {
			/* UDP may have dropped it; partners answer duplicates from their cache. */
			_sendStart(gLink.waitMode, gLink.waitSeq, NO_GAP, gLink.ownData);
			lastSend = now;
		}
		_nap();
	}
	return true;
}

static void _startWaiting(uint8_t mode, uint32_t data, unsigned needed) {
	gLink.seq++;
	gLink.waiting = true;
	gLink.waitMode = mode;
	gLink.waitSeq = gLink.seq;
	gLink.ownData = data;
	gLink.replyMask = 0;
	gLink.replyNeeded = needed;
	memset(gLink.replies, 0xFF, sizeof(gLink.replies));
	_sendStart(mode, gLink.seq, _beginTransfer(), data);
}

/* ---- answering a partner's START ------------------------------------------------------------- */

static void _service(void) {
	if (!gLink.pending) {
		return;
	}
	gLink.pending = false;
	mTimingDeschedule(gLink.timing, &gLink.serviceEvent);
	uint32_t mine = 0xFFFFFFFF;

	switch (gLink.pendingMode) {
#ifdef M_CORE_GB
	case MODE_GB:
		if (gLink.gbSio) {
			struct GBSIO* sio = gLink.gbSio;
			struct GB* gb = sio->p;
			mine = gb->memory.io[GB_REG_SB];
			uint8_t sc = gb->memory.io[GB_REG_SC];
			/* Like mGBA's own lockstep: the partner always gets our SB, but our side only
			 * completes a transfer if we armed one with the external clock. */
			if (GBRegisterSCIsEnable(sc) && !GBRegisterSCIsShiftClock(sc)) {
				sio->pendingSB = gLink.pendingData;
				sio->remainingBits = 8;
				mTimingDeschedule(&gb->timing, &sio->event);
				mTimingSchedule(&gb->timing, &sio->event, 0);
			}
		}
		break;
#endif
#ifdef M_CORE_GBA
	case MODE_MULTI:
		if (gLink.gbaSio) {
			struct GBASIO* sio = gLink.gbaSio;
			mine = sio->p->memory.io[GBA_REG(SIOMLT_SEND)];
			sio->siocnt = GBASIOMultiplayerFillBusy(sio->siocnt);
			sio->rcnt = GBASIORegisterRCNTClearSc(sio->rcnt);
			gLink.multiBusy = true;
			gLink.multiSeq = gLink.pendingSeq;
		}
		break;
	case MODE_NORMAL8:
	case MODE_NORMAL32:
		if (gLink.gbaSio) {
			struct GBASIO* sio = gLink.gbaSio;
			if (gLink.pendingMode == MODE_NORMAL8) {
				mine = sio->p->memory.io[GBA_REG(SIODATA8)] & 0xFF;
			} else {
				mine = sio->p->memory.io[GBA_REG(SIODATA32_LO)] | ((uint32_t) sio->p->memory.io[GBA_REG(SIODATA32_HI)] << 16);
			}
			bool armed = GBASIONormalIsStart(sio->siocnt) && !GBASIONormalIsInternalSc(sio->siocnt);
			if (armed && gLink.pendingMode == MODE_NORMAL8 && sio->mode == GBA_SIO_NORMAL_8) {
				GBASIONormal8FinishTransfer(sio, gLink.pendingData, 0);
			} else if (armed && gLink.pendingMode == MODE_NORMAL32 && sio->mode == GBA_SIO_NORMAL_32) {
				GBASIONormal32FinishTransfer(sio, gLink.pendingData, 0);
			}
		}
		break;
#endif
	default:
		break;
	}

	gLink.haveAnswered = true;
	gLink.answeredSeq = gLink.pendingSeq;
	gLink.answeredFrom = gLink.pendingFrom;
	gLink.answeredMode = gLink.pendingMode;
	gLink.answeredData = mine;
	if (gLink.pendingMode != MODE_MULTI) {
		gLink.haveLastTransfer = true;
		gLink.lastTransfer = _emuTime();
	}
	_markActivity();
	_sendReply(gLink.pendingMode, gLink.pendingSeq, mine, gLink.pendingFrom);
}

static void _serviceEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	UNUSED(context);
	UNUSED(cyclesLate);
	_service();
}

/* GBA multiplayer child: the transfer we answered is complete. */
static void _applyFinish(uint16_t seq, const uint16_t data[4]) {
#ifdef M_CORE_GBA
	if (!gLink.multiBusy || seq != gLink.multiSeq || !gLink.gbaSio || gLink.gbaSio->mode != GBA_SIO_MULTI) {
		return;
	}
	gLink.multiBusy = false;
	LDBG("FINISH seq=%u applied %04X %04X", seq, data[0], data[1]);
	GBASIOMultiplayerFinishTransfer(gLink.gbaSio, (uint16_t*) data, 0);
	gLink.haveLastTransfer = true;
	gLink.lastTransfer = _emuTime();
#else
	UNUSED(seq);
	UNUSED(data);
#endif
}

static void _handleStart(const uint8_t* msg, size_t len, uint16_t from) {
	if (len < 12) {
		return;
	}
	uint8_t mode = msg[1];
	uint16_t seq = _get16(&msg[2]);
	uint32_t gap = _get32(&msg[4]);
	uint32_t data = _get32(&msg[8]);

	if (gLink.haveAnswered && gLink.answeredFrom == from && gLink.answeredSeq == seq && gLink.answeredMode == mode) {
		_sendReply(mode, seq, gLink.answeredData, from); /* our REPLY was lost */
		return;
	}
	if (gLink.pending && gLink.pendingFrom == from && gLink.pendingSeq == seq) {
		return; /* resent while we're still getting there */
	}

#ifdef M_CORE_GB
	if (mode == MODE_GB && gLink.waiting && gLink.waitMode == MODE_GB) {
		/* Both Game Boys clocked at once: each simply gets the other's byte. */
		gLink.replies[from & 3] = data;
		gLink.replyMask |= 1u << (from & 3);
		gLink.haveAnswered = true;
		gLink.answeredSeq = seq;
		gLink.answeredFrom = from;
		gLink.answeredMode = mode;
		gLink.answeredData = gLink.ownData;
		_sendReply(mode, seq, gLink.ownData, from);
		return;
	}
#endif

	if (gLink.pending) {
		_service(); /* superseded; answer the old one with what we have */
	}
	if (gLink.multiBusy && mode == MODE_MULTI && len >= 21 && msg[12] && seq == (uint16_t) (gLink.multiSeq + 1)) {
		/* The previous FINISH is late or lost; this START carries a copy. */
		uint16_t previous[4];
		int i;
		for (i = 0; i < 4; ++i) {
			previous[i] = _get16(&msg[13 + i * 2]);
		}
		_applyFinish(gLink.multiSeq, previous);
	}
	if (gLink.multiBusy) {
		gLink.multiBusy = false; /* can't recover that transfer; move on */
	}

	gLink.pending = true;
	gLink.pendingMode = mode;
	gLink.pendingSeq = seq;
	gLink.pendingFrom = from;
	gLink.pendingGap = gap;
	gLink.pendingData = data;

	uint64_t now = _emuTime();
	if (gap == NO_GAP || !gLink.haveLastTransfer || now - gLink.lastTransfer >= gap) {
		_service();
		return;
	}
	/* Not far enough yet: answer at exactly the matching point of our own timeline. */
	gLink.pendingDue = gLink.lastTransfer + gap;
	LDBG("defer  seq=%u by %lld cycles", seq, (long long) (gLink.pendingDue - now));
	mTimingDeschedule(gLink.timing, &gLink.serviceEvent);
	mTimingSchedule(gLink.timing, &gLink.serviceEvent, (int32_t) (gLink.pendingDue - now));
	if (gLink.env && !gLink.runSoonRequested) {
		gLink.runSoonRequested = true;
		gLink.env(RETRO_ENVIRONMENT_LINK_RUN_SOON, NULL);
	}
}

static void _handleFinish(const uint8_t* msg, size_t len) {
	if (len < 12) {
		return;
	}
	uint16_t data[4];
	int i;
	for (i = 0; i < 4; ++i) {
		data[i] = _get16(&msg[4 + i * 2]);
	}
	_applyFinish(_get16(&msg[2]), data);
}

/* While hot, look at the network often enough that answers are never held up by our frame. */
static void _pollEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(context);
	if (!gLink.active || !gLink.hot || !gLink.poll) {
		return; /* rescheduled by LinkFrameStart once the link is busy again */
	}
	if (!gLink.waiting) {
		gLink.poll();
	}
	if (gLink.active && gLink.hot) {
		mTimingSchedule(timing, &gLink.pollEvent, gLink.pollInterval - (int32_t) cyclesLate);
	}
}

static void _schedulePolling(void) {
	if (gLink.timing && gLink.pollInterval > 0 && !mTimingIsScheduled(gLink.timing, &gLink.pollEvent)) {
		mTimingSchedule(gLink.timing, &gLink.pollEvent, gLink.pollInterval);
	}
}

/* ---- netpacket callbacks ---------------------------------------------------------------------- */

static void RETRO_CALLCONV _netStart(uint16_t clientId, retro_netpacket_send_t send, retro_netpacket_poll_receive_t poll) {
	gLink.send = send;
	gLink.poll = poll;
	gLink.selfId = clientId;
	gLink.peerMask = clientId == 0 ? 0 : 1u; /* a client is linked to the host (player 0) */
	gLink.active = true;
	gLink.waiting = false;
	gLink.pending = false;
	gLink.multiBusy = false;
	gLink.haveAnswered = false;
	gLink.haveLastTransfer = false;
	gLink.haveLastFinish = false;
	gLink.hot = false;
	memset(gLink.havePeerFrame, 0, sizeof(gLink.havePeerFrame));
	memset(gLink.haveBaseline, 0, sizeof(gLink.haveBaseline));
}

static void RETRO_CALLCONV _netReceive(const void* buf, size_t len, uint16_t from) {
	const uint8_t* msg = buf;
	if (!gLink.active || len < 4) {
		return;
	}
	switch (msg[0]) {
	case MSG_START:
		_handleStart(msg, len, from);
		break;
	case MSG_REPLY:
		if (len >= 9 && gLink.waiting && _get16(&msg[2]) == gLink.waitSeq && msg[1] == gLink.waitMode) {
			unsigned player = msg[8] & 3;
			gLink.replies[player] = _get32(&msg[4]);
			gLink.replyMask |= 1u << player;
		}
		break;
	case MSG_FINISH:
		_handleFinish(msg, len);
		break;
	case MSG_SYNC:
		if (len >= 8 && from < MAX_PLAYERS) {
			gLink.peerFrame[from] = _get32(&msg[4]);
			gLink.havePeerFrame[from] = true;
			if (gLink.hot && !gLink.haveBaseline[from]) {
				gLink.baseline[from] = (int32_t) (_frame() - gLink.peerFrame[from]);
				gLink.haveBaseline[from] = true;
			}
		}
		break;
	default:
		break;
	}
}

static void RETRO_CALLCONV _netStop(void) {
	gLink.active = false;
	gLink.waiting = false;
	gLink.pending = false;
	gLink.multiBusy = false;
	gLink.hot = false;
	gLink.send = NULL;
	gLink.poll = NULL;
	gLink.peerMask = 0;
	if (gLink.timing) {
		mTimingDeschedule(gLink.timing, &gLink.serviceEvent);
		mTimingDeschedule(gLink.timing, &gLink.pollEvent);
	}
}

static bool RETRO_CALLCONV _netConnected(uint16_t clientId) {
	/* GB link cables connect two consoles; GBA multiplayer cables up to four. */
	int maxPeers = gLink.platform == mPLATFORM_GB ? 1 : MAX_PLAYERS - 1;
	if (clientId == 0 || clientId >= MAX_PLAYERS || _peerCount() >= maxPeers) {
		return false;
	}
	gLink.peerMask |= 1u << clientId;
	return true;
}

static void RETRO_CALLCONV _netDisconnected(uint16_t clientId) {
	if (clientId < MAX_PLAYERS) {
		gLink.peerMask &= ~(1u << clientId);
		gLink.havePeerFrame[clientId] = false;
		gLink.haveBaseline[clientId] = false;
	}
}

void LinkSetEnvironment(retro_environment_t env) {
	gLink.env = env;
	static const struct retro_netpacket_callback callbacks = {
		.start = _netStart,
		.receive = _netReceive,
		.stop = _netStop,
		.poll = NULL,
		.connected = _netConnected,
		.disconnected = _netDisconnected,
		.protocol_version = "mgba-link-1",
	};
	env(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, (void*) &callbacks);
}

/* ---- GB serial driver ----------------------------------------------------------------------- */

#ifdef M_CORE_GB
static bool _gbInit(struct GBSIODriver* driver) {
	gLink.gbSio = driver->p;
	return true;
}

static void _gbDeinit(struct GBSIODriver* driver) {
	UNUSED(driver);
}

static void _gbWriteSB(struct GBSIODriver* driver, uint8_t value) {
	UNUSED(driver);
	UNUSED(value);
}

static uint8_t _gbWriteSC(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIO* sio = driver->p;
	if ((value & 0x81) != 0x81 || !_canLink() || gLink.waiting) {
		return value;
	}
	/* We clock this byte: GBSIOWriteSC has already scheduled the 8 bit shifts, we only need
	 * to know what the partner sends back before they happen. */
	_startWaiting(MODE_GB, sio->p->memory.io[GB_REG_SB], _partners());
	bool ok = _awaitReplies();
	gLink.waiting = false;
	sio->pendingSB = ok ? (uint8_t) gLink.replies[_partnerId()] : 0xFF;
	return value;
}
#endif

/* ---- GBA serial driver ---------------------------------------------------------------------- */

#ifdef M_CORE_GBA
static bool _gbaInit(struct GBASIODriver* driver) {
	gLink.gbaSio = driver->p;
	return true;
}

static void _gbaDeinit(struct GBASIODriver* driver) {
	UNUSED(driver);
}

static uint32_t _gbaDriverId(const struct GBASIODriver* driver) {
	UNUSED(driver);
	return 0x4C4E4554; /* "TENL" */
}

static bool _gbaHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	return mode == GBA_SIO_MULTI || mode == GBA_SIO_NORMAL_8 || mode == GBA_SIO_NORMAL_32;
}

static int _gbaConnectedDevices(struct GBASIODriver* driver) {
	UNUSED(driver);
	if (!_canLink()) {
		return 0;
	}
	int n = _peerCount();
	return n < MAX_PLAYERS - 1 ? n : MAX_PLAYERS - 1;
}

static int _gbaDeviceId(struct GBASIODriver* driver) {
	UNUSED(driver);
	if (!_canLink() || gLink.selfId >= MAX_PLAYERS) {
		return 0;
	}
	return gLink.selfId;
}

static uint16_t _gbaWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	switch (driver->p->mode) {
	case GBA_SIO_MULTI:
		/* SD: all connected consoles are in multiplayer mode and ready. */
		value = _canLink() ? GBASIOMultiplayerFillReady(value) : GBASIOMultiplayerClearReady(value);
		value = GBASIOMultiplayerClearError(value);
		if (gLink.multiBusy) {
			value = GBASIOMultiplayerFillBusy(value);
		}
		break;
	case GBA_SIO_NORMAL_8:
	case GBA_SIO_NORMAL_32:
		value = GBASIONormalFillSi(value);
		break;
	default:
		break;
	}
	return value;
}

static uint16_t _gbaWriteRCNT(struct GBASIODriver* driver, uint16_t value) {
	UNUSED(driver);
	return value;
}

static bool _gbaStart(struct GBASIODriver* driver) {
	struct GBASIO* sio = driver->p;
	if (!_canLink() || gLink.waiting) {
		return true; /* no partner: the core completes the transfer with "nothing connected" data */
	}
	uint16_t* io = sio->p->memory.io;
	switch (sio->mode) {
	case GBA_SIO_MULTI:
		if (gLink.selfId != 0) {
			return true;
		}
		_startWaiting(MODE_MULTI, io[GBA_REG(SIOMLT_SEND)], _partners());
		return true; /* the core schedules completion; we collect replies in finishMultiplayer */
	case GBA_SIO_NORMAL_8:
	case GBA_SIO_NORMAL_32:
		if (!GBASIONormalIsInternalSc(sio->siocnt)) {
			return false; /* external clock: completed when the partner's START arrives */
		}
		if (sio->mode == GBA_SIO_NORMAL_8) {
			_startWaiting(MODE_NORMAL8, io[GBA_REG(SIODATA8)] & 0xFF, 1u << _partnerId());
		} else {
			_startWaiting(MODE_NORMAL32, io[GBA_REG(SIODATA32_LO)] | ((uint32_t) io[GBA_REG(SIODATA32_HI)] << 16), 1u << _partnerId());
		}
		return true;
	default:
		return true;
	}
}

static void _gbaFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
	UNUSED(driver);
	memset(data, 0xFF, sizeof(uint16_t) * 4);
	if (!gLink.waiting || gLink.waitMode != MODE_MULTI) {
		return;
	}
	_awaitReplies();
	gLink.waiting = false;
	gLink.haveLastTransfer = true;
	gLink.lastTransfer = _emuTime();
	data[0] = (uint16_t) gLink.ownData;
	int i;
	for (i = 1; i < MAX_PLAYERS; ++i) {
		if (gLink.replyMask & (1u << i)) {
			data[i] = (uint16_t) gLink.replies[i];
		}
	}
	gLink.haveLastFinish = true;
	gLink.lastFinishSeq = gLink.waitSeq;
	memcpy(gLink.lastFinish, data, sizeof(gLink.lastFinish));
	uint8_t msg[12];
	msg[0] = MSG_FINISH;
	msg[1] = MODE_MULTI;
	_put16(&msg[2], gLink.waitSeq);
	for (i = 0; i < 4; ++i) {
		_put16(&msg[4 + i * 2], data[i]);
	}
	_send(msg, sizeof(msg), RETRO_NETPACKET_BROADCAST);
	_send(msg, sizeof(msg), RETRO_NETPACKET_BROADCAST); /* cheap insurance against a lost datagram */
}

static uint32_t _gbaFinishNormal(void) {
	if (!gLink.waiting || (gLink.waitMode != MODE_NORMAL8 && gLink.waitMode != MODE_NORMAL32)) {
		return 0xFFFFFFFF;
	}
	bool ok = _awaitReplies();
	gLink.waiting = false;
	return ok ? gLink.replies[_partnerId()] : 0xFFFFFFFF;
}

static uint8_t _gbaFinishNormal8(struct GBASIODriver* driver) {
	UNUSED(driver);
	return (uint8_t) _gbaFinishNormal();
}

static uint32_t _gbaFinishNormal32(struct GBASIODriver* driver) {
	UNUSED(driver);
	return _gbaFinishNormal();
}
#endif

/* ---- lifecycle ------------------------------------------------------------------------------ */

void LinkAttach(struct mCore* core) {
	gLink.core = core;
	gLink.platform = core->platform(core);
	gLink.timing = NULL;
	gLink.timeAccum = 0;
	gLink.timeLast = 0;
	gLink.haveLastTransfer = false;
	gLink.serviceEvent.context = NULL;
	gLink.serviceEvent.name = "Netplay link service";
	gLink.serviceEvent.callback = _serviceEvent;
	gLink.serviceEvent.priority = 0x90;
	gLink.pollEvent.context = NULL;
	gLink.pollEvent.name = "Netplay link poll";
	gLink.pollEvent.callback = _pollEvent;
	gLink.pollEvent.priority = 0x91;
	gLink.pollInterval = (int32_t) (core->frequency(core) / 4096); /* ~0.25 ms */
	gLink.runSoonRequested = false;

#ifdef M_CORE_GB
	if (gLink.platform == mPLATFORM_GB) {
		struct GB* gb = core->board;
		gLink.timing = &gb->timing;
		memset(&gLink.gbDriver, 0, sizeof(gLink.gbDriver));
		gLink.gbDriver.init = _gbInit;
		gLink.gbDriver.deinit = _gbDeinit;
		gLink.gbDriver.writeSB = _gbWriteSB;
		gLink.gbDriver.writeSC = _gbWriteSC;
		GBSIOSetDriver(&gb->sio, &gLink.gbDriver);
	}
#endif
#ifdef M_CORE_GBA
	if (gLink.platform == mPLATFORM_GBA) {
		struct GBA* gba = core->board;
		gLink.timing = &gba->timing;
		memset(&gLink.gbaDriver, 0, sizeof(gLink.gbaDriver));
		gLink.gbaDriver.init = _gbaInit;
		gLink.gbaDriver.deinit = _gbaDeinit;
		gLink.gbaDriver.driverId = _gbaDriverId;
		gLink.gbaDriver.handlesMode = _gbaHandlesMode;
		gLink.gbaDriver.connectedDevices = _gbaConnectedDevices;
		gLink.gbaDriver.deviceId = _gbaDeviceId;
		gLink.gbaDriver.writeSIOCNT = _gbaWriteSIOCNT;
		gLink.gbaDriver.writeRCNT = _gbaWriteRCNT;
		gLink.gbaDriver.start = _gbaStart;
		gLink.gbaDriver.finishMultiplayer = _gbaFinishMultiplayer;
		gLink.gbaDriver.finishNormal8 = _gbaFinishNormal8;
		gLink.gbaDriver.finishNormal32 = _gbaFinishNormal32;
		core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &gLink.gbaDriver);
	}
#endif
}

void LinkDetach(void) {
	if (gLink.timing) {
		mTimingDeschedule(gLink.timing, &gLink.serviceEvent);
		mTimingDeschedule(gLink.timing, &gLink.pollEvent);
	}
	gLink.pending = false;
	gLink.waiting = false;
	gLink.multiBusy = false;
	gLink.core = NULL;
	gLink.timing = NULL;
#ifdef M_CORE_GB
	gLink.gbSio = NULL;
#endif
#ifdef M_CORE_GBA
	gLink.gbaSio = NULL;
#endif
}

/* Don't run more than THROTTLE_FRAMES ahead of a partner while the link is busy. */
void LinkFrameStart(void) {
	if (!gLink.active || !gLink.hot || !gLink.poll) {
		return;
	}
	_schedulePolling();
	if (_frame() - gLink.lastActivityFrame > COOL_DOWN_FRAMES) {
		gLink.hot = false;
		return;
	}
	uint64_t start = _nowMs();
	while (gLink.active && gLink.hot) {
		bool ahead = false;
		int i;
		for (i = 0; i < MAX_PLAYERS; ++i) {
			if (i == gLink.selfId || !gLink.havePeerFrame[i] || !gLink.haveBaseline[i]) {
				continue;
			}
			int32_t lead = (int32_t) (_frame() - gLink.peerFrame[i]) - gLink.baseline[i];
			if (lead > THROTTLE_FRAMES) {
				ahead = true;
			}
		}
		if (!ahead || gLink.pending) {
			return; /* a pending START needs us to run, not to wait */
		}
		if (_nowMs() - start > THROTTLE_TIMEOUT_MS) {
			gLink.hot = false; /* partner paused or gone; stop throttling until the next transfer */
			return;
		}
		gLink.poll();
		_nap();
	}
}

void LinkFrameEnd(void) {
	_emuTime(); /* keep the 64-bit clock extension fed */
	if (!gLink.active || !gLink.core) {
		return;
	}
	if (gLink.hot) {
		uint8_t msg[8];
		msg[0] = MSG_SYNC;
		msg[1] = 0;
		_put16(&msg[2], 0);
		_put32(&msg[4], _frame());
		_send(msg, sizeof(msg), RETRO_NETPACKET_BROADCAST);
	}
}

bool LinkRunPartial(void) {
	bool requested = gLink.runSoonRequested;
	gLink.runSoonRequested = false;
	if (!requested || !gLink.pending || !gLink.active || !gLink.core) {
		return false;
	}
	/* We were woken early only to answer a partner: run just until the answer is out (or the
	 * frame ends), so we're back to listening right away instead of emulating a whole frame. */
	uint32_t frame = _frame();
	while (gLink.pending && gLink.active && _frame() == frame) {
		gLink.core->runLoop(gLink.core);
	}
	return _frame() == frame;
}
