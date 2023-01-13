/* Output to stdout / stderr for GNU make
Copyright (C) 2013-2016 Free Software Foundation, Inc.
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

#include "makeint.h"
#include "job.h"

/* GNU make no longer supports pre-ANSI89 environments.  */

#include <assert.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#else
# include <sys/file.h>
#endif

#ifdef WINDOWS32
# include <windows.h>
# include <io.h>
# ifndef CONFIG_NEW_WIN_CHILDREN
#  include "sub_proc.h"
# else
#  include "w32/winchildren.h"
# endif
#endif /* WINDOWS32 */
#ifdef KBUILD_OS_WINDOWS
# include "console.h"
#endif

struct output *output_context = NULL;
unsigned int stdio_traced = 0;

#define OUTPUT_NONE (-1)

#define OUTPUT_ISSET(_out) ((_out)->out >= 0 || (_out)->err >= 0)

#ifdef HAVE_FCNTL_H
# define STREAM_OK(_s) ((fcntl (fileno (_s), F_GETFD) != -1) || (errno != EBADF))
#else
# define STREAM_OK(_s) 1
#endif


#if defined(KMK) && !defined(NO_OUTPUT_SYNC)
/* Non-negative if we're counting output lines.

   This is used by die_with_job_output to decide whether the initial build
   error needs to be repeated because there was too much output from parallel
   jobs between it and the actual make termination. */
int output_metered = -1;

static void meter_output_block (char const *buffer, size_t len)
{
  while (len > 0)
    {
      char *nl = (char *)memchr (buffer, '\n', len);
      size_t linelen;
      if (nl)
        {
          linelen = nl - buffer + 1;
          output_metered++;
        }
      else
          linelen = len;
      output_metered += linelen / 132;

      /* advance */
      buffer += linelen;
      len    -= linelen;
    }
}
#endif


#ifdef CONFIG_WITH_OUTPUT_IN_MEMORY
# define MEMBUF_MIN_SEG_SIZE  4096
# define MEMBUF_MAX_SEG_SIZE  (512*1024)
# define MEMBUF_MAX_MOVE_LEN  (  MEMBUF_MIN_SEG_SIZE \
                               - offsetof (struct output_segment, runs) \
                               - sizeof (struct output_run))
# define MEMBUF_MAX_TOTAL     (  sizeof (void *) <= 4 \
                               ? (size_t)512*1024 : (size_t)16*1024*1024 )

static void *acquire_semaphore (void);
static void  release_semaphore (void *);
static int   log_working_directory (int);

/* Is make's stdout going to the same place as stderr?
   Also, did we already sync_init (== -1)?  */
static int combined_output = -1;

/* Helper for membuf_reset and output_reset */
static membuf_reset (struct output *out)
{
  struct output_segment *seg;
  while ((seg = out->out.head_seg))
    {
     out->out.head_seg = seg->next;
     free (seg);
    }
  out->out.tail_seg = NULL;
  out->out.tail_run = NULL;
  out->out.head_run = NULL;
  out->out.left     = 0;
  out->out.total    = 0;

  while ((seg = out->err.head_seg))
    {
     out->err.head_seg = seg->next;
     free (seg);
    }
  out->err.tail_seg = NULL;
  out->err.tail_run = NULL;
  out->err.head_run = NULL;
  out->err.left     = 0;
  out->err.total    = 0;

  out->seqno = 0;
}

/* Used by die_with_job_output to suppress output when it shouldn't be repeated. */
void output_reset (struct output *out)
{
  if (out && (out->out.total || out->err.total))
    membuf_reset (out);
}

