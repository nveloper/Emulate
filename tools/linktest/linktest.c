/* Desktop test harness for the mGBA network link (cores/mgba-link).
 *
 * Loads N copies of a libretro core, runs each in its own thread at ~60 fps and connects them
 * through libretro's netpacket interface with an in-memory "network" that adds latency, the way
 * the Android frontend does over UDP (including RETRO_ENVIRONMENT_LINK_RUN_SOON). Prints the
 * colour of each console's centre pixel so a link test ROM's verdict can be read.
 *
 *   linktest <core.dll> <rom[,rom2,...]> [players=2] [seconds=10] [latency_ms=3] [jitter_ms=1]
 *
 * With several comma-separated ROMs, player i loads ROM i (the last one repeats).
 */
#include "../../app/src/main/cpp/libretro.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#define MAX_INST 4
#define LINK_RUN_SOON (RETRO_ENVIRONMENT_PRIVATE | 0x4D4C)

typedef struct Msg {
	struct Msg* next;
	double due;
	uint16_t from;
	size_t len;
	uint8_t data[];
} Msg;

typedef struct {
	int idx;
	HMODULE dll;
	void (*init)(void);
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	bool (*load_game)(const struct retro_game_info*);
	void (*run)(void);
	void (*get_system_av_info)(struct retro_system_av_info*);

	struct retro_netpacket_callback np;
	bool hasNp;
	enum retro_pixel_format format;
	atomic_uint center;
	atomic_uint frames;
	atomic_bool runSoon;
	atomic_uint delivered;
	pthread_mutex_t qm;
	Msg* queue;
	pthread_t thread;
} Inst;

static Inst inst[MAX_INST];
static int players = 2;
static double latency = 0.003, jitter = 0.001;
static atomic_bool stopAll;
#define MAX_ROMS 4
static uint8_t* romData[MAX_ROMS];
static size_t romSize[MAX_ROMS];
static char* romPath[MAX_ROMS];
static int romCount;

static double now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void nap(double seconds) {
	struct timespec ts = { (time_t) seconds, (long) ((seconds - (time_t) seconds) * 1e9) };
	nanosleep(&ts, NULL);
}

/* ---- network ---- */

static void sendImpl(int from, const void* buf, size_t len, uint16_t to) {
	if (!buf || !len) {
		return;
	}
	for (int i = 0; i < players; ++i) {
		if (i == from || (to != RETRO_NETPACKET_BROADCAST && to != i)) {
			continue;
		}
		Msg* m = malloc(sizeof(Msg) + len);
		m->next = NULL;
		m->due = now() + latency + jitter * (rand() / (double) RAND_MAX);
		m->from = (uint16_t) from;
		m->len = len;
		memcpy(m->data, buf, len);
		pthread_mutex_lock(&inst[i].qm);
		/* keep arrival order per sender, like a LAN usually does */
		Msg** p = &inst[i].queue;
		while (*p && (*p)->due <= m->due) {
			p = &(*p)->next;
		}
		m->next = *p;
		*p = m;
		pthread_mutex_unlock(&inst[i].qm);
	}
}

static void deliver(int i) {
	for (;;) {
		pthread_mutex_lock(&inst[i].qm);
		Msg* m = inst[i].queue;
		if (!m || m->due > now()) {
			pthread_mutex_unlock(&inst[i].qm);
			return;
		}
		inst[i].queue = m->next;
		pthread_mutex_unlock(&inst[i].qm);
		atomic_fetch_add(&inst[i].delivered, 1);
		inst[i].np.receive(m->data, m->len, m->from);
		free(m);
	}
}

/* ---- libretro callbacks ---- */

static void logImpl(enum retro_log_level level, const char* fmt, ...) {
	if (level < RETRO_LOG_WARN) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

static bool envImpl(int i, unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char**) data = ".";
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		inst[i].format = *(enum retro_pixel_format*) data;
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback*) data)->log = logImpl;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool*) data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
		inst[i].np = *(const struct retro_netpacket_callback*) data;
		inst[i].hasNp = true;
		return true;
	case LINK_RUN_SOON:
		inst[i].runSoon = true;
		return true;
	default:
		return false;
	}
}

