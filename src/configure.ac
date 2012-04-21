# Generate configure by running
# aclocal -I ./m4 --force && glibtoolize -i && autoconf --force
#
AC_INIT(echotest, 0.1)

# Init pkg-config 
PKG_PROG_PKG_CONFIG()

AC_PROG_CC

# Codec2
AC_CHECK_HEADERS([codec2.h])
AC_CHECK_LIB([codec2], [codec2_create])

# Threading
ACX_PTHREAD()

# Math library functions
AC_CHECK_HEADERS([math.h], [INSERT_MATH_HEADER="#include <math.h>"])
AC_CHECK_HEADERS([float.h])

AC_CHECK_LIB([m], [cos])
# Some platforms still seem to lack the basic single precision trig and power related function.
AC_SEARCH_LIBS([sinf], [m], AC_DEFINE([HAVE_SINF], [1], [Define to 1 if you have the sinf() function.]))
AC_SEARCH_LIBS([cosf], [m], AC_DEFINE([HAVE_COSF], [1], [Define to 1 if you have the cosf() function.]))
AC_SEARCH_LIBS([tanf], [m], AC_DEFINE([HAVE_TANF], [1], [Define to 1 if you have the tanf() function.]))
AC_SEARCH_LIBS([asinf], [m], AC_DEFINE([HAVE_ASINF], [1], [Define to 1 if you have the asinf() function.]))
AC_SEARCH_LIBS([acosf], [m], AC_DEFINE([HAVE_ACOSF], [1], [Define to 1 if you have the acosf() function.]))
AC_SEARCH_LIBS([atanf], [m], AC_DEFINE([HAVE_ATANF], [1], [Define to 1 if you have the atanf() function.]))
AC_SEARCH_LIBS([atan2f], [m], AC_DEFINE([HAVE_ATAN2F], [1], [Define to 1 if you have the atan2f() function.]))
AC_SEARCH_LIBS([ceilf], [m], AC_DEFINE([HAVE_CEILF], [1], [Define to 1 if you have the ceilf() function.]))
AC_SEARCH_LIBS([floorf], [m], AC_DEFINE([HAVE_FLOORF], [1], [Define to 1 if you have the floorf() function.]))
AC_SEARCH_LIBS([powf], [m], AC_DEFINE([HAVE_POWF], [1], [Define to 1 if you have the powf() function.]))
AC_SEARCH_LIBS([expf], [m], AC_DEFINE([HAVE_EXPF], [1], [Define to 1 if you have the expf() function.]))
AC_SEARCH_LIBS([logf], [m], AC_DEFINE([HAVE_LOGF], [1], [Define to 1 if you have the logf() function.]))
AC_SEARCH_LIBS([log10f], [m], AC_DEFINE([HAVE_LOG10F], [1], [Define to 1 if you have the log10f() function.]))

# Needed packages
PKG_CHECK_MODULES([PORTAUDIO], [portaudio-2.0])
PKG_CHECK_MODULES([SRC], [samplerate])

AC_OUTPUT(Makefile)
