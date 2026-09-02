#ifndef PERFECT_DARK_ROMDATA_POLICY_H
#define PERFECT_DARK_ROMDATA_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * File-backed ROM segments keep the original pointer-shaped game contract,
 * but live in a virtual address window which cannot alias the PS2's 32 MiB of
 * EE RAM. dmaStart resolves this window before considering a normal memory
 * copy. The complete N64 ROM fits in the 32 MiB window below.
 */
#define ROMDATA_STREAM_VIRTUAL_BASE UINT32_C(0x60000000)
#define ROMDATA_STREAM_VIRTUAL_SIZE UINT32_C(0x02000000)

/* Equal offsets are legal and describe an empty ROM file. */
static inline bool romdataFileExtentValid(
		uint32_t offset, uint32_t nextOffset, uint32_t romSize)
{
	return offset <= nextOffset && nextOffset <= romSize;
}

static inline uintptr_t romdataStreamVirtualAddress(uint32_t romOffset)
{
	return (uintptr_t)ROMDATA_STREAM_VIRTUAL_BASE + romOffset;
}

static inline bool romdataStreamWindowContains(uintptr_t address)
{
	return address >= (uintptr_t)ROMDATA_STREAM_VIRTUAL_BASE &&
		address < (uintptr_t)ROMDATA_STREAM_VIRTUAL_BASE +
			(uintptr_t)ROMDATA_STREAM_VIRTUAL_SIZE;
}

/* Resolve a transfer wholly contained in one mapped segment. */
static inline bool romdataStreamRangeOffset(
		uintptr_t address, uint32_t length, uintptr_t segmentAddress,
		uint32_t segmentSize, uint32_t *outOffset)
{
	if (!outOffset || address < segmentAddress) {
		return false;
	}

	const uintptr_t deltaWide = address - segmentAddress;
	if (deltaWide > segmentSize) {
		return false;
	}

	const uint32_t delta = (uint32_t)deltaWide;
	if (length > segmentSize - delta) {
		return false;
	}

	*outOffset = delta;
	return true;
}

#endif