/* Internal worker for output_dump and membuf_dump_most. */
static void membuf_dump (struct output *out)
{
  if (out->out.total || out->err.total)
    {
      int traced = 0;
      struct output_run *err_run;
      struct output_run *out_run;
      FILE *prevdst;

      /* Try to acquire the semaphore.  If it fails, dump the output
         unsynchronized; still better than silently discarding it.
         We want to keep this lock for as little time as possible.  */
      void *sem = acquire_semaphore ();
# if defined (KBUILD_OS_WINDOWS) || defined (KBUILD_OS_OS2) || defined (KBUILD_OS_DOS)
      int prev_mode_out = _setmode (fileno (stdout), _O_BINARY);
      int prev_mode_err = _setmode (fileno (stderr), _O_BINARY);
# endif

# ifndef KMK /* this drives me bananas. */
      /* Log the working directory for this dump.  */
      if (print_directory_flag && output_sync != OUTPUT_SYNC_RECURSE)
        traced = log_working_directory (1);
# endif

      /* Work the out and err sequences in parallel. */
      out_run = out->out.head_run;
      err_run = out->err.head_run;
      prevdst = NULL;
      while (err_run || out_run)
        {
          FILE       *dst;
          const void *src;
          size_t      len;
          if (out_run && (!err_run || out_run->seqno <= err_run->seqno))
            {
              src = out_run + 1;
              len = out_run->len;
              dst = stdout;
              out_run = out_run->next;
            }
          else
            {
              src = err_run + 1;
              len = err_run->len;
              dst = stderr;
              err_run = err_run->next;
            }
          if (dst != prevdst)
            fflush(prevdst);
          prevdst = dst;
#ifdef KMK
          if (output_metered < 0)
            { /* likely */ }
          else
            meter_output_block (src, len);
#endif
# if 0 /* for debugging */
          while (len > 0)
            {
              const char *nl = (const char *)memchr (src, '\n', len);
              size_t line_len = nl ? nl - (const char *)src + 1 : len;
              char *tmp = (char *)xmalloc (1 + line_len + 1 + 1);
              tmp[0] = '{';
              memcpy (&tmp[1], src, line_len);
              tmp[1 + line_len] = '}';
#  ifdef KBUILD_OS_WINDOWS
              maybe_con_fwrite (tmp, 1 + line_len + 1, 1, dst);
#  else
              fwrite (tmp, 1 + line_len + 1, 1, dst);
#  endif
              free (tmp);
              src  = (const char *)src + line_len;
              len -= line_len;
            }
#else
#  ifdef KBUILD_OS_WINDOWS
          maybe_con_fwrite (src, len, 1, dst);
#  else
          fwrite (src, len, 1, dst);
#  endif
# endif
        }
      if (prevdst)
        fflush (prevdst);

# ifndef KMK /* this drives me bananas. */
      if (traced)
        log_working_directory (0);
# endif

      /* Exit the critical section.  */
# if defined (KBUILD_OS_WINDOWS) || defined (KBUILD_OS_OS2) || defined (KBUILD_OS_DOS)
      _setmode (fileno (stdout), prev_mode_out);
      _setmode (fileno (stderr), prev_mode_err);
# endif
      if (sem)
        release_semaphore (sem);

# ifdef KMK
      if (!out->dont_truncate)
        { /* likely */ }
      else return;
# endif

      /* Free the segments and reset the state. */
      membuf_reset (out);
    }
  else
    assert (out->out.head_seg == NULL && out->err.head_seg == NULL);
}

/* Writes up to LEN bytes to the given segment.
   Returns how much was actually written.  */
static size_t
membuf_write_segment (struct output_membuf *membuf, struct output_segment *seg,
                      const char *src, size_t len, unsigned int *pseqno)
{
  size_t written = 0;
  if (seg && membuf->left > 0)
    {
      struct output_run *run = membuf->tail_run;
      char  *dst = (char *)(run + 1) + run->len;
      assert ((uintptr_t)run - (uintptr_t)seg < seg->size);

      /* If the sequence number didn't change, then we can append
         to the current run without further considerations. */
      if (run->seqno == *pseqno)
          written = len;
      /* If the current run does not end with a newline, don't start a new
         run till we encounter one. */
      else if (dst[-1] != '\n')
        {
          char const *srcnl = (const char *)memchr (src, '\n', len);
          written = srcnl ? srcnl - src + 1 : len;
        }
      /* Try create a new empty run and append to it. */
      else
        {
          size_t const offnextrun = (  (uintptr_t)dst - (uintptr_t)(seg)
                                     + sizeof(void *) - 1)
                                  & ~(sizeof(void *) - 1);
          if (offnextrun > seg->size - sizeof (struct output_run) * 2)
            return 0; /* need new segment */

          run = run->next = (struct output_run *)((char *)seg + offnextrun);
          run->next  = NULL;
          run->seqno = ++(*pseqno);
          run->len   = 0;
          membuf->tail_run = run;
          membuf->left = seg->size - (offnextrun + sizeof (*run));
          dst = (char *)(run + 1);
          written = len;
        }

      /* Append to the current run. */
      if (written > membuf->left)
        written = membuf->left;
      memcpy (dst, src, written);
      run->len += written;
      membuf->left -= written;
    }
  return written;
}

/* Helper for membuf_write_new_segment and membuf_dump_most that figures out
   now much data needs to be moved from the previous run in order to make it
   end with a newline.  */
static size_t membuf_calc_move_len (struct output_run *tail_run)
{
  size_t to_move = 0;
  if (tail_run)
    {
      const char *data = (const char *)(tail_run + 1);
      size_t off = tail_run->len;
      while (off > 0 && data[off - 1] != '\n')
        off--;
      to_move = tail_run->len - off;
      if (to_move  >=  MEMBUF_MAX_MOVE_LEN)
        to_move = 0;
    }
  return to_move;
}

/* Allocates a new segment and writes to it.
   This will take care to make sure the previous run terminates with
   a newline so that we pass whole lines to fwrite when dumping. */
