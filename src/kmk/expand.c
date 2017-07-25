/* Variable expansion functions for GNU Make.
Copyright (C) 1988, 1989, 1990, 1991, 1992, 1993, 1994, 1995, 1996, 1997,
1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009,
2010 Free Software Foundation, Inc.
This file is part of GNU Make.

GNU Make is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 3 of the License, or (at your option) any later
version.

GNU Make is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "make.h"

#include <assert.h>

#include "filedef.h"
#include "job.h"
#include "commands.h"
#include "variable.h"
#include "rule.h"
#ifdef CONFIG_WITH_COMPILER
# include "kmk_cc_exec.h"
#endif

/* Initially, any errors reported when expanding strings will be reported
   against the file where the error appears.  */
const struct floc **expanding_var = &reading_file;

/* The next two describe the variable output buffer.
   This buffer is used to hold the variable-expansion of a line of the
   makefile.  It is made bigger with realloc whenever it is too small.
   variable_buffer_length is the size currently allocated.
   variable_buffer is the address of the buffer.

   For efficiency, it's guaranteed that the buffer will always have
   VARIABLE_BUFFER_ZONE extra bytes allocated.  This allows you to add a few
   extra chars without having to call a function.  Note you should never use
   these bytes unless you're _sure_ you have room (you know when the buffer
   length was last checked.  */

#define VARIABLE_BUFFER_ZONE    5

#ifndef KMK
static unsigned int variable_buffer_length;
#else
unsigned int variable_buffer_length;
#endif
char *variable_buffer;


#ifdef CONFIG_WITH_VALUE_LENGTH
struct recycled_buffer
{
  struct recycled_buffer *next;
  unsigned int length;
};
struct recycled_buffer *recycled_head;
#endif /* CONFIG_WITH_VALUE_LENGTH */



#ifndef KMK
/* Subroutine of variable_expand and friends:
   The text to add is LENGTH chars starting at STRING to the variable_buffer.
   The text is added to the buffer at PTR, and the updated pointer into
   the buffer is returned as the value.  Thus, the value returned by
   each call to variable_buffer_output should be the first argument to
   the following call.  */

char *
variable_buffer_output (char *ptr, const char *string, unsigned int length)
{
  register unsigned int newlen = length + (ptr - variable_buffer);

  if ((newlen + VARIABLE_BUFFER_ZONE) > variable_buffer_length)
    {
      unsigned int offset = ptr - variable_buffer;
      variable_buffer_length = (newlen + 100 > 2 * variable_buffer_length
				? newlen + 100
				: 2 * variable_buffer_length);
      variable_buffer = xrealloc (variable_buffer, variable_buffer_length);
      ptr = variable_buffer + offset;
    }

  memcpy (ptr, string, length);
  return ptr + length;
}
#endif

/* Return a pointer to the beginning of the variable buffer.  */

static char *
initialize_variable_output (void)
{
  /* If we don't have a variable output buffer yet, get one.  */

#ifdef CONFIG_WITH_VALUE_LENGTH
  if (variable_buffer == 0)
    {
      struct recycled_buffer *recycled = recycled_head;
      if (recycled)
        {
          recycled_head = recycled->next;
          variable_buffer_length = recycled->length;
          variable_buffer = (char *)recycled;
        }
      else
        {
          variable_buffer_length = 384;
          variable_buffer = xmalloc (variable_buffer_length);
        }
      variable_buffer[0] = '\0';
    }
#else  /* CONFIG_WITH_VALUE_LENGTH */
  if (variable_buffer == 0)
    {
      variable_buffer_length = 200;
      variable_buffer = xmalloc (variable_buffer_length);
      variable_buffer[0] = '\0';
    }
#endif /* CONFIG_WITH_VALUE_LENGTH */

  return variable_buffer;
}

/* Recursively expand V.  The returned string is malloc'd.  */

static char *allocated_variable_append (const struct variable *v);

char *
#ifndef CONFIG_WITH_VALUE_LENGTH
recursively_expand_for_file (struct variable *v, struct file *file)
#else
recursively_expand_for_file (struct variable *v, struct file *file,
                             unsigned int *value_lenp)
