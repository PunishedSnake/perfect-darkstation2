#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <zlib.h>

#include "lib/rzip.h"

void rmonPrintf(const char *format, ...)
{
	(void)format;
}

static uint32_t makeRareZip(const uint8_t *input, uint32_t inputLen,
	uint8_t *output, uint32_t outputCapacity, int use1173)
{
	z_stream stream = { 0 };
	const uint32_t headerLen = use1173 ? 5u : 2u;

	assert(inputLen > 0 && inputLen <= 0x00ffffffu);
	assert(outputCapacity > headerLen);
	assert(deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, -15, 8,
		Z_DEFAULT_STRATEGY) == Z_OK);

	output[0] = 0x11;
	output[1] = use1173 ? 0x73 : 0x72;
	if (use1173) {
		output[2] = (uint8_t)(inputLen >> 16);
		output[3] = (uint8_t)(inputLen >> 8);
		output[4] = (uint8_t)inputLen;
	}

	stream.next_in = (Bytef *)input;
	stream.avail_in = inputLen;
	stream.next_out = output + headerLen;
	stream.avail_out = outputCapacity - headerLen;

	assert(deflate(&stream, Z_FINISH) == Z_STREAM_END);
	const uint32_t result = headerLen + (uint32_t)stream.total_out;
	assert(deflateEnd(&stream) == Z_OK);
	return result;
}

int main(void)
{
	static const uint8_t input[] =
		"Perfect DarkStation 2 bounded RZIP loader contract";
	uint8_t packed[256];
	uint8_t output[sizeof(input)];
	uint8_t scratch[32];
	uint32_t packedLen = makeRareZip(input, sizeof(input), packed, sizeof(packed), 1);

	memset(output, 0, sizeof(output));
	assert(rzipInflateSized(packed, packedLen, output, sizeof(output), scratch)
		== (s32)sizeof(input));
	assert(memcmp(output, input, sizeof(input)) == 0);

	assert(rzipInflateSized(packed, 4, output, sizeof(output), scratch) == 0);
	assert(rzipInflateSized(packed, packedLen - 1, output, sizeof(output), scratch) == 0);
	assert(rzipInflateSized(packed, packedLen, output, sizeof(output) - 1, scratch) == 0);

	packed[4]++;
	assert(rzipInflateSized(packed, packedLen, output, sizeof(output), scratch) == 0);
	packed[4]--;

	packed[0] = 0;
	assert(rzipInflateSized(packed, packedLen, output, sizeof(output), scratch) == 0);

	packedLen = makeRareZip(input, sizeof(input), packed, sizeof(packed), 0);
	memset(output, 0, sizeof(output));
	assert(rzipInflateSized(packed, packedLen, output, sizeof(output), scratch)
		== (s32)sizeof(input));
	assert(memcmp(output, input, sizeof(input)) == 0);
	assert(rzipInflateSized(packed, packedLen - 1, output, sizeof(output), scratch) == 0);
	assert(rzipInflateSized(packed, packedLen, output, sizeof(output) - 1, scratch) == 0);

	return 0;
}
