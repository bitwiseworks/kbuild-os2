/* $Id: ntunlink.h 3060 2017-09-21 15:11:07Z bird $ */
/** @file
 * MSC + NT unlink and variations.
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

#ifndef ___nt_ntunlink_h
#define ___nt_ntunlink_h

#include "nttypes.h"
#include <wchar.h>

int birdUnlink(const char *pszFile);
int birdUnlinkW(const wchar_t *pwszFile);
int birdUnlinkEx(void *hRoot, const char *pszFile);
int birdUnlinkExW(void *hRoot, const wchar_t *pwszFile);
int birdUnlinkForced(const char *pszFile);
int birdUnlinkForcedW(const wchar_t *pwszFile);
int birdUnlinkForcedEx(void *hRoot, const char *pszFile);
int birdUnlinkForcedExW(void *hRoot, const wchar_t *pszFile);
int birdUnlinkForcedFast(const char *pszFile);
int birdUnlinkForcedFastW(const wchar_t *pwszFile);
int birdUnlinkForcedFastEx(void *hRoot, const char *pszFile);
int birdUnlinkForcedFastExW(void *hRoot, const wchar_t *pwszFile);

#undef  unlink
#define unlink(a_pszPath)     birdUnlinkForced(a_pszPath)

#endif