#endif
{
  char *value;
  const struct floc *this_var;
  const struct floc **saved_varp;
  struct variable_set_list *save = 0;
  int set_reading = 0;

  /* Don't install a new location if this location is empty.
     This can happen for command-line variables, builtin variables, etc.  */
  saved_varp = expanding_var;
  if (v->fileinfo.filenm)
    {
      this_var = &v->fileinfo;
      expanding_var = &this_var;
    }

  /* If we have no other file-reading context, use the variable's context. */
  if (!reading_file)
    {
      set_reading = 1;
      reading_file = &v->fileinfo;
    }

  if (v->expanding)
    {
      if (!v->exp_count)
        /* Expanding V causes infinite recursion.  Lose.  */
        fatal (*expanding_var,
               _("Recursive variable `%s' references itself (eventually)"),
               v->name);
      --v->exp_count;
    }

  if (file)
    {
      save = current_variable_set_list;
      current_variable_set_list = file->variables;
    }

  v->expanding = 1;
#ifndef CONFIG_WITH_VALUE_LENGTH
  if (v->append)
    value = allocated_variable_append (v);
  else
    value = allocated_variable_expand (v->value);
#else  /* CONFIG_WITH_VALUE_LENGTH */
  if (!v->append)
    {
      if (!IS_VARIABLE_RECURSIVE_WITHOUT_DOLLAR (v))
        value = allocated_variable_expand_2 (v->value, v->value_length, value_lenp);
      else
        {
          unsigned int len = v->value_length;
          value = xmalloc (len + 2);
          memcpy (value, v->value, len + 1);
          value[len + 1] = '\0'; /* Extra terminator like allocated_variable_expand_2 returns. Why? */
          if (value_lenp)
            *value_lenp = len;
        }
    }
  else
    {
      value = allocated_variable_append (v);
      if (value_lenp)
        *value_lenp = strlen (value);
    }
#endif /* CONFIG_WITH_VALUE_LENGTH */
  v->expanding = 0;

  if (set_reading)
    reading_file = 0;

  if (file)
    current_variable_set_list = save;

  expanding_var = saved_varp;

  return value;
}

#ifdef CONFIG_WITH_VALUE_LENGTH
/* Worker for reference_variable() and kmk_exec_* that expands the recursive
   variable V. The main difference between this and
   recursively_expand[_for_file] is that this worker avoids the temporary
   buffer and outputs directly into the current variable buffer (O).  */
char *
reference_recursive_variable (char *o, struct variable *v)
{
  const struct floc *this_var;
  const struct floc **saved_varp;
  int set_reading = 0;

  /* Don't install a new location if this location is empty.
     This can happen for command-line variables, builtin variables, etc.  */
  saved_varp = expanding_var;
  if (v->fileinfo.filenm)
    {
      this_var = &v->fileinfo;
      expanding_var = &this_var;
    }

  /* If we have no other file-reading context, use the variable's context. */
  if (!reading_file)
    {
      set_reading = 1;
      reading_file = &v->fileinfo;
    }

  if (v->expanding)
    {
      if (!v->exp_count)
        /* Expanding V causes infinite recursion.  Lose.  */
        fatal (*expanding_var,
               _("Recursive variable `%s' references itself (eventually)"),
               v->name);
      --v->exp_count;
    }

  v->expanding = 1;
  if (!v->append)
    {
      /* Expand directly into the variable buffer.  */
# ifdef CONFIG_WITH_COMPILER
      v->expand_count++;
      if (   v->expandprog
          || (v->expand_count == 3 && kmk_cc_compile_variable_for_expand (v)) )
        o = kmk_exec_expand_to_var_buf (v, o);
      else
        variable_expand_string_2 (o, v->value, v->value_length, &o);
# else
      MAKE_STATS_2 (v->expand_count++);
      variable_expand_string_2 (o, v->value, v->value_length, &o);
# endif
    }
  else
    {
      /* XXX: Feel free to optimize appending target variables as well.  */
      char *value = allocated_variable_append (v);
      unsigned int value_len = strlen (value);
      o = variable_buffer_output (o, value, value_len);
      free (value);
    }
  v->expanding = 0;

  if (set_reading)
    reading_file = 0;

  expanding_var = saved_varp;

  return o;
}
#endif /* CONFIG_WITH_VALUE_LENGTH */

/* Expand a simple reference to variable NAME, which is LENGTH chars long.  */

#ifdef MY_INLINE /* bird */
MY_INLINE char *
#else
#if defined(__GNUC__)
__inline
#endif
static char *
#endif
reference_variable (char *o, const char *name, unsigned int length)
{
  struct variable *v;
#ifndef CONFIG_WITH_VALUE_LENGTH
  char *value;
#endif

  v = lookup_variable (name, length);

  if (v == 0)
    warn_undefined (name, length);

  /* If there's no variable by that name or it has no value, stop now.  */
  if (v == 0 || (*v->value == '\0' && !v->append))
    return o;

#ifdef CONFIG_WITH_VALUE_LENGTH
  assert (v->value_length == strlen (v->value));
  if (!v->recursive || IS_VARIABLE_RECURSIVE_WITHOUT_DOLLAR (v))
    o = variable_buffer_output (o, v->value, v->value_length);
  else
    o = reference_recursive_variable (o, v);
#else  /* !CONFIG_WITH_VALUE_LENGTH */
  value = (v->recursive ? recursively_expand (v) : v->value);

  o = variable_buffer_output (o, value, strlen (value));

  if (v->recursive)
    free (value);
#endif /* !CONFIG_WITH_VALUE_LENGTH */

  return o;
}

