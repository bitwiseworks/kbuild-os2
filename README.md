# kBuild for OS/2

This repository contains the source code for the kBuild project with the patches
for the OS2 operating system.

kBuild is a makefile framework for writing simple makefiles for complex tasks.
The original project home is http://trac.netlabs.org/kbuild/wiki. The original
kBuild source code is imported into this repository as is (with a few
modifications mentioned below) and gets periodically synchronized to pick up
latest changes from upstream. The modifications applied when importing include:

- Removed `/kBuild/bin` directory containing pre-built binaries for various
platforms as we only build kBuild on OS/2 and re-generate all the binaries
during the build process so it makes no sense to store them in a repo.
As opposed to the original repository, this one is not intended as a means of
installing kBuild -- we provide a set of RPM packages instead.

- Removed `/dist` directory containing installation files for platforms we
don't maintain.

- Removed `/SlickEdit` directory containing irrelevant kBuild syntax
highlighting for SlickEdit.

Note also that the original repository pulls an external repository
http://trac.netlabs.org/kstuff/ into `/src/lib/kStuff` which we import as raw
source now but it will be redone as a git submodule once kStuff is on GitHub.
