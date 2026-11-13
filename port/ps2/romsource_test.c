#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "romsource.h"

#define TEST_OUTPUT_SIZE 4096u
#define TEST_CONTAINER_SIZE 8192u
#define TEST_SCRATCH_SIZE 257u

static u32 buildContainer(u8 *container, const u8 *input, u32 inputSize)
{
	z_stream stream;

	memset(&stream, 0, sizeof(stream));
	assert(deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8,
		Z_DEFAULT_STRATEGY) == Z_OK);

	container[0] = 0x11;
	container[1] = 0x73;
	container[2] = (u8)(inputSize >> 16);
	container[3] = (u8)(inputSize >> 8);
	container[4] = (u8)inputSize;

	stream.next_in = (u8 *)input;
	stream.avail_in = inputSize;
	stream.next_out = container + 5;
	stream.avail_out = TEST_CONTAINER_SIZE - 5;
	assert(deflate(&stream, Z_FINISH) == Z_STREAM_END);
	assert(deflateEnd(&stream) == Z_OK);

	return 5u + (u32)stream.total_out;
}

static void verifySource(struct romsource *source, const u8 *expected)
{
	u8 output[TEST_OUTPUT_SIZE];
	u8 scratch[TEST_SCRATCH_SIZE];
	u8 range[37];
	u32 outputSize = 0;
	u32 compressedSize = 0;

	assert(romSourceGetRzip1173Size(source, 0, &outputSize));
	assert(outputSize == TEST_OUTPUT_SIZE);
	assert(romSourceInflate1173(source, 0, output, sizeof(output), scratch,
		sizeof(scratch), &compressedSize));
	assert(compressedSize > 0);
	assert(memcmp(output, expected, sizeof(output)) == 0);

	assert(romSourceReadAt(source, 11, range, sizeof(range)));
	assert(!romSourceReadAt(source, romSourceGetSize(source) - 3, range, sizeof(range)));
	assert(!romSourceInflate1173(source, 0, output, sizeof(output) - 1, scratch,
		sizeof(scratch), NULL));
}

int main(void)
{
	u8 expected[TEST_OUTPUT_SIZE];
	u8 container[TEST_CONTAINER_SIZE];
	u8 cachedRange[73];
	struct romsource source = { 0 };
	char path[] = "/tmp/pd_romsource_test_XXXXXX";
	const int fd = mkstemp(path);
	u32 containerSize;

	assert(fd >= 0);

	for (u32 i = 0; i < TEST_OUTPUT_SIZE; ++i) {
		expected[i] = (u8)((i * 29u) ^ (i >> 3));
	}

	containerSize = buildContainer(container, expected, sizeof(expected));
	romSourceInitMemory(&source, container, containerSize);
	verifySource(&source, expected);
	assert(romSourceView(&source, 0, 5) == container);
	romSourceClose(&source);

	assert(write(fd, container, containerSize) == (ssize_t)containerSize);
	assert(close(fd) == 0);
	assert(romSourceOpenFile(&source, path));
	assert(unlink(path) == 0);
	assert(romSourceView(&source, 0, 5) == NULL);
	verifySource(&source, expected);
	assert(containerSize > 1024u + sizeof(cachedRange));
	assert(romSourceReadAt(&source, 509u, cachedRange, sizeof(cachedRange)));
	assert(memcmp(cachedRange, container + 509u, sizeof(cachedRange)) == 0);
	assert(romSourceReadAt(&source, 1024u, cachedRange, sizeof(cachedRange)));
	assert(memcmp(cachedRange, container + 1024u, sizeof(cachedRange)) == 0);
	romSourceClose(&source);

	/*
	 * Streamed audio: a dozen voices each read a few hundred bytes per
	 * audio frame from their own position. Interleaved sequential streams
	 * must share the cache without evicting each other, so the device is
	 * touched once per 16 KiB of progress per stream, not once per read.
	 */
	{
		enum { STREAMS = 12, STEP = 300, ROUNDS = 200, FILE_BYTES = 1024 * 1024 };
		static u8 big[FILE_BYTES];
		char bigPath[] = "/tmp/pd_romsource_stream_XXXXXX";
		const int bigFd = mkstemp(bigPath);
		u8 chunk[STEP];

		assert(bigFd >= 0);
		for (u32 i = 0; i < FILE_BYTES; ++i) {
			big[i] = (u8)((i * 131u) ^ (i >> 7) ^ (i >> 13));
		}
		assert(write(bigFd, big, FILE_BYTES) == (ssize_t)FILE_BYTES);
		assert(close(bigFd) == 0);
		assert(romSourceOpenFile(&source, bigPath));
		assert(unlink(bigPath) == 0);

		for (u32 round = 0; round < ROUNDS; ++round) {
			for (u32 stream = 0; stream < STREAMS; ++stream) {
				const u32 offset = stream * 70001u + round * STEP;
				assert(romSourceReadAt(&source, offset, chunk, STEP));
				assert(memcmp(chunk, big + offset, STEP) == 0);
			}
		}
		/* 60000 bytes per stream = 4 lines each, plus slack for line edges */
		assert(source.read_cache_reads <= STREAMS * 5u);
		assert(source.read_cache_reads >= STREAMS * 3u);
		romSourceClose(&source);
	}

	puts("romsource_test: ok");
	return 0;
}