#ifndef CONFIG_WITH_VALUE_LENGTH /* Only using variable_expand_string_2! */
/* Scan STRING for variable references and expansion-function calls.  Only
   LENGTH bytes of STRING are actually scanned.  If LENGTH is -1, scan until
   a null byte is found.

   Write the results to LINE, which must point into `variable_buffer'.  If
   LINE is NULL, start at the beginning of the buffer.
   Return a pointer to LINE, or to the beginning of the buffer if LINE is
   NULL.
 */
char *
variable_expand_string (char *line, const char *string, long length)
{
  struct variable *v;
  const char *p, *p1;
  char *abuf = NULL;
  char *o;
  unsigned int line_offset;

  if (!line)
    line = initialize_variable_output();
  o = line;
  line_offset = line - variable_buffer;

  if (length == 0)
    {
      variable_buffer_output (o, "", 1);
      return (variable_buffer);
    }

  /* If we want a subset of the string, allocate a temporary buffer for it.
     Most of the functions we use here don't work with length limits.  */
  if (length > 0 && string[length] != '\0')
    {
      abuf = xmalloc(length+1);
      memcpy(abuf, string, length);
      abuf[length] = '\0';
      string = abuf;
    }
  p = string;

  while (1)
    {
      /* Copy all following uninteresting chars all at once to the
         variable output buffer, and skip them.  Uninteresting chars end
	 at the next $ or the end of the input.  */

      p1 = strchr (p, '$');

      o = variable_buffer_output (o, p, p1 != 0 ? (unsigned int)(p1 - p) : strlen (p) + 1);

      if (p1 == 0)
	break;
      p = p1 + 1;

      /* Dispatch on the char that follows the $.  */

      switch (*p)
	{
	case '$':
	  /* $$ seen means output one $ to the variable output buffer.  */
	  o = variable_buffer_output (o, p, 1);
	  break;

	case '(':
	case '{':
	  /* $(...) or ${...} is the general case of substitution.  */
	  {
	    char openparen = *p;
	    char closeparen = (openparen == '(') ? ')' : '}';
            const char *begp;
	    const char *beg = p + 1;
	    char *op;
            char *abeg = NULL;
	    const char *end, *colon;

	    op = o;
	    begp = p;
	    if (handle_function (&op, &begp))
	      {
		o = op;
		p = begp;
		break;
	      }

	    /* Is there a variable reference inside the parens or braces?
	       If so, expand it before expanding the entire reference.  */

	    end = strchr (beg, closeparen);
	    if (end == 0)
              /* Unterminated variable reference.  */
              fatal (*expanding_var, _("unterminated variable reference"));
	    p1 = lindex (beg, end, '$');
	    if (p1 != 0)
	      {
		/* BEG now points past the opening paren or brace.
		   Count parens or braces until it is matched.  */
		int count = 0;
		for (p = beg; *p != '\0'; ++p)
		  {
		    if (*p == openparen)
		      ++count;
		    else if (*p == closeparen && --count < 0)
		      break;
		  }
		/* If COUNT is >= 0, there were unmatched opening parens
		   or braces, so we go to the simple case of a variable name
		   such as `$($(a)'.  */
		if (count < 0)
		  {
		    abeg = expand_argument (beg, p); /* Expand the name.  */
		    beg = abeg;
		    end = strchr (beg, '\0');
		  }
	      }
	    else
	      /* Advance P to the end of this reference.  After we are
                 finished expanding this one, P will be incremented to
                 continue the scan.  */
	      p = end;

	    /* This is not a reference to a built-in function and
	       any variable references inside are now expanded.
	       Is the resultant text a substitution reference?  */

	    colon = lindex (beg, end, ':');
	    if (colon)
	      {
		/* This looks like a substitution reference: $(FOO:A=B).  */
		const char *subst_beg, *subst_end, *replace_beg, *replace_end;

		subst_beg = colon + 1;
		subst_end = lindex (subst_beg, end, '=');
		if (subst_end == 0)
		  /* There is no = in sight.  Punt on the substitution
		     reference and treat this as a variable name containing
		     a colon, in the code below.  */
		  colon = 0;
		else
		  {
		    replace_beg = subst_end + 1;
		    replace_end = end;

		    /* Extract the variable name before the colon
		       and look up that variable.  */
		    v = lookup_variable (beg, colon - beg);
		    if (v == 0)
		      warn_undefined (beg, colon - beg);

                    /* If the variable is not empty, perform the
                       substitution.  */
		    if (v != 0 && *v->value != '\0')
		      {
			char *pattern, *replace, *ppercent, *rpercent;
			char *value = (v->recursive
                                       ? recursively_expand (v)
				       : v->value);

                        /* Copy the pattern and the replacement.  Add in an
                           extra % at the beginning to use in case there
                           isn't one in the pattern.  */
                        pattern = alloca (subst_end - subst_beg + 2);
                        *(pattern++) = '%';
                        memcpy (pattern, subst_beg, subst_end - subst_beg);
                        pattern[subst_end - subst_beg] = '\0';

                        replace = alloca (replace_end - replace_beg + 2);
                        *(replace++) = '%';
                        memcpy (replace, replace_beg,
                               replace_end - replace_beg);
                        replace[replace_end - replace_beg] = '\0';

                        /* Look for %.  Set the percent pointers properly
                           based on whether we find one or not.  */
			ppercent = find_percent (pattern);
			if (ppercent)
                          {
                            ++ppercent;
                            rpercent = find_percent (replace);
                            if (rpercent)
                              ++rpercent;
                          }
			else
                          {
                            ppercent = pattern;
                            rpercent = replace;
                            --pattern;
                            --replace;
                          }

                        o = patsubst_expand_pat (o, value, pattern, replace,
                                                 ppercent, rpercent);

			if (v->recursive)
			  free (value);
		      }
		  }
	      }

	    if (colon == 0)
	      /* This is an ordinary variable reference.
		 Look up the value of the variable.  */
		o = reference_variable (o, beg, end - beg);

	  if (abeg)
	    free (abeg);
	  }
	  break;

	case '\0':
	  break;

	default:
	  if (isblank ((unsigned char)p[-1]))
	    break;

	  /* A $ followed by a random char is a variable reference:
	     $a is equivalent to $(a).  */
          o = reference_variable (o, p, 1);

	  break;
	}

      if (*p == '\0')
	break;

      ++p;
    }

  if (abuf)
    free (abuf);

  variable_buffer_output (o, "", 1);
  return (variable_buffer + line_offset);
}

