# This is a -*-Makefile-*-, or close enough
#
# Copyright (C) 1997, 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006,
# 2007, 2008, 2009, 2010 Free Software Foundation, Inc.
# This file is part of GNU Make.
#
# GNU Make is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation; either version 3 of the License, or (at your option) any later
# version.
#
# GNU Make is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program.  If not, see <http://www.gnu.org/licenses/>.

AUTOMAKE_OPTIONS = 1.8 dist-bzip2 check-news
ACLOCAL_AMFLAGS	=  -I config

MAKE_HOST =	@MAKE_HOST@

# Only process if target is MS-Windows
if WINDOWSENV
  MAYBE_W32 =	w32
  W32INC =	-I $(top_srcdir)/w32/include
  W32LIB =	-Lw32 -lw32
endif

# we can safely drop doc and po when bootstrapping kmk.
# SUBDIRS =	glob config po doc $(MAYBE_W32)
SUBDIRS =	glob config $(MAYBE_W32)

bin_PROGRAMS =	kmk kmk_redirect

if USE_CUSTOMS
  remote =	remote-cstms.c
else
  remote =	remote-stub.c
endif


kmk_SOURCES =	ar.c arscan.c commands.c default.c dir.c expand.c file.c \
		function.c getopt.c getopt1.c implicit.c job.c main.c \
		misc.c read.c remake.c $(remote) rule.c signame.c \
		strcache.c variable.c version.c vpath.c hash.c \
		\
		expreval.c \
		incdep.c \
		strcache2.c \
		alloccache.c \
		kbuild.c \
		kbuild-object.c \
		electric.c \
		../lib/md5.c \
		../lib/kDep.c \
		../lib/kbuild_version.c \
		../lib/dos2unix.c \
		../lib/maybe_con_fwrite.c \
		\
		kmkbuiltin.c \
		kmkbuiltin/append.c \
		kmkbuiltin/cat.c \
		kmkbuiltin/chmod.c \
		kmkbuiltin/cmp.c \
		kmkbuiltin/cmp_util.c \
		kmkbuiltin/cp.c \
		kmkbuiltin/cp_utils.c \
		kmkbuiltin/echo.c \
		kmkbuiltin/expr.c \
		kmkbuiltin/install.c \
		kmkbuiltin/kDepIDB.c \
		kmkbuiltin/kDepObj.c \
		kmkbuiltin/ln.c \
		kmkbuiltin/md5sum.c \
		kmkbuiltin/mkdir.c \
		kmkbuiltin/mv.c \
		kmkbuiltin/printf.c \
		kmkbuiltin/redirect.c \
		kmkbuiltin/rm.c \
		kmkbuiltin/rmdir.c \
		kmkbuiltin/sleep.c \
		kmkbuiltin/test.c \
		kmkbuiltin/touch.c \
               \
		kmkbuiltin/err.c \
		kmkbuiltin/fts.c \
		kmkbuiltin/setmode.c \
		kmkbuiltin/strmode.c \
		kmkbuiltin/strlcpy.c \
		kmkbuiltin/osdep.c \
		kmkbuiltin/kbuild_protection.c \
		kmkbuiltin/common-env-and-cwd-opt.c

kmk_redirect_SOURCES = kmkbuiltin/redirect.c \
		kmkbuiltin/common-env-and-cwd-opt.c \
		kmkbuiltin/err.c \
		../lib/kbuild_version.c
kmk_redirect_CFLAGS = -UKMK


EXTRA_kmk_SOURCES = vmsjobs.c remote-stub.c remote-cstms.c

noinst_HEADERS = commands.h dep.h filedef.h job.h make.h rule.h variable.h \
		debug.h getopt.h gettext.h hash.h

#kmk_LDADD =	@LIBOBJS@ @ALLOCA@ $(GLOBLIB) @GETLOADAVG_LIBS@ @LIBINTL@
kmk_LDADD =	@LIBOBJS@ @ALLOCA@ $(GLOBLIB) @GETLOADAVG_LIBS@
# Only process if target is MS-Windows
if WINDOWSENV
    kmk_LDADD += $(W32LIB)
endif

man_MANS =	make.1

