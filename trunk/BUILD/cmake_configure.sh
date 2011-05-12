#!/bin/sh

# Ensure cmake and perl are there
cmake -P cmake/check_minimal_version.cmake >/dev/null 2>&1 || HAVE_CMAKE=no
perl --version >/dev/null 2>&1 || HAVE_PERL=no
scriptdir=`dirname $0`
if test "$HAVE_CMAKE" = "no"
then
  echo "CMake is required to build MySQL."
  exit 1
elif test "$HAVE_PERL" = "no"
then
  echo "Perl is required to build MySQL using the configure to CMake translator."
  exit 1
else
  perl $scriptdir/cmake/configure.pl "$@"
fi