#else /* CONFIG_WITH_VALUE_LENGTH */
/* Scan STRING for variable references and expansion-function calls.  Only
   LENGTH bytes of STRING are actually scanned.  If LENGTH is -1, scan until
   a null byte is found.

   Write the results to LINE, which must point into `variable_buffer'.  If
   LINE is NULL, start at the beginning of the buffer.
   Return a pointer to LINE, or to the beginning of the buffer if LINE is
   NULL. Set EOLP to point to the string terminator.
 */
char *
variable_expand_string_2 (char *line, const char *string, long length, char **eolp)
{
  struct variable *v;
  const char *p, *p1, *eos;
  char *o;
  unsigned int line_offset;

  if (!line)
    line = initialize_variable_output();
  o = line;
  line_offset = line - variable_buffer;

  if (length < 0)
    length = strlen (string);
  else
    MY_ASSERT_MSG (string + length == (p1 = memchr (string, '\0', length)) || !p1, ("len=%ld p1=%p %s\n", length, p1, line));

  /* Simple 1: Emptry string. */

  if (length == 0)
    {
      o = variable_buffer_output (o, "\0", 2);
      *eolp = o - 2;
      return (variable_buffer + line_offset);
    }

  /* Simple 2: Nothing to expand. ~50% if the kBuild calls. */

  p1 = (const char *)memchr (string, '$', length);
  if (p1 == 0)
    {
      o = variable_buffer_output (o, string, length);
      o = variable_buffer_output (o, "\0", 2);
      *eolp = o - 2;
      assert (strchr (variable_buffer + line_offset, '\0') == *eolp);
      return (variable_buffer + line_offset);
    }

  p = string;
  eos = p + length;

  while (1)
    {
      /* Copy all following uninteresting chars all at once to the
         variable output buffer, and skip them.  Uninteresting chars end
	 at the next $ or the end of the input.  */

      o = variable_buffer_output (o, p, p1 != 0 ? (p1 - p) : (eos - p));

      if (p1 == 0)
	break;
      p = p1 + 1;

      /* Dispatch on the char that follows the $.  */

      switch (*p)
	{
	case '$':
	  /* $$ seen means output one $ to the variable output buffer.  */
	  o = variable_buffer_output (o, p, 1);
	  break;

	case '(':
	case '{':
	  /* $(...) or ${...} is the general case of substitution.  */
	  {
	    char openparen = *p;
	    char closeparen = (openparen == '(') ? ')' : '}';
            const char *begp;
	    const char *beg = p + 1;
	    char *op;
            char *abeg = NULL;
            unsigned int alen = 0;
	    const char *end, *colon;

	    op = o;
	    begp = p;
            end = may_be_function_name (p + 1, eos);
	    if (    end
                &&  handle_function (&op, &begp, end, eos))
	      {
		o = op;
		p = begp;
                MY_ASSERT_MSG (!(p1 = memchr (variable_buffer + line_offset, '\0', o - (variable_buffer + line_offset))),
                               ("line=%p o/exp_end=%p act_end=%p\n", variable_buffer + line_offset, o, p1));
		break;
	      }

	    /* Is there a variable reference inside the parens or braces?
	       If so, expand it before expanding the entire reference.  */

	    end = memchr (beg, closeparen, eos - beg);
	    if (end == 0)
              /* Unterminated variable reference.  */
              fatal (*expanding_var, _("unterminated variable reference"));
	    p1 = lindex (beg, end, '$');
	    if (p1 != 0)
	      {
		/* BEG now points past the opening paren or brace.
		   Count parens or braces until it is matched.  */
		int count = 0;
		for (p = beg; p < eos; ++p)
		  {
		    if (*p == openparen)
		      ++count;
		    else if (*p == closeparen && --count < 0)
		      break;
		  }
		/* If COUNT is >= 0, there were unmatched opening parens
		   or braces, so we go to the simple case of a variable name
		   such as `$($(a)'.  */
		if (count < 0)
		  {
                    unsigned int len;
                    char saved;

                     /* Expand the name.  */
                    saved = *p;
                    *(char *)p = '\0'; /* XXX: proove that this is safe! XXX2: shouldn't be necessary any longer! */
                    abeg = allocated_variable_expand_3 (beg, p - beg, &len, &alen);
                    beg = abeg;
                    end = beg + len;
                    *(char *)p = saved;
		  }
	      }
	    else
	      /* Advance P to the end of this reference.  After we are
                 finished expanding this one, P will be incremented to
                 continue the scan.  */
	      p = end;

	    /* This is not a reference to a built-in function and
	       any variable references inside are now expanded.
	       Is the resultant text a substitution reference?  */

	    colon = lindex (beg, end, ':');
	    if (colon)
	      {
		/* This looks like a substitution reference: $(FOO:A=B).  */
		const char *subst_beg, *subst_end, *replace_beg, *replace_end;

		subst_beg = colon + 1;
		subst_end = lindex (subst_beg, end, '=');
		if (subst_end == 0)
		  /* There is no = in sight.  Punt on the substitution
		     reference and treat this as a variable name containing
		     a colon, in the code below.  */
		  colon = 0;
		else
		  {
		    replace_beg = subst_end + 1;
		    replace_end = end;

		    /* Extract the variable name before the colon
		       and look up that variable.  */
		    v = lookup_variable (beg, colon - beg);
		    if (v == 0)
		      warn_undefined (beg, colon - beg);

                    /* If the variable is not empty, perform the
                       substitution.  */
		    if (v != 0 && *v->value != '\0')
		      {
			char *pattern, *replace, *ppercent, *rpercent;
			char *value = (v->recursive
                                       ? recursively_expand (v)
				       : v->value);

                        /* Copy the pattern and the replacement.  Add in an
                           extra % at the beginning to use in case there
                           isn't one in the pattern.  */
                        pattern = alloca (subst_end - subst_beg + 2);
                        *(pattern++) = '%';
                        memcpy (pattern, subst_beg, subst_end - subst_beg);
                        pattern[subst_end - subst_beg] = '\0';

                        replace = alloca (replace_end - replace_beg + 2);
                        *(replace++) = '%';
                        memcpy (replace, replace_beg,
                               replace_end - replace_beg);
                        replace[replace_end - replace_beg] = '\0';

                        /* Look for %.  Set the percent pointers properly
                           based on whether we find one or not.  */
			ppercent = find_percent (pattern);
			if (ppercent)
                          {
                            ++ppercent;
                            rpercent = find_percent (replace);
                            if (rpercent)
                              ++rpercent;
                          }
			else
                          {
                            ppercent = pattern;
                            rpercent = replace;
                            --pattern;
                            --replace;
                          }

                        o = patsubst_expand_pat (o, value, pattern, replace,
                                                 ppercent, rpercent);

			if (v->recursive)
			  free (value);
		      }
		  }
	      }

	    if (colon == 0)
	      /* This is an ordinary variable reference.
		 Look up the value of the variable.  */
		o = reference_variable (o, beg, end - beg);

	  if (abeg)
            recycle_variable_buffer (abeg, alen);
	  }
	  break;

	case '\0':
          assert (p == eos);
          break;

	default:
	  if (isblank ((unsigned char)p[-1])) /* XXX: This looks incorrect, previous is '$' */
	    break;

	  /* A $ followed by a random char is a variable reference:
	     $a is equivalent to $(a).  */
          o = reference_variable (o, p, 1);

	  break;
	}

      if (++p >= eos)
	break;
      p1 = memchr (p, '$', eos - p);
    }

  o = variable_buffer_output (o, "\0", 2); /* KMK: compensate for the strlen + 1 that was removed above. */
  *eolp = o - 2;
  MY_ASSERT_MSG (strchr (variable_buffer + line_offset, '\0') == *eolp,
                 ("expected=%d actual=%d\nlength=%ld string=%.*s\n",
                  (int)(*eolp - variable_buffer + line_offset), (int)strlen(variable_buffer + line_offset),
                  length, (int)length, string));
  return (variable_buffer + line_offset);
}
#endif /* CONFIG_WITH_VALUE_LENGTH */

