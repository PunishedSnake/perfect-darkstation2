#ifndef _ULTRATYPES_H_
#define _ULTRATYPES_H_


/**************************************************************************
 *                                                                        *
 *               Copyright (C) 1995, Silicon Graphics, Inc.               *
 *                                                                        *
 *  These coded instructions, statements, and computer programs  contain  *
 *  unpublished  proprietary  information of Silicon Graphics, Inc., and  *
 *  are protected by Federal copyright law.  They  may not be disclosed  *
 *  to  third  parties or copied or duplicated in any form, in whole or  *
 *  in part, without the prior written consent of Silicon Graphics, Inc.  *
 *                                                                        *
 **************************************************************************/


/*************************************************************************
 *
 *  File: ultratypes.h
 *
 *  This file contains various types used in Ultra64 interfaces.
 *
 *  $Revision: 1.6 $
 *  $Date: 1997/12/17 04:02:06 $
 *  $Source: /hosts/gate3/exdisk2/cvs/N64OS/Master/cvsmdev2/PR/include/ultratypes.h,v $
 *
 **************************************************************************/



/**********************************************************************
 * General data types for R4300
 */
#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

typedef unsigned char           u8;  /* unsigned  8-bit */
typedef unsigned short int      u16; /* unsigned 16-bit */
typedef unsigned int            u32; /* unsigned 32-bit */
typedef unsigned long long int  u64; /* unsigned 64-bit */

typedef signed char             s8;  /* signed  8-bit */
typedef signed short int        s16; /* signed 16-bit */
typedef signed int              s32; /* signed 32-bit */
typedef signed long long int    s64; /* signed 64-bit */

/* Keep volatile aliases tied to the fixed-width base aliases above. Using
 * long for the 32-bit variants is not portable to LP64 hosts and conflicts
 * with PS2SDK tamtypes.h, where u32/s32 are int-based. */
typedef volatile u8             vu8;
typedef volatile u16            vu16;
typedef volatile u32            vu32;
typedef volatile u64            vu64;

typedef volatile s8             vs8;
typedef volatile s16            vs16;
typedef volatile s32            vs32;
typedef volatile s64            vs64;

typedef float                   f32; /* single prec floating point */
typedef double                  f64; /* double prec floating point */

#ifdef PLATFORM_N64

#if !defined(_SIZE_T) && !defined(_SIZE_T_) && !defined(_SIZE_T_DEF)
#define _SIZE_T
#define _SIZE_T_DEF             /* exeGCC size_t define label */
#if (_MIPS_SZLONG == 32)
typedef unsigned int    size_t;
#endif
#if (_MIPS_SZLONG == 64)
typedef unsigned long   size_t;
#endif
#endif

#else

#include <stddef.h>

#endif

#endif  /* _LANGUAGE_C */


/*************************************************************************
 * Common definitions
 */
#ifndef TRUE
#define TRUE    1
#endif

#ifndef FALSE
#define FALSE   0
#endif

#ifndef NULL
#define NULL    0
#endif

#endif  /* _ULTRATYPES_H_ */
