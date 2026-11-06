/* Joins an Emulate app session over UDP with a libretro core running on the desktop, speaking
 * the app's wire format (app/src/main/cpp/netplay.cpp). End-to-end test for link features.
 *
 *   linkpeer <core.dll> <rom> <host[:port]> <tag> [seconds=15]
 *
 * e.g. with the app hosting a GB game in the Android emulator (adb emu redir add udp:47000:47000):
 *   linkpeer mgba_libretro.dll linktest_slave.gb 127.0.0.1 GB
 */
#include "../../app/src/main/cpp/libretro.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LINK_RUN_SOON (RETRO_ENVIRONMENT_PRIVATE | 0x4D4C)
enum { Discover = 1, Announce = 2, Join = 3, Welcome = 4, Reject = 5, Data = 6, Ping = 7, Bye = 8 };

static SOCKET sock;
static struct sockaddr_in host;
static uint16_t selfId;
static struct retro_netpacket_callback np;
static bool hasNp, runSoon;
static enum retro_pixel_format format;
static uint32_t center;
static unsigned frames, received;

static double now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sendRaw(uint8_t type, uint16_t dst, const void* payload, size_t len) {
	uint8_t buf[2048];
	memcpy(buf, "EMNP", 4);
	buf[4] = 1;
	buf[5] = type;
	buf[6] = selfId & 0xFF;
	buf[7] = selfId >> 8;
	buf[8] = dst & 0xFF;
	buf[9] = dst >> 8;
	if (len) {
		memcpy(buf + 10, payload, len);
	}
	sendto(sock, (const char*) buf, (int) (10 + len), 0, (struct sockaddr*) &host, sizeof(host));
}

static void npSend(int flags, const void* buf, size_t len, uint16_t dst) {
	(void) flags;
	if (buf && len) {
		sendRaw(Data, dst, buf, len);
	}
}

static void drain(void) {
	uint8_t buf[65536];
	for (;;) {
		int n = recv(sock, (char*) buf, sizeof(buf), 0);
		if (n < 10 || memcmp(buf, "EMNP", 4) != 0) {
			return;
		}
		if (buf[5] == Data) {
			++received;
			np.receive(buf + 10, n - 10, buf[6] | (buf[7] << 8));
		} else if (buf[5] == Bye) {
			printf("host left\n");
			exit(0);
		}
	}
}

static void npPoll(void) {
	drain();
}

static void logImpl(enum retro_log_level level, const char* fmt, ...) {
	if (level < RETRO_LOG_WARN) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

static bool env(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char**) data = ".";
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		format = *(enum retro_pixel_format*) data;
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback*) data)->log = logImpl;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool*) data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
		np = *(const struct retro_netpacket_callback*) data;
		hasNp = true;
		return true;
	case LINK_RUN_SOON:
		runSoon = true;
		return true;
	default:
		return false;
	}
}

static void video(const void* data, unsigned w, unsigned h, size_t pitch) {
	if (!data) {
		return;
	}
	const uint8_t* row = (const uint8_t*) data + (h / 2) * pitch;
	if (format == RETRO_PIXEL_FORMAT_XRGB8888) {
		center = ((const uint32_t*) row)[w / 2] & 0xFFFFFF;
	} else {
		uint16_t p = ((const uint16_t*) row)[w / 2];
		center = ((p >> 11) & 0x1F) << 19 | ((p >> 5) & 0x3F) << 10 | (p & 0x1F) << 3;
	}
	++frames;
}

static void audioSample(int16_t l, int16_t r) { (void) l; (void) r; }
static size_t audioBatch(const int16_t* d, size_t f) { (void) d; return f; }
static void inputPoll(void) {}
static int16_t inputState(unsigned a, unsigned b, unsigned c, unsigned d) { (void) a; (void) b; (void) c; (void) d; return 0; }

static const char* colourName(uint32_t c) {
	int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
	if (r > 150 && g < 80 && b < 80) return "RED";
	if (g > 150 && r < 80 && b < 80) return "GREEN";
	if (b > 150 && r < 80 && g < 80) return "BLUE";
	if (g > 150 && b > 150 && r < 80) return "CYAN";
	return "OTHER";
}

#define SYM(name) ((void*) GetProcAddress(dll, name))