/* Scan LINE for variable references and expansion-function calls.
   Build in `variable_buffer' the result of expanding the references and calls.
   Return the address of the resulting string, which is null-terminated
   and is valid only until the next time this function is called.  */

char *
variable_expand (const char *line)
{
#ifndef CONFIG_WITH_VALUE_LENGTH
  return variable_expand_string(NULL, line, (long)-1);
#else  /* CONFIG_WITH_VALUE_LENGTH */
  char *s;

  /* this function is abused a lot like this: variable_expand(""). */
  if (!*line)
    {
      s = variable_buffer_output (initialize_variable_output (), "\0", 2);
      return s - 2;
    }
  return variable_expand_string_2 (NULL, line, (long)-1, &s);
#endif /* CONFIG_WITH_VALUE_LENGTH */
}

/* Expand an argument for an expansion function.
   The text starting at STR and ending at END is variable-expanded
   into a null-terminated string that is returned as the value.
   This is done without clobbering `variable_buffer' or the current
   variable-expansion that is in progress.  */

char *
expand_argument (const char *str, const char *end)
{
#ifndef CONFIG_WITH_VALUE_LENGTH
  char *tmp, *alloc = NULL;
  char *r;
#endif

  if (str == end)
    return xstrdup("");

#ifndef CONFIG_WITH_VALUE_LENGTH
  if (!end || *end == '\0')
    return allocated_variable_expand (str);

  if (end - str + 1 > 1000)
    tmp = alloc = xmalloc (end - str + 1);
  else
    tmp = alloca (end - str + 1);

  memcpy (tmp, str, end - str);
  tmp[end - str] = '\0';

  r = allocated_variable_expand (tmp);

  if (alloc)
    free (alloc);

  return r;
#else  /* CONFIG_WITH_VALUE_LENGTH */
  if (!end)
      return allocated_variable_expand_2 (str, ~0U, NULL);
  return allocated_variable_expand_2 (str, end - str, NULL);
#endif /* CONFIG_WITH_VALUE_LENGTH */
}

