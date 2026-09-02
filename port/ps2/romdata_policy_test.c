#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "romdata_policy.h"

int main(void)
{
	const uint32_t romSize = 32u * 1024u * 1024u;
	const uintptr_t segment = romdataStreamVirtualAddress(0x00839dd0u);
	uint32_t offset = UINT32_MAX;

	assert(romdataFileExtentValid(0x1000u, 0x2000u, romSize));
	assert(romdataFileExtentValid(0x12bace0u, 0x12bace0u, romSize));
	assert(romdataFileExtentValid(romSize, romSize, romSize));
	assert(!romdataFileExtentValid(0x2000u, 0x1000u, romSize));
	assert(!romdataFileExtentValid(romSize, romSize + 1u, romSize));

	assert(romdataStreamWindowContains(segment));
	assert(romdataStreamWindowContains(
		romdataStreamVirtualAddress(romSize - 1u)));
	assert(!romdataStreamWindowContains(ROMDATA_STREAM_VIRTUAL_BASE - 1u));
	assert(!romdataStreamWindowContains(
		(uintptr_t)ROMDATA_STREAM_VIRTUAL_BASE + romSize));

	assert(romdataStreamRangeOffset(segment, 16u, segment, 0x100u, &offset));
	assert(offset == 0u);
	assert(romdataStreamRangeOffset(segment + 0xf0u, 16u,
		segment, 0x100u, &offset));
	assert(offset == 0xf0u);
	assert(romdataStreamRangeOffset(segment + 0x100u, 0u,
		segment, 0x100u, &offset));
	assert(offset == 0x100u);
	assert(!romdataStreamRangeOffset(segment - 1u, 1u,
		segment, 0x100u, &offset));
	assert(!romdataStreamRangeOffset(segment + 0xf8u, 16u,
		segment, 0x100u, &offset));
	assert(!romdataStreamRangeOffset(segment, 1u,
		segment, 0x100u, NULL));

	return 0;
}
