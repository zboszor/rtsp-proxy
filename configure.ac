AC_INIT([rtsp-proxy],[1.0])
AC_DEFINE(VERSION_TITLE,["Release Title"],[Release Title])
AC_PREREQ([2.50])
AC_CONFIG_SRCDIR(config.h.in)
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR(.)

AM_INIT_AUTOMAKE()
AC_CONFIG_HEADERS(config.h)

AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MAKE_SET
AM_PROG_CC_C_O
LT_INIT

dnl Checks for header files.
AC_HEADER_STDC

PKG_CHECK_MODULES(FFMPEG, libavformat libavcodec libavutil libswscale)
AC_SUBST(FFMPEG_CFLAGS)
AC_SUBST(FFMPEG_LIBS)

AC_CHECK_HEADERS(iniparser/iniparser.h, [],
	[AC_CHECK_HEADERS(iniparser.h, [], [AC_MSG_ERROR([iniparser.h not found])])])
AC_CHECK_LIB(iniparser, iniparser_load, [], [AC_MSG_ERROR([iniparser library not found])], [])

AC_CONFIG_FILES([
	Makefile
])
AC_OUTPUT
