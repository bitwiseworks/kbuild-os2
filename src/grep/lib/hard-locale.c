/* hard-locale.c -- Determine whether a locale is hard.

   Copyright (C) 1997-1999, 2002-2004, 2006-2007, 2009-2021 Free Software
   Foundation, Inc.

   This file is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of the
   License, or (at your option) any later version.

   This file is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

#include <config.h>

#include "hard-locale.h"

#include <locale.h>
#include <string.h>

bool
hard_locale (int category)
{
  char locale[SETLOCALE_NULL_MAX];

  if (setlocale_null_r (category, locale, sizeof (locale)))
    return false;

  return !(strcmp (locale, "C") == 0 || strcmp (locale, "POSIX") == 0);
}