static void videoImpl(int i, const void* data, unsigned w, unsigned h, size_t pitch) {
	if (!data) {
		return;
	}
	const uint8_t* row = (const uint8_t*) data + (h / 2) * pitch;
	uint32_t rgb;
	if (inst[i].format == RETRO_PIXEL_FORMAT_XRGB8888) {
		rgb = ((const uint32_t*) row)[w / 2] & 0xFFFFFF;
	} else {
		uint16_t p = ((const uint16_t*) row)[w / 2];
		rgb = ((p >> 11) & 0x1F) << 19 | ((p >> 5) & 0x3F) << 10 | (p & 0x1F) << 3;
	}
	inst[i].center = rgb;
	atomic_fetch_add(&inst[i].frames, 1);
}

#define CALLBACKS(n)                                                                                                \
	static bool env##n(unsigned c, void* d) { return envImpl(n, c, d); }                                          \
	static void video##n(const void* d, unsigned w, unsigned h, size_t p) { videoImpl(n, d, w, h, p); }           \
	static void npSend##n(int f, const void* b, size_t l, uint16_t t) { (void) f; sendImpl(n, b, l, t); }          \
	static void npPoll##n(void) { deliver(n); }
CALLBACKS(0)
CALLBACKS(1)
CALLBACKS(2)
CALLBACKS(3)
static retro_environment_t envs[] = { env0, env1, env2, env3 };
static retro_video_refresh_t videos[] = { video0, video1, video2, video3 };
static retro_netpacket_send_t sends[] = { npSend0, npSend1, npSend2, npSend3 };
static retro_netpacket_poll_receive_t polls[] = { npPoll0, npPoll1, npPoll2, npPoll3 };

static void audioSample(int16_t l, int16_t r) { (void) l; (void) r; }
static size_t audioBatch(const int16_t* d, size_t f) { (void) d; return f; }
static void inputPoll(void) {}
static int16_t inputState(unsigned a, unsigned b, unsigned c, unsigned d) { (void) a; (void) b; (void) c; (void) d; return 0; }

/* ---- instances ---- */

static void* sym(Inst* in, const char* name) {
	void* p = (void*) GetProcAddress(in->dll, name);
	if (!p) {
		fprintf(stderr, "missing %s\n", name);
		exit(1);
	}
	return p;
}

static void loadInstance(int i, const char* corePath) {
	char copy[MAX_PATH];
	snprintf(copy, sizeof(copy), "linktest_core%d.dll", i);
	if (!CopyFileA(corePath, copy, FALSE)) {
		fprintf(stderr, "copy %s failed\n", corePath);
		exit(1);
	}
	Inst* in = &inst[i];
	in->idx = i;
	pthread_mutex_init(&in->qm, NULL);
	in->dll = LoadLibraryA(copy);
	if (!in->dll) {
		fprintf(stderr, "LoadLibrary %s failed (%lu)\n", copy, GetLastError());
		exit(1);
	}
	in->init = sym(in, "retro_init");
	in->set_environment = sym(in, "retro_set_environment");
	in->set_video_refresh = sym(in, "retro_set_video_refresh");
	in->set_audio_sample = sym(in, "retro_set_audio_sample");
	in->set_audio_sample_batch = sym(in, "retro_set_audio_sample_batch");
	in->set_input_poll = sym(in, "retro_set_input_poll");
	in->set_input_state = sym(in, "retro_set_input_state");
	in->load_game = sym(in, "retro_load_game");
	in->run = sym(in, "retro_run");
	in->get_system_av_info = sym(in, "retro_get_system_av_info");

	in->set_environment(envs[i]);
	in->set_video_refresh(videos[i]);
	in->set_audio_sample(audioSample);
	in->set_audio_sample_batch(audioBatch);
	in->set_input_poll(inputPoll);
	in->set_input_state(inputState);
	in->init();
	int r = i < romCount ? i : romCount - 1;
	struct retro_game_info game = { romPath[r], romData[r], romSize[r], NULL };
	if (!in->load_game(&game)) {
		fprintf(stderr, "load_game failed\n");
		exit(1);
	}
	if (!in->hasNp) {
		fprintf(stderr, "core has no netpacket interface\n");
		exit(1);
	}
}