/* Expand LINE for FILE.  Error messages refer to the file and line where
   FILE's commands were found.  Expansion uses FILE's variable set list.  */

char *
variable_expand_for_file (const char *line, struct file *file)
{
  char *result;
  struct variable_set_list *savev;
  const struct floc *savef;

  if (file == 0)
    return variable_expand (line);

  savev = current_variable_set_list;
  current_variable_set_list = file->variables;

  savef = reading_file;
  if (file->cmds && file->cmds->fileinfo.filenm)
    reading_file = &file->cmds->fileinfo;
  else
    reading_file = 0;

  result = variable_expand (line);

  current_variable_set_list = savev;
  reading_file = savef;

  return result;
}

#if defined (CONFIG_WITH_VALUE_LENGTH) || defined (CONFIG_WITH_COMMANDS_FUNC)
/* Expand LINE for FILE.  Error messages refer to the file and line where
   FILE's commands were found.  Expansion uses FILE's variable set list.

   Differs from variable_expand_for_file in that it takes a pointer to
   where in the variable buffer to start outputting the expanded string,
   and that it can returned the length of the string if you wish.  */

char *
variable_expand_for_file_2 (char *o, const char *line, unsigned int length,
                            struct file *file, unsigned int *value_lenp)
{
  char *result;
  struct variable_set_list *savev;
  const struct floc *savef;
  long len = length == ~0U ? (long)-1 : (long)length;
  char *eol;

  if (!o)
    o = initialize_variable_output();

  if (file == 0)
     result = variable_expand_string_2 (o, line, len, &eol);
  else
    {
      savev = current_variable_set_list;
      current_variable_set_list = file->variables;

      savef = reading_file;
      if (file->cmds && file->cmds->fileinfo.filenm)
        reading_file = &file->cmds->fileinfo;
      else
        reading_file = 0;

      result = variable_expand_string_2 (o, line, len, &eol);

      current_variable_set_list = savev;
      reading_file = savef;
    }

  if (value_lenp)
    *value_lenp = eol - result;

  return result;
}