static size_t
membuf_write_new_segment (struct output_membuf *membuf, const char *src,
                          size_t len, unsigned int *pseqno)
{
  struct output_run     *prev_run = membuf->tail_run;
  struct output_segment *prev_seg = membuf->tail_seg;
  size_t const           to_move  = membuf_calc_move_len (prev_run);
  struct output_segment *new_seg;
  size_t written;
  char *dst;

  /* Figure the the segment size.  We start with MEMBUF_MIN_SEG_SIZE and double
     it each time till we reach MEMBUF_MAX_SEG_SIZE. */
  size_t const offset_runs = offsetof (struct output_segment, runs);
  size_t segsize = !prev_seg ? MEMBUF_MIN_SEG_SIZE
                 : prev_seg->size >= MEMBUF_MAX_SEG_SIZE ? MEMBUF_MAX_SEG_SIZE
                 : prev_seg->size * 2;
  while (   segsize < to_move + len + offset_runs + sizeof (struct output_run) * 2
         && segsize < MEMBUF_MAX_SEG_SIZE)
    segsize *= 2;

  /* Allocate the segment and link it and the first run. */
  new_seg = (struct output_segment *)xmalloc (segsize);
  new_seg->size = segsize;
  new_seg->next = NULL;
  new_seg->runs[0].next = NULL;
  if (!prev_seg)
    {
      membuf->head_seg = new_seg;
      membuf->head_run = &new_seg->runs[0];
    }
  else
    {
      prev_seg->next = new_seg;
      prev_run->next = &new_seg->runs[0];
    }
  membuf->tail_seg = new_seg;
  membuf->tail_run = &new_seg->runs[0];
  membuf->total += segsize;
  membuf->left = segsize - sizeof (struct output_run) - offset_runs;

  /* Initialize and write data to the first run. */
  dst = (char *)&new_seg->runs[0]; /* Try bypass gcc array size cleverness. */
  dst += sizeof (struct output_run);
  assert (MEMBUF_MAX_MOVE_LEN < MEMBUF_MIN_SEG_SIZE);
  if (to_move > 0)
    {
      /* Move to_move bytes from the previous run in hope that we'll get a
         newline to soon.  Afterwards call membuf_segment_write to work SRC. */
      assert (prev_run != NULL);
      assert (membuf->left >= to_move);
      prev_run->len -= to_move;
      new_seg->runs[0].len = to_move;
      new_seg->runs[0].seqno = prev_run->seqno;
      memcpy (dst, (const char *)(prev_run + 1) + prev_run->len, to_move);
      membuf->left -= to_move;

      written = membuf_write_segment (membuf, new_seg, src, len, pseqno);
    }
  else
    {
      /* Create a run with up to LEN from SRC. */
      written = len;
      if (written > membuf->left)
        written = membuf->left;
      new_seg->runs[0].len = written;
      new_seg->runs[0].seqno = ++(*pseqno);
      memcpy (dst, src, written);
      membuf->left -= written;
    }
  return written;
}

/* Worker for output_write that will dump most of the output when we hit
   MEMBUF_MAX_TOTAL on either of the two membuf structures, then free all the
   output segments.  Incomplete lines will be held over to the next buffers
   and copied into new segments. */
static void
membuf_dump_most (struct output *out)
{
  size_t out_to_move = membuf_calc_move_len (out->out.tail_run);
  size_t err_to_move = membuf_calc_move_len (out->err.tail_run);
  if (!out_to_move && !err_to_move)
    membuf_dump (out);
  else
    {
      /* Allocate a stack buffer for holding incomplete lines.  This should be
         fine since we're only talking about max 2 * MEMBUF_MAX_MOVE_LEN.
         The -1 on the sequence numbers, ise because membuf_write_new_segment
         will increment them before use. */
      unsigned int out_seqno = out_to_move ? out->out.tail_run->seqno - 1 : 0;
      unsigned int err_seqno = err_to_move ? out->err.tail_run->seqno - 1 : 0;
      char *tmp = alloca (out_to_move + err_to_move);
      if (out_to_move)
        {
          out->out.tail_run->len -= out_to_move;
          memcpy (tmp,
                  (char *)(out->out.tail_run + 1) + out->out.tail_run->len,
                  out_to_move);
        }
      if (err_to_move)
        {
          out->err.tail_run->len -= err_to_move;
          memcpy (tmp + out_to_move,
                  (char *)(out->err.tail_run + 1) + out->err.tail_run->len,
                  err_to_move);
        }

      membuf_dump (out);

      if (out_to_move)
        {
          size_t written = membuf_write_new_segment (&out->out, tmp,
                                                     out_to_move, &out_seqno);
          assert (written == out_to_move); (void)written;
        }
      if (err_to_move)
        {
          size_t written = membuf_write_new_segment (&out->err,
                                                     tmp + out_to_move,
                                                     err_to_move, &err_seqno);
          assert (written == err_to_move); (void)written;
        }
    }
}


/* write/fwrite like function, binary mode. */
ssize_t
output_write_bin (struct output *out, int is_err, const char *src, size_t len)
{
  size_t ret = len;
  if (!out || !out->syncout)
    {
      FILE *f = is_err ? stderr : stdout;
# if defined (KBUILD_OS_WINDOWS) || defined (KBUILD_OS_OS2) || defined (KBUILD_OS_DOS)
      /* On DOS platforms we need to disable \n -> \r\n converts that is common on
         standard output/error.  Also optimize for console output. */
      int saved_errno;
      int fd = fileno (f);
      int prev_mode = _setmode (fd, _O_BINARY);
      maybe_con_fwrite (src, len, 1, f);
      if (fflush (f) == EOF)
        ret = -1;
      saved_errno = errno;
      _setmode (fd, prev_mode);
      errno = saved_errno;
# else
      fwrite (src, len, 1, f);
      if (fflush (f) == EOF)
        ret = -1;
# endif
    }
  else
    {
      struct output_membuf *membuf = is_err ? &out->err : &out->out;
      while (len > 0)
        {
          size_t runlen = membuf_write_segment (membuf, membuf->tail_seg, src, len, &out->seqno);
          if (!runlen)
            {
              if (membuf->total < MEMBUF_MAX_TOTAL)
                runlen = membuf_write_new_segment (membuf, src, len, &out->seqno);
              else
                membuf_dump_most (out);
            }
          /* advance */
          len -= runlen;
          src += runlen;
        }
    }
  return ret;
}