# org - DEFS =		-DLOCALEDIR=\"$(localedir)\" -DLIBDIR=\"$(libdir)\" -DINCLUDEDIR=\"$(includedir)\" @DEFS@
DEFS = \
	-DNO_ARCHIVES \
	-DEXPERIMENTAL \
	-DCONFIG_WITH_TOUPPER_TOLOWER \
	-DCONFIG_WITH_DEFINED \
	-DCONFIG_WITH_EXPLICIT_MULTITARGET \
	-DCONFIG_WITH_DOT_MUST_MAKE \
	-DCONFIG_WITH_PREPEND_ASSIGNMENT \
	-DCONFIG_WITH_LOCAL_VARIABLES \
	-DCONFIG_WITH_2ND_TARGET_EXPANSION \
	-DCONFIG_WITH_ALLOC_CACHES \
	-DCONFIG_WITH_STRCACHE2 \
	\
	-DKMK \
	-DKMK_HELPERS \
	-DCONFIG_NO_DEFAULT_SUFFIXES \
	-DCONFIG_NO_DEFAULT_PATTERN_RULES	\
	-DCONFIG_NO_DEFAULT_TERMINAL_RULES \
	-DCONFIG_NO_DEFAULT_SUFFIX_RULES \
	-DCONFIG_NO_DEFAULT_VARIABLES \
	-DCONFIG_WITH_EXTENDED_NOTPARALLEL \
	-DCONFIG_WITH_INCLUDEDEP \
	-DCONFIG_WITHOUT_THREADS \
	-DCONFIG_WITH_VALUE_LENGTH \
	\
	-DCONFIG_WITH_ABSPATHEX \
	-DCONFIG_WITH_COMMANDS_FUNC \
	-DCONFIG_WITH_DATE \
	-DCONFIG_WITH_DEFINED_FUNCTIONS \
	-DCONFIG_WITH_EVALPLUS \
	-DCONFIG_WITH_FILE_SIZE \
	-DCONFIG_WITH_LOOP_FUNCTIONS \
	-DCONFIG_WITH_MATH \
	-DCONFIG_WITH_NANOTS \
	-DCONFIG_WITH_ROOT_FUNC \
	-DCONFIG_WITH_RSORT \
	-DCONFIG_WITH_STACK \
	-DCONFIG_WITH_STRING_FUNCTIONS \
	-DCONFIG_WITH_WHERE_FUNCTION \
	-DCONFIG_WITH_WHICH \
	-DCONFIG_WITH_XARGS \
	\
	-DCONFIG_WITH_COMPARE \
	-DCONFIG_WITH_SET_CONDITIONALS \
	-DCONFIG_WITH_IF_CONDITIONALS \
	-DCONFIG_WITH_PRINTF \
	-DCONFIG_WITH_MINIMAL_STATS \
	-DCONFIG_PRETTY_COMMAND_PRINTING \
	-DCONFIG_WITH_PRINT_STATS_SWITCH \
	-DCONFIG_WITH_PRINT_TIME_SWITCH \
	-DCONFIG_WITH_RDONLY_VARIABLE_VALUE \
	-DCONFIG_WITH_LAZY_DEPS_VARS \
	\
	-DKBUILD_TYPE=\"$(KBUILD_TYPE)\" \
	-DKBUILD_HOST=\"$(KBUILD_TARGET)\" \
	-DKBUILD_HOST_ARCH=\"$(KBUILD_TARGET_ARCH)\" \
	-DKBUILD_HOST_CPU=\"$(KBUILD_TARGET_CPU)\" \
	\
	-DKBUILD_SVN_REV=1 \
	-DKBUILD_VERSION_MAJOR=0 \
	-DKBUILD_VERSION_MINOR=1 \
	-DKBUILD_VERSION_PATCH=9998 \
	\
	-DCONFIG_WITH_KMK_BUILTIN \
    @DEFS@

AM_CPPFLAGS =	$(GLOBINC) -I$(srcdir)/../lib -I$(srcdir)/../lib/kStuff/include
# Only process if target is MS-Windows
if WINDOWSENV
    AM_CPPFLAGS +=	$(W32INC)
endif


# Extra stuff to include in the distribution.

EXTRA_DIST =	README build.sh.in $(man_MANS) \
		README.customs README.OS2 \
		SCOPTIONS SMakefile \
		README.Amiga Makefile.ami config.ami make.lnk amiga.c amiga.h \
		README.DOS Makefile.DOS configure.bat dosbuild.bat configh.dos\
		README.W32 NMakefile config.h.W32 build_w32.bat subproc.bat \
		make_msvc_net2003.sln make_msvc_net2003.vcproj \
		readme.vms makefile.vms makefile.com config.h-vms \
		vmsdir.h vmsfunctions.c vmsify.c