#endif /* CONFIG_WITH_VALUE_LENGTH || CONFIG_WITH_COMMANDS_FUNC */
/* Like allocated_variable_expand, but for += target-specific variables.
   First recursively construct the variable value from its appended parts in
   any upper variable sets.  Then expand the resulting value.  */

static char *
variable_append (const char *name, unsigned int length,
                 const struct variable_set_list *set)
{
  const struct variable *v;
  char *buf = 0;

  /* If there's nothing left to check, return the empty buffer.  */
  if (!set)
    return initialize_variable_output ();

  /* Try to find the variable in this variable set.  */
  v = lookup_variable_in_set (name, length, set->set);

  /* If there isn't one, look to see if there's one in a set above us.  */
  if (!v)
    return variable_append (name, length, set->next);

  /* If this variable type is append, first get any upper values.
     If not, initialize the buffer.  */
  if (v->append)
    buf = variable_append (name, length, set->next);
  else
    buf = initialize_variable_output ();

  /* Append this value to the buffer, and return it.
     If we already have a value, first add a space.  */
  if (buf > variable_buffer)
    buf = variable_buffer_output (buf, " ", 1);
#ifdef CONFIG_WITH_VALUE_LENGTH
  assert (v->value_length == strlen (v->value));
#endif

  /* Either expand it or copy it, depending.  */
  if (! v->recursive || IS_VARIABLE_RECURSIVE_WITHOUT_DOLLAR (v))
#ifdef CONFIG_WITH_VALUE_LENGTH
    return variable_buffer_output (buf, v->value, v->value_length);
#else
    return variable_buffer_output (buf, v->value, strlen (v->value));
#endif

#ifdef CONFIG_WITH_VALUE_LENGTH
  variable_expand_string_2 (buf, v->value, v->value_length, &buf);
  return buf;
#else
  buf = variable_expand_string (buf, v->value, strlen (v->value));
  return (buf + strlen (buf));
#endif
}

#ifdef CONFIG_WITH_VALUE_LENGTH
/* Expands the specified string, appending it to the specified
   variable value. */
void
append_expanded_string_to_variable (struct variable *v, const char *value,
                                    unsigned int value_len, int append)
{
  char *p = (char *) memchr (value, '$', value_len);
  if (!p)
    /* fast path */
    append_string_to_variable (v,value, value_len, append);
  else if (value_len)
    {
      unsigned int off_dollar = p - (char *)value;

      /* Install a fresh variable buffer. */
      char *saved_buffer;
      unsigned int saved_buffer_length;
      install_variable_buffer (&saved_buffer, &saved_buffer_length);

      p = variable_buffer;
      if (append || !v->value_length)
        {
          /* Copy the current value into it and append a space. */
          if (v->value_length)
            {
              p = variable_buffer_output (p, v->value, v->value_length);
              p = variable_buffer_output (p, " ", 1);
            }

          /* Append the assignment value. */
          p = variable_buffer_output (p, value, off_dollar);
          variable_expand_string_2 (p, value + off_dollar, value_len - off_dollar, &p);
        }
      else
        {
          /* Expand the assignemnt value. */
          p = variable_buffer_output (p, value, off_dollar);
          variable_expand_string_2 (p, value + off_dollar, value_len - off_dollar, &p);

          /* Append a space followed by the old value. */
          p = variable_buffer_output (p, " ", 1);
          p = variable_buffer_output (p, v->value, v->value_length + 1) - 1;
        }

      /* Replace the variable with the variable buffer. */
#ifdef CONFIG_WITH_RDONLY_VARIABLE_VALUE
      if (v->rdonly_val)
        v->rdonly_val = 0;
      else
#endif
        free (v->value);
      v->value = variable_buffer;
      v->value_length = p - v->value;
      v->value_alloc_len = variable_buffer_length;
      VARIABLE_CHANGED(v);

      /* Restore the variable buffer, but without freeing the current. */
      variable_buffer = NULL;
      restore_variable_buffer (saved_buffer, saved_buffer_length);
    }
  /* else: Drop empty strings. Use $(NO_SUCH_VARIABLE) if a space is wanted. */
}
#endif /* CONFIG_WITH_VALUE_LENGTH */

static char *
allocated_variable_append (const struct variable *v)
{
  char *val;

  /* Construct the appended variable value.  */

  char *obuf = variable_buffer;
  unsigned int olen = variable_buffer_length;

  variable_buffer = 0;

  assert ((unsigned int)v->length == strlen (v->name)); /* bird */
  val = variable_append (v->name, strlen (v->name), current_variable_set_list);
  variable_buffer_output (val, "", 1);
  val = variable_buffer;

  variable_buffer = obuf;
  variable_buffer_length = olen;

  return val;
}

/* Like variable_expand_for_file, but the returned string is malloc'd.
   This function is called a lot.  It wants to be efficient.  */