#endif /* CONFIG_WITH_OUTPUT_IN_MEMORY */

/* write/fwrite like function, text mode. */
ssize_t
output_write_text (struct output *out, int is_err, const char *src, size_t len)
{
#ifdef CONFIG_WITH_OUTPUT_IN_MEMORY
# if defined (KBUILD_OS_WINDOWS) || defined (KBUILD_OS_OS2) || defined (KBUILD_OS_DOS)
  ssize_t ret = len;
  if (!out || !out->syncout)
    {
      /* ASSUME fwrite does the desired conversion. */
      FILE *f = is_err ? stderr : stdout;
# ifdef KBUILD_OS_WINDOWS
      if (maybe_con_fwrite (src, len, 1, f) < 0)
        ret = -1;
# else
      fwrite (src, len, 1, f);
# endif
      if (fflush (f) == EOF)
        ret = -1;
    }
  else
    {
      /* Work the buffer line by line, replacing each \n with \r\n. */
      while (len > 0)
        {
          const char *nl = memchr ( src, '\n', len);
          size_t line_len = nl ? nl - src : len;
          output_write_bin (out, is_err, src, line_len);
          if (!nl)
              break;
          output_write_bin (out, is_err, "\r\n", 2);
          len -= line_len + 1;
          src += line_len + 1;
        }
    }
  return ret;
# else
  return output_write_bin (out, is_err, src, len);
# endif
#else
  ssize_t ret = len;
  if (! out || ! out->syncout)
    {
      FILE *f = is_err ? stderr : stdout;
# ifdef KBUILD_OS_WINDOWS
      maybe_con_fwrite(src, len, 1, f);
# else
      fwrite (src, len, 1, f);
# endif
      fflush (f);
    }
  else
    {
      int fd = is_err ? out->err : out->out;
      int r;

      EINTRLOOP (r, lseek (fd, 0, SEEK_END));
      while (1)
        {
          EINTRLOOP (r, write (fd, src, len));
          if ((size_t)r == len || r <= 0)
            break;
          len -= r;
          src += r;
        }
    }
  return ret;
#endif
}



/* Write a string to the current STDOUT or STDERR.  */
static void
_outputs (struct output *out, int is_err, const char *msg)
{
#ifdef CONFIG_WITH_OUTPUT_IN_MEMORY
  output_write_text (out, is_err, msg, strlen (msg));
#else  /* !CONFIG_WITH_OUTPUT_IN_MEMORY */
  if (! out || ! out->syncout)
    {
      FILE *f = is_err ? stderr : stdout;
# ifdef KBUILD_OS_WINDOWS
      maybe_con_fwrite(msg, strlen(msg), 1, f);
# else
      fputs (msg, f);
# endif
      fflush (f);
    }
  else
    {
      int fd = is_err ? out->err : out->out;
      int len = strlen (msg);
      int r;

      EINTRLOOP (r, lseek (fd, 0, SEEK_END));
      while (1)
        {
          EINTRLOOP (r, write (fd, msg, len));
          if (r == len || r <= 0)
            break;
          len -= r;
          msg += r;
        }
    }
#endif /* !CONFIG_WITH_OUTPUT_IN_MEMORY */
}

/* Write a message indicating that we've just entered or
   left (according to ENTERING) the current directory.  */

static int
log_working_directory (int entering)
{
  static char *buf = NULL;
  static unsigned int len = 0;
  unsigned int need;
  const char *fmt;
  char *p;

  /* Get enough space for the longest possible output.  */
  need = strlen (program) + INTSTR_LENGTH + 2 + 1;
  if (starting_directory)
    need += strlen (starting_directory);

  /* Use entire sentences to give the translators a fighting chance.  */
  if (makelevel == 0)
    if (starting_directory == 0)
      if (entering)
        fmt = _("%s: Entering an unknown directory\n");
      else
        fmt = _("%s: Leaving an unknown directory\n");
    else
      if (entering)
        fmt = _("%s: Entering directory '%s'\n");
      else
        fmt = _("%s: Leaving directory '%s'\n");
  else
    if (starting_directory == 0)
      if (entering)
        fmt = _("%s[%u]: Entering an unknown directory\n");
      else
        fmt = _("%s[%u]: Leaving an unknown directory\n");
    else
      if (entering)
        fmt = _("%s[%u]: Entering directory '%s'\n");
      else
        fmt = _("%s[%u]: Leaving directory '%s'\n");

  need += strlen (fmt);

  if (need > len)
    {
      buf = xrealloc (buf, need);
      len = need;
    }

  p = buf;
  if (print_data_base_flag)
    {
      *(p++) = '#';
      *(p++) = ' ';
    }

  if (makelevel == 0)
    if (starting_directory == 0)
      sprintf (p, fmt , program);
    else
      sprintf (p, fmt, program, starting_directory);
  else if (starting_directory == 0)
    sprintf (p, fmt, program, makelevel);
  else
    sprintf (p, fmt, program, makelevel, starting_directory);

  _outputs (NULL, 0, buf);

  return 1;
}

