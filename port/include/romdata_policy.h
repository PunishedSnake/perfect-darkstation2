#ifndef PERFECT_DARK_ROMDATA_POLICY_H
#define PERFECT_DARK_ROMDATA_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* Equal offsets are legal and describe an empty ROM file. */
static inline bool romdataFileExtentValid(
		uint32_t offset, uint32_t nextOffset, uint32_t romSize)
{
	return offset <= nextOffset && nextOffset <= romSize;
}

#endif