int main(int argc, char** argv) {
	if (argc < 5) {
		fprintf(stderr, "usage: %s core.dll rom host[:port] tag [seconds]\n", argv[0]);
		return 2;
	}
	double seconds = argc > 5 ? atof(argv[5]) : 15;

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	u_long nonblocking = 1;
	ioctlsocket(sock, FIONBIO, &nonblocking);
	char hostname[256];
	strncpy(hostname, argv[3], sizeof(hostname) - 1);
	int port = 47000;
	char* colon = strchr(hostname, ':');
	if (colon) {
		*colon = 0;
		port = atoi(colon + 1);
	}
	host.sin_family = AF_INET;
	host.sin_port = htons(port);
	inet_pton(AF_INET, hostname, &host.sin_addr);

	HMODULE dll = LoadLibraryA(argv[1]);
	if (!dll) {
		fprintf(stderr, "LoadLibrary failed\n");
		return 1;
	}
	((void (*)(retro_environment_t)) SYM("retro_set_environment"))(env);
	((void (*)(retro_video_refresh_t)) SYM("retro_set_video_refresh"))(video);
	((void (*)(retro_audio_sample_t)) SYM("retro_set_audio_sample"))(audioSample);
	((void (*)(retro_audio_sample_batch_t)) SYM("retro_set_audio_sample_batch"))(audioBatch);
	((void (*)(retro_input_poll_t)) SYM("retro_set_input_poll"))(inputPoll);
	((void (*)(retro_input_state_t)) SYM("retro_set_input_state"))(inputState);
	((void (*)(void)) SYM("retro_init"))();
	void (*run)(void) = (void (*)(void)) SYM("retro_run");

	FILE* f = fopen(argv[2], "rb");
	if (!f) {
		perror(argv[2]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* rom = malloc(size);
	fread(rom, 1, size, f);
	fclose(f);
	struct retro_game_info game = { argv[2], rom, size, NULL };
	if (!((bool (*)(const struct retro_game_info*)) SYM("retro_load_game"))(&game) || !hasNp) {
		fprintf(stderr, "load failed or no netpacket interface\n");
		return 1;
	}

	/* Join: protocol is "<core protocol>/<tag>", as the app builds it. */
	char protocol[128];
	snprintf(protocol, sizeof(protocol), "%s/%s", np.protocol_version ? np.protocol_version : "", argv[4]);
	double deadline = now() + 6;
	bool joined = false;
	while (!joined && now() < deadline) {
		sendRaw(Join, 0, protocol, strlen(protocol));
		double until = now() + 0.25;
		while (now() < until) {
			uint8_t buf[2048];
			int n = recv(sock, (char*) buf, sizeof(buf), 0);
			if (n >= 10 && !memcmp(buf, "EMNP", 4)) {
				if (buf[5] == Welcome) {
					selfId = buf[8] | (buf[9] << 8);
					joined = true;
					break;
				}
				if (buf[5] == Reject) {
					printf("rejected: %.*s\n", n - 10, buf + 10);
					return 1;
				}
			}
			Sleep(1);
		}
	}
	if (!joined) {
		printf("no answer from host\n");
		return 1;
	}
	printf("joined as player %u (protocol %s)\n", selfId + 1, protocol);
	np.start(selfId, npSend, npPoll);

	double t0 = now(), next = now(), lastPing = 0, lastReport = now();
	unsigned lastFrames = 0;
	while (now() - t0 < seconds) {
		drain();
		run();
		if (now() - lastPing > 0.5) {
			sendRaw(Ping, 0, NULL, 0);
			lastPing = now();
		}
		next += 1.0 / 60;
		if (next < now() - 0.05) {
			next = now();
		}
		while (!runSoon && now() < next) {
			drain();
			Sleep(0);
		}
		if (runSoon) {
			runSoon = false;
			next = now();
		}
		if (now() - lastReport >= 1) {
			printf("t=%4.1fs peer %-5s %3u fps %6u pkts\n", now() - t0, colourName(center), frames - lastFrames, received);
			fflush(stdout);
			lastFrames = frames;
			lastReport = now();
		}
	}
	sendRaw(Bye, 0, NULL, 0);
	printf("FINAL peer=%s\n", colourName(center));
	return 0;
}