/* Set a file descriptor to be in O_APPEND mode.
   If it fails, just ignore it.  */

static void
set_append_mode (int fd)
{
#if defined(F_GETFL) && defined(F_SETFL) && defined(O_APPEND)
  int flags = fcntl (fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl (fd, F_SETFL, flags | O_APPEND);
#endif
}


#ifndef NO_OUTPUT_SYNC

/* Semaphore for use in -j mode with output_sync. */
static sync_handle_t sync_handle = -1;

#define FD_NOT_EMPTY(_f) ((_f) != OUTPUT_NONE && lseek ((_f), 0, SEEK_END) > 0)

/* Set up the sync handle.  Disables output_sync on error.  */
static int
sync_init (void)
{
  int combined_output = 0;

#ifdef WINDOWS32
# ifdef CONFIG_NEW_WIN_CHILDREN
  if (STREAM_OK (stdout))
    {
      if (STREAM_OK (stderr))
        {
          char mtxname[256];
          sync_handle = create_mutex (mtxname, sizeof (mtxname));
          if (sync_handle != -1)
            {
              prepare_mutex_handle_string (mtxname);
              return same_stream (stdout, stderr);
            }
          perror_with_name ("output-sync suppressed: ", "create_mutex");
        }
      else
        perror_with_name ("output-sync suppressed: ", "stderr");
    }
  else
    perror_with_name ("output-sync suppressed: ", "stdout");
  output_sync = OUTPUT_SYNC_NONE;

# else  /* !CONFIG_NEW_WIN_CHILDREN */
  if ((!STREAM_OK (stdout) && !STREAM_OK (stderr))
      || (sync_handle = create_mutex ()) == -1)
    {
      perror_with_name ("output-sync suppressed: ", "stderr");
      output_sync = 0;
    }
  else
    {
      combined_output = same_stream (stdout, stderr);
      prepare_mutex_handle_string (sync_handle);
    }
# endif /* !CONFIG_NEW_WIN_CHILDREN */

#else
  if (STREAM_OK (stdout))
    {
      struct stat stbuf_o, stbuf_e;

      sync_handle = fileno (stdout);
      combined_output = (fstat (fileno (stdout), &stbuf_o) == 0
                         && fstat (fileno (stderr), &stbuf_e) == 0
                         && stbuf_o.st_dev == stbuf_e.st_dev
                         && stbuf_o.st_ino == stbuf_e.st_ino);
    }
  else if (STREAM_OK (stderr))
    sync_handle = fileno (stderr);
  else
    {
      perror_with_name ("output-sync suppressed: ", "stderr");
      output_sync = 0;
    }
#endif

  return combined_output;
}

#ifndef CONFIG_WITH_OUTPUT_IN_MEMORY
/* Support routine for output_sync() */
static void
pump_from_tmp (int from, FILE *to)
{
# ifdef KMK
  char buffer[8192];
# else
  static char buffer[8192];
#endif

# if defined (KBUILD_OS_WINDOWS) || defined (KBUILD_OS_OS2) || defined (KBUILD_OS_DOS)
  int prev_mode;

  /* "from" is opened by open_tmpfd, which does it in binary mode, so
     we need the mode of "to" to match that.  */
  prev_mode = _setmode (fileno (to), O_BINARY);
#endif

  if (lseek (from, 0, SEEK_SET) == -1)
    perror ("lseek()");

  while (1)
    {
      int len;
      EINTRLOOP (len, read (from, buffer, sizeof (buffer)));
      if (len < 0)
        perror ("read()");
      if (len <= 0)
        break;
#ifdef KMK
      if (output_metered < 0)
        { /* likely */ }
      else
        meter_output_block (buffer, len);
#endif
      if (fwrite (buffer, len, 1, to) < 1)
        {
          perror ("fwrite()");
          break;
        }
      fflush (to);
    }

# if defined (KBUILD_OS_WINDOWS) || defined (KBUILD_OS_OS2) || defined (KBUILD_OS_DOS)
  /* Switch "to" back to its original mode, so that log messages by
     Make have the same EOL format as without --output-sync.  */
  _setmode (fileno (to), prev_mode);
#endif
}
#endif /* CONFIG_WITH_OUTPUT_IN_MEMORY */

/* Obtain the lock for writing output.  */
static void *
acquire_semaphore (void)
{
  static struct flock fl;

  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 1;
  if (fcntl (sync_handle, F_SETLKW, &fl) != -1)
    return &fl;
#ifdef KBUILD_OS_DARWIN /* F_SETLKW isn't supported on pipes */
  if (errno != EBADF)
#endif  
  perror ("fcntl()");
  return NULL;
}

/* Release the lock for writing output.  */
static void
release_semaphore (void *sem)
{
  struct flock *flp = (struct flock *)sem;
  flp->l_type = F_UNLCK;
  if (fcntl (sync_handle, F_SETLKW, flp) == -1)
    perror ("fcntl()");
}

#ifndef CONFIG_WITH_OUTPUT_IN_MEMORY

/* Returns a file descriptor to a temporary file.  The file is automatically
   closed/deleted on exit.  Don't use a FILE* stream.  */
int
output_tmpfd (void)
{
  int fd = -1;
  FILE *tfile = tmpfile ();

  if (! tfile)
    {
#ifdef KMK
      if (output_context && output_context->syncout)
        output_context->syncout = 0; /* Avoid inifinit recursion. */
#endif
      pfatal_with_name ("tmpfile");
    }

  /* Create a duplicate so we can close the stream.  */
  fd = dup (fileno (tfile));
  if (fd < 0)
    {
#ifdef KMK
      if (output_context && output_context->syncout)
        output_context->syncout = 0; /* Avoid inifinit recursion. */
#endif
      pfatal_with_name ("dup");
    }

  fclose (tfile);

  set_append_mode (fd);

  return fd;
}

/* Adds file descriptors to the child structure to support output_sync; one
   for stdout and one for stderr as long as they are open.  If stdout and
   stderr share a device they can share a temp file too.
   Will reset output_sync on error.  */
static void
setup_tmpfile (struct output *out)
{
  /* Is make's stdout going to the same place as stderr?  */
  static int combined_output = -1;

  if (combined_output < 0)
    {
#ifdef KMK /* prevent infinite recursion if sync_init() calls perror_with_name. */
      combined_output = 0;
#endif
      combined_output = sync_init ();
    }

  if (STREAM_OK (stdout))
    {
      int fd = output_tmpfd ();
      if (fd < 0)
        goto error;
      CLOSE_ON_EXEC (fd);
      out->out = fd;
    }

  if (STREAM_OK (stderr))
    {
      if (out->out != OUTPUT_NONE && combined_output)
        out->err = out->out;
      else
        {
          int fd = output_tmpfd ();
          if (fd < 0)
            goto error;
          CLOSE_ON_EXEC (fd);
          out->err = fd;
        }
    }

  return;

  /* If we failed to create a temp file, disable output sync going forward.  */
 error:
  output_close (out);
  output_sync = OUTPUT_SYNC_NONE;
}

#endif /* CONFIG_WITH_OUTPUT_IN_MEMORY */

/* Synchronize the output of jobs in -j mode to keep the results of
   each job together. This is done by holding the results in temp files,
   one for stdout and potentially another for stderr, and only releasing
   them to "real" stdout/stderr when a semaphore can be obtained. */

void
output_dump (struct output *out)
{
#ifdef CONFIG_WITH_OUTPUT_IN_MEMORY
  membuf_dump (out);
#else
  int outfd_not_empty = FD_NOT_EMPTY (out->out);
  int errfd_not_empty = FD_NOT_EMPTY (out->err);

  if (outfd_not_empty || errfd_not_empty)
    {
# ifndef KMK /* this drives me bananas. */
      int traced = 0;
# endif

      /* Try to acquire the semaphore.  If it fails, dump the output
         unsynchronized; still better than silently discarding it.
         We want to keep this lock for as little time as possible.  */
      void *sem = acquire_semaphore ();

# ifndef KMK /* this drives me bananas. */
      /* Log the working directory for this dump.  */
      if (print_directory_flag && output_sync != OUTPUT_SYNC_RECURSE)
        traced = log_working_directory (1);
# endif

      if (outfd_not_empty)
        pump_from_tmp (out->out, stdout);
      if (errfd_not_empty && out->err != out->out)
        pump_from_tmp (out->err, stderr);

# ifndef KMK /* this drives me bananas. */
      if (traced)
        log_working_directory (0);
# endif

      /* Exit the critical section.  */
      if (sem)
        release_semaphore (sem);

# ifdef KMK
      if (!out->dont_truncate)
        { /* likely */ }
      else return;
# endif
      /* Truncate and reset the output, in case we use it again.  */
      if (out->out != OUTPUT_NONE)
        {
          int e;
          lseek (out->out, 0, SEEK_SET);
          EINTRLOOP (e, ftruncate (out->out, 0));
        }
      if (out->err != OUTPUT_NONE && out->err != out->out)
        {
          int e;
          lseek (out->err, 0, SEEK_SET);
          EINTRLOOP (e, ftruncate (out->err, 0));
        }
    }
#endif
}

# if defined(KMK) && !defined(CONFIG_WITH_OUTPUT_IN_MEMORY)
/* Used by die_with_job_output to suppress output when it shouldn't be repeated. */
void output_reset (struct output *out)
{
  if (out)
    {
      if (out->out != OUTPUT_NONE)
        {
          int e;
          lseek (out->out, 0, SEEK_SET);
          EINTRLOOP (e, ftruncate (out->out, 0));
        }
      if (out->err != OUTPUT_NONE && out->err != out->out)
        {
          int e;
          lseek (out->err, 0, SEEK_SET);
          EINTRLOOP (e, ftruncate (out->err, 0));
        }
    }
}
# endif
#endif /* NO_OUTPUT_SYNC */


/* Provide support for temporary files.  */

#ifndef HAVE_STDLIB_H
# ifdef HAVE_MKSTEMP
int mkstemp (char *template);
# else
char *mktemp (char *template);
# endif
#endif

FILE *
output_tmpfile (char **name, const char *template)
{
#ifdef HAVE_FDOPEN
  int fd;
#endif

#if defined HAVE_MKSTEMP || defined HAVE_MKTEMP
# define TEMPLATE_LEN   strlen (template)
#else
# define TEMPLATE_LEN   L_tmpnam
#endif
  *name = xmalloc (TEMPLATE_LEN + 1);
  strcpy (*name, template);

#if defined HAVE_MKSTEMP && defined HAVE_FDOPEN
  /* It's safest to use mkstemp(), if we can.  */
  fd = mkstemp (*name);
  if (fd == -1)
    return 0;
  return fdopen (fd, "w");
#else
# ifdef HAVE_MKTEMP
  (void) mktemp (*name);
# else
  (void) tmpnam (*name);
# endif

# ifdef HAVE_FDOPEN
  /* Can't use mkstemp(), but guard against a race condition.  */
  EINTRLOOP (fd, open (*name, O_CREAT|O_EXCL|O_WRONLY, 0600));
  if (fd == -1)
    return 0;
  return fdopen (fd, "w");
# else
  /* Not secure, but what can we do?  */
  return fopen (*name, "w");
# endif
#endif
}


/* This code is stolen from gnulib.
   If/when we abandon the requirement to work with K&R compilers, we can
   remove this (and perhaps other parts of GNU make!) and migrate to using
   gnulib directly.

   This is called only through atexit(), which means die() has already been
   invoked.  So, call exit() here directly.  Apparently that works...?
*/

/* Close standard output, exiting with status 'exit_failure' on failure.
   If a program writes *anything* to stdout, that program should close
   stdout and make sure that it succeeds before exiting.  Otherwise,
   suppose that you go to the extreme of checking the return status
   of every function that does an explicit write to stdout.  The last
   printf can succeed in writing to the internal stream buffer, and yet
   the fclose(stdout) could still fail (due e.g., to a disk full error)
   when it tries to write out that buffered data.  Thus, you would be
   left with an incomplete output file and the offending program would
   exit successfully.  Even calling fflush is not always sufficient,
   since some file systems (NFS and CODA) buffer written/flushed data
   until an actual close call.

   Besides, it's wasteful to check the return value from every call
   that writes to stdout -- just let the internal stream state record
   the failure.  That's what the ferror test is checking below.

   It's important to detect such failures and exit nonzero because many
   tools (most notably 'make' and other build-management systems) depend
   on being able to detect failure in other tools via their exit status.  */

static void
close_stdout (void)
{
  int prev_fail = ferror (stdout);
  int fclose_fail = fclose (stdout);

  if (prev_fail || fclose_fail)
    {
      if (fclose_fail)
        perror_with_name (_("write error: stdout"), "");
      else
        O (error, NILF, _("write error: stdout"));
      exit (MAKE_TROUBLE);
    }
}


void
output_init (struct output *out)
{
  if (out)
    {
#ifdef CONFIG_WITH_OUTPUT_IN_MEMORY
      out->out.head_seg  = NULL;
      out->out.tail_seg  = NULL;
      out->out.head_run  = NULL;
      out->out.tail_run  = NULL;
      out->err.head_seg  = NULL;
      out->err.tail_seg  = NULL;
      out->err.head_run  = NULL;
      out->err.tail_run  = NULL;
      out->err.total     = 0;
      out->out.total     = 0;
      out->seqno         = 0;
#else
      out->out = out->err = OUTPUT_NONE;
#endif
      out->syncout = !!output_sync;
#ifdef KMK
      out->dont_truncate = 0;
#endif
      return;
    }

  /* Configure this instance of make.  Be sure stdout is line-buffered.  */

#ifdef HAVE_SETVBUF
# ifdef SETVBUF_REVERSED
  setvbuf (stdout, _IOLBF, xmalloc (BUFSIZ), BUFSIZ);
# else  /* setvbuf not reversed.  */
  /* Some buggy systems lose if we pass 0 instead of allocating ourselves.  */
  setvbuf (stdout, 0, _IOLBF, BUFSIZ);
# endif /* setvbuf reversed.  */
#elif HAVE_SETLINEBUF
  setlinebuf (stdout);
#endif  /* setlinebuf missing.  */

  /* Force stdout/stderr into append mode.  This ensures parallel jobs won't
     lose output due to overlapping writes.  */
  set_append_mode (fileno (stdout));
  set_append_mode (fileno (stderr));

#ifdef HAVE_ATEXIT
  if (STREAM_OK (stdout))
    atexit (close_stdout);
#endif
}

void
output_close (struct output *out)
{
  if (! out)
    {
      if (stdio_traced)
        log_working_directory (0);
      return;
    }

#ifndef NO_OUTPUT_SYNC
  output_dump (out);
#endif

#ifdef CONFIG_WITH_OUTPUT_IN_MEMORY
  assert (out->out.total == 0);
  assert (out->out.head_seg == NULL);
  assert (out->err.total == 0);
  assert (out->err.head_seg == NULL);
#else
  if (out->out >= 0)
    close (out->out);
  if (out->err >= 0 && out->err != out->out)
    close (out->err);
#endif

  output_init (out);
}

/* We're about to generate output: be sure it's set up.  */
void
output_start (void)
{
#ifdef CONFIG_WITH_OUTPUT_IN_MEMORY
  /* If we're syncing output make sure the sempahore (win) is set up. */
  if (output_context && output_context->syncout)
    if (combined_output < 0)
      combined_output = sync_init ();
#else
#ifndef NO_OUTPUT_SYNC
  /* If we're syncing output make sure the temporary file is set up.  */
  if (output_context && output_context->syncout)
    if (! OUTPUT_ISSET(output_context))
      setup_tmpfile (output_context);
#endif
#endif

#ifndef KMK
  /* If we're not syncing this output per-line or per-target, make sure we emit
     the "Entering..." message where appropriate.  */
  if (output_sync == OUTPUT_SYNC_NONE || output_sync == OUTPUT_SYNC_RECURSE)
#else
  /* Indiscriminately output "Entering..." and "Leaving..." message for each
     command line or target is plain annoying!  And when there is no recursion
     it's actually inappropriate.   Haven't got a simple way of detecting that,
     so back to the old behavior for now.  [bird] */
#endif
    if (! stdio_traced && print_directory_flag)
      stdio_traced = log_working_directory (1);
}

void
outputs (int is_err, const char *msg)
{
  if (! msg || *msg == '\0')
    return;

  output_start ();

  _outputs (output_context, is_err, msg);
}


static struct fmtstring
  {
    char *buffer;
    size_t size;
  } fmtbuf = { NULL, 0 };

static char *
get_buffer (size_t need)
{
  /* Make sure we have room.  NEED includes space for \0.  */
  if (need > fmtbuf.size)
    {
      fmtbuf.size += need * 2;
      fmtbuf.buffer = xrealloc (fmtbuf.buffer, fmtbuf.size);
    }

  fmtbuf.buffer[need-1] = '\0';

  return fmtbuf.buffer;
}

/* Print a message on stdout.  */

void
message (int prefix, size_t len, const char *fmt, ...)
{
  va_list args;
  char *p;

  len += strlen (fmt) + strlen (program) + INTSTR_LENGTH + 4 + 1 + 1;
  p = get_buffer (len);

  if (prefix)
    {
      if (makelevel == 0)
        sprintf (p, "%s: ", program);
      else
        sprintf (p, "%s[%u]: ", program, makelevel);
      p += strlen (p);
    }

  va_start (args, fmt);
  vsprintf (p, fmt, args);
  va_end (args);

  strcat (p, "\n");

  assert (fmtbuf.buffer[len-1] == '\0');
  outputs (0, fmtbuf.buffer);
}

/* Print an error message.  */

void
error (const floc *flocp, size_t len, const char *fmt, ...)
{
  va_list args;
  char *p;

  len += (strlen (fmt) + strlen (program)
          + (flocp && flocp->filenm ? strlen (flocp->filenm) : 0)
          + INTSTR_LENGTH + 4 + 1 + 1);
  p = get_buffer (len);

  if (flocp && flocp->filenm)
    sprintf (p, "%s:%lu: ", flocp->filenm, flocp->lineno + flocp->offset);
  else if (makelevel == 0)
    sprintf (p, "%s: ", program);
  else
    sprintf (p, "%s[%u]: ", program, makelevel);
  p += strlen (p);

  va_start (args, fmt);
  vsprintf (p, fmt, args);
  va_end (args);

  strcat (p, "\n");

  assert (fmtbuf.buffer[len-1] == '\0');
  outputs (1, fmtbuf.buffer);
}

/* Print an error message and exit.  */

void
fatal (const floc *flocp, size_t len, const char *fmt, ...)
{
  va_list args;
  const char *stop = _(".  Stop.\n");
  char *p;

  len += (strlen (fmt) + strlen (program)
          + (flocp && flocp->filenm ? strlen (flocp->filenm) : 0)
          + INTSTR_LENGTH + 8 + strlen (stop) + 1);
  p = get_buffer (len);

  if (flocp && flocp->filenm)
    sprintf (p, "%s:%lu: *** ", flocp->filenm, flocp->lineno + flocp->offset);
  else if (makelevel == 0)
    sprintf (p, "%s: *** ", program);
  else
    sprintf (p, "%s[%u]: *** ", program, makelevel);
  p += strlen (p);

  va_start (args, fmt);
  vsprintf (p, fmt, args);
  va_end (args);

  strcat (p, stop);

  assert (fmtbuf.buffer[len-1] == '\0');
  outputs (1, fmtbuf.buffer);

  die (MAKE_FAILURE);
}

/* Print an error message from errno.  */

void
perror_with_name (const char *str, const char *name)
{
  const char *err = strerror (errno);
  OSSS (error, NILF, _("%s%s: %s"), str, name, err);
}

/* Print an error message from errno and exit.  */

void
pfatal_with_name (const char *name)
{
  const char *err = strerror (errno);
  OSS (fatal, NILF, _("%s: %s"), name, err);

  /* NOTREACHED */
}
