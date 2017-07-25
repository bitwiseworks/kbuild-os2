# kBuild for OS/2

This repository contains the source code for the kBuild project with the patches
for the OS2 operating system.

kBuild is a makefile framework for writing simple makefiles for complex tasks.
The original project home is http://trac.netlabs.org/kbuild/wiki. The original
kBuild source code is imported into this repository as is (with a few
modifications mentioned below) and gets periodically synchronized to pick up
latest changes from upstream. These modifications include:

- All pre-built binaries for various platforms are removed as we only build
kBuild on OS/2 and re-generate all the binaries during the build process so
it makes no sense to store them in a repo. As opposed to the original
repository, this one is not intended as a means of kBuild installation, we
provide a set of RPM packages instead.
