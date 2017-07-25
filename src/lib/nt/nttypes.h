/* $Id: nttypes.h 2702 2013-11-21 00:11:08Z bird $ */
/** @file
 * MSC + NT basic & common types, various definitions.
 */

/*
 * Copyright (c) 2005-2013 knut st. osmundsen <bird-kBuild-spamx@anduin.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Alternatively, the content of this file may be used under the terms of the
 * GPL version 2 or later, or LGPL version 2.1 or later.
 */

#ifndef ___nt_nttypes_h
#define ___nt_nttypes_h

#include <sys/types.h>

typedef struct BirdTimeSpec
{
    __int64       tv_sec;
    __int32       tv_nsec;
    __int32       tv_padding0;
} BirdTimeSpec_T;

/** The distance between the NT and unix epochs given in NT time (units of 100
 *  ns). */
#define BIRD_NT_EPOCH_OFFSET_UNIX_100NS 116444736000000000LL

#endif