# This is built during configure, but behind configure's back

DISTCLEANFILES = build.sh

# Forward targets

html:
	cd doc && $(MAKE) $(AM_MAKEFLAGS) $@

.PHONY: html

# --------------- Internationalization Section

localedir =	$(datadir)/locale

# --------------- Local INSTALL Section

# If necessary, change the gid of the app and turn on the setgid flag.
#

# Whether or not make needs to be installed setgid.
# The value should be either `true' or `false'.
# On many systems, the getloadavg function (used to implement the `-l'
# switch) will not work unless make is installed setgid kmem.
#
inst_setgid = @NEED_SETGID@

# Install make setgid to this group so it can get the load average.
#
inst_group = @KMEM_GROUP@

install-exec-local:
	@if $(inst_setgid); then \
	   app=$(DESTDIR)$(bindir)/`echo $(bin_PROGRAMS)|sed '$(transform)'`; \
	   if chgrp $(inst_group) $$app && chmod g+s $$app; then \
	     echo "chgrp $(inst_group) $$app && chmod g+s $$app"; \
	   else \
	     echo "$$app needs to be owned by group $(inst_group) and setgid;"; \
	     echo "otherwise the \`-l' option will probably not work."; \
	     echo "You may need special privileges to complete the installation"; \
	     echo "of $$app."; \
	   fi; \
	 else true; fi

# --------------- Local DIST Section

# Install the w32 and tests subdirectories
#
dist-hook:
	(cd $(srcdir); \
	 sub=`find w32 tests -follow \( -name CVS -prune -o -name .cvsignore -o -name work -prune \) -o \( -name \*.orig -o -name \*.rej -o -name \*~ -prune \) -o -type f -print`; \
	 tar chf - $$sub) \
	| (cd $(distdir); tar xfBp -)


# --------------- Local CHECK Section

check-local: check-regression check-loadavg
	@banner=" Regression PASSED: GNU Make $(VERSION) ($(MAKE_HOST)) built with $(CC) "; \
	dashes=`echo "$$banner" | sed s/./=/g`; \
	echo; \
	echo "$$dashes"; \
	echo "$$banner"; \
	echo "$$dashes"; \
	echo

.PHONY: check-loadavg check-regression

check-loadavg: loadavg$(EXEEXT)
	@echo The system uptime program believes the load average to be:
	-uptime
	@echo The GNU load average checking code thinks:
	-./loadavg$(EXEEXT)

# The loadavg function is invoked during "make check" to test getloadavg.
check_PROGRAMS = loadavg
nodist_loadavg_SOURCES = getloadavg.c
loadavg_CPPFLAGS = -DTEST
loadavg_LDADD = @GETLOADAVG_LIBS@

# > check-regression
#
# Look for the make test suite, and run it if found and we can find perl.
# If we're building outside the tree, we use symlinks to make a local copy of
# the test suite.  Unfortunately the test suite itself isn't localizable yet.
#
MAKETESTFLAGS =

check-regression:
	@if test -f "$(srcdir)/tests/run_make_tests"; then \
	  if $(PERL) -v >/dev/null 2>&1; then \
	    case `cd $(srcdir); pwd` in `pwd`) : ;; \
	      *) test -d tests || mkdir tests; \
		 rm -f srctests; \
		 if ln -s "$(srcdir)/tests" srctests; then \
		   for f in run_make_tests run_make_tests.pl test_driver.pl scripts; do \
		     rm -f tests/$$f; ln -s ../srctests/$$f tests; \
		   done; fi ;; \
	    esac; \
	    echo "cd tests && $(PERL) ./run_make_tests.pl -make ../make$(EXEEXT) $(MAKETESTFLAGS)"; \
	    cd tests && $(PERL) ./run_make_tests.pl -make ../make$(EXEEXT) $(MAKETESTFLAGS); \
	  else \
	    echo "Can't find a working Perl ($(PERL)); the test suite requires Perl."; \
	  fi; \
	 else \
	  echo "Can't find the GNU Make test suite ($(srcdir)/tests)."; \
	 fi


# --------------- Maintainer's Section

# Tell automake that I haven't forgotten about this file and it will be
# created before we build a distribution (see maintMakefile in the CVS
# distribution).

README:

@MAINT_MAKEFILE@