static void* runInstance(void* arg) {
	Inst* in = arg;
	struct retro_system_av_info av;
	in->get_system_av_info(&av);
	double frame = 1.0 / (av.timing.fps > 1 ? av.timing.fps : 60.0);
	double next = now();
	while (!stopAll) {
		deliver(in->idx);
		in->run();
		next += frame;
		if (next < now() - 0.05) {
			next = now();
		}
		while (!in->runSoon && now() < next && !stopAll) {
			deliver(in->idx);
			nap(0.0002);
		}
		if (atomic_exchange(&in->runSoon, false)) {
			next = now();
		}
	}
	return NULL;
}

static const char* colourName(uint32_t c) {
	int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
	if (r > 150 && g < 80 && b < 80) return "RED";
	if (g > 150 && r < 80 && b < 80) return "GREEN";
	if (b > 150 && r < 80 && g < 80) return "BLUE";
	if (g > 150 && b > 150 && r < 80) return "CYAN";
	if (r < 30 && g < 30 && b < 30) return "BLACK";
	if (r > 200 && g > 200 && b > 200) return "WHITE";
	return "OTHER";
}

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s core.dll rom [players] [seconds] [latency_ms] [jitter_ms]\n", argv[0]);
		return 2;
	}
	char* list = strdup(argv[2]);
	for (char* tok = strtok(list, ","); tok && romCount < MAX_ROMS; tok = strtok(NULL, ",")) {
		romPath[romCount++] = tok;
	}
	players = argc > 3 ? atoi(argv[3]) : 2;
	double seconds = argc > 4 ? atof(argv[4]) : 10;
	latency = (argc > 5 ? atof(argv[5]) : 3) / 1000.0;
	jitter = (argc > 6 ? atof(argv[6]) : 1) / 1000.0;
	if (players < 1 || players > MAX_INST) {
		return 2;
	}

	for (int r = 0; r < romCount; ++r) {
		FILE* f = fopen(romPath[r], "rb");
		if (!f) {
			perror(romPath[r]);
			return 1;
		}
		fseek(f, 0, SEEK_END);
		romSize[r] = ftell(f);
		fseek(f, 0, SEEK_SET);
		romData[r] = malloc(romSize[r]);
		fread(romData[r], 1, romSize[r], f);
		fclose(f);
	}

	for (int i = 0; i < players; ++i) {
		loadInstance(i, argv[1]);
	}
	/* Session: instance 0 hosts, the others join (as the Android Netplay class does). */
	inst[0].np.start(0, sends[0], polls[0]);
	for (int i = 1; i < players; ++i) {
		if (inst[0].np.connected && !inst[0].np.connected(i)) {
			fprintf(stderr, "host rejected player %d\n", i);
		}
		inst[i].np.start(i, sends[i], polls[i]);
	}

	for (int i = 0; i < players; ++i) {
		pthread_create(&inst[i].thread, NULL, runInstance, &inst[i]);
	}
	double t0 = now();
	unsigned lastFrames[MAX_INST] = { 0 };
	while (now() - t0 < seconds) {
		nap(1.0);
		printf("t=%4.1fs", now() - t0);
		for (int i = 0; i < players; ++i) {
			unsigned frames = inst[i].frames;
			printf(" | P%d %-5s %3u fps %6u pkts", i + 1, colourName(inst[i].center), frames - lastFrames[i], (unsigned) inst[i].delivered);
			lastFrames[i] = frames;
		}
		printf("\n");
		fflush(stdout);
	}
	stopAll = true;
	for (int i = 0; i < players; ++i) {
		pthread_join(inst[i].thread, NULL);
	}
	printf("FINAL");
	for (int i = 0; i < players; ++i) {
		printf(" P%d=%s", i + 1, colourName(inst[i].center));
	}
	printf("\n");
	return 0;
}
