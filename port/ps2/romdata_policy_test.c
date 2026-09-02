#include <assert.h>
#include <stdint.h>

#include "romdata_policy.h"

int main(void)
{
	const uint32_t romSize = 32u * 1024u * 1024u;

	assert(romdataFileExtentValid(0x1000u, 0x2000u, romSize));
	assert(romdataFileExtentValid(0x12bace0u, 0x12bace0u, romSize));
	assert(romdataFileExtentValid(romSize, romSize, romSize));
	assert(!romdataFileExtentValid(0x2000u, 0x1000u, romSize));
	assert(!romdataFileExtentValid(romSize, romSize + 1u, romSize));

	return 0;
}