char *
allocated_variable_expand_for_file (const char *line, struct file *file)
{
  char *value;

  char *obuf = variable_buffer;
  unsigned int olen = variable_buffer_length;

  variable_buffer = 0;

  value = variable_expand_for_file (line, file);

  variable_buffer = obuf;
  variable_buffer_length = olen;

  return value;
}

#ifdef CONFIG_WITH_VALUE_LENGTH
/* Handle the most common case in allocated_variable_expand_for_file
   specially and provide some additional string length features. */

char *
allocated_variable_expand_2 (const char *line, unsigned int length,
                             unsigned int *value_lenp)
{
  char *value;
  char *obuf = variable_buffer;
  unsigned int olen = variable_buffer_length;
  long len = length == ~0U ? -1L : (long)length;
  char *eol;

  variable_buffer = 0;

  value = variable_expand_string_2 (NULL, line, len, &eol);
  if (value_lenp)
    *value_lenp = eol - value;

  variable_buffer = obuf;
  variable_buffer_length = olen;

  return value;
}

/* Initially created for handling a special case for variable_expand_string2
   where the variable name is expanded and freed right afterwards.  This
   variant allows the variable_buffer to be recycled and thus avoid bothering
   with a slow free implementation. (Darwin is horrible slow.)  */

char *
allocated_variable_expand_3 (const char *line, unsigned int length,
                             unsigned int *value_lenp,
                             unsigned int *buffer_lengthp)
{
  char        *obuf = variable_buffer;
  unsigned int olen = variable_buffer_length;
  long         len  = (long)length;
  char *value;
  char *eol;

  variable_buffer = 0;

  value = variable_expand_string_2 (NULL, line, len, &eol);
  if (value_lenp)
    *value_lenp = eol - value;
  *buffer_lengthp = variable_buffer_length;

  variable_buffer = obuf;
  variable_buffer_length = olen;

  return value;
}

/* recycle a buffer. */

void
recycle_variable_buffer (char *buffer, unsigned int length)
{
    struct recycled_buffer *recycled = (struct recycled_buffer *)buffer;

    assert (!(length & 31));
    assert (length >= 384);
    recycled->length = length;
    recycled->next = recycled_head;
    recycled_head = recycled;
}

#endif /* CONFIG_WITH_VALUE_LENGTH */

/* Install a new variable_buffer context, returning the current one for
   safe-keeping.  */

void
install_variable_buffer (char **bufp, unsigned int *lenp)
{
  *bufp = variable_buffer;
  *lenp = variable_buffer_length;

  variable_buffer = 0;
  initialize_variable_output ();
}

#ifdef CONFIG_WITH_COMPILER
/* Same as install_variable_buffer, except we supply a size hint.  */

char *
install_variable_buffer_with_hint (char **bufp, unsigned int *lenp, unsigned int size_hint)
{
  struct recycled_buffer *recycled;
  char *buf;

  *bufp = variable_buffer;
  *lenp = variable_buffer_length;

  recycled = recycled_head;
  if (recycled)
    {
      recycled_head = recycled->next;
      variable_buffer_length = recycled->length;
      variable_buffer = buf = (char *)recycled;
    }
  else
    {
      if (size_hint < 512)
        variable_buffer_length = (size_hint + 1 + 63) & ~(unsigned int)63;
      else if (size_hint < 4096)
        variable_buffer_length = (size_hint + 1 + 1023) & ~(unsigned int)1023;
      else
        variable_buffer_length = (size_hint + 1 + 4095) & ~(unsigned int)4095;
      variable_buffer = buf = xmalloc (variable_buffer_length);
    }
  buf[0] = '\0';
  return buf;
}
#endif /* CONFIG_WITH_COMPILER */

/* Restore a previously-saved variable_buffer setting (free the
   current one). */

void
restore_variable_buffer (char *buf, unsigned int len)
{
#ifndef CONFIG_WITH_VALUE_LENGTH
  free (variable_buffer);
#else
  if (variable_buffer)
    recycle_variable_buffer (variable_buffer, variable_buffer_length);
#endif

  variable_buffer = buf;
  variable_buffer_length = len;
}


/* Used to make sure there is at least SIZE bytes of buffer space
   available starting at PTR.  */
char *
ensure_variable_buffer_space(char *ptr, unsigned int size)
{
  unsigned int offset = (unsigned int)(ptr - variable_buffer);
  assert(offset <= variable_buffer_length);
  if (variable_buffer_length - offset < size)
    {
      unsigned minlen = size + offset;
      variable_buffer_length *= 2;
      if (variable_buffer_length < minlen + 100)
        variable_buffer_length = (minlen + 100 + 63) & ~(unsigned int)63;
      variable_buffer = xrealloc (variable_buffer, variable_buffer_length);
      ptr = variable_buffer + offset;
    }
  return ptr;
}

