AC_INIT([omphalos],[0.99.12],[omphalos-dev@googlegroups.com],[omphalos],[https://nick-black.com/dankwiki/index.php/Omphalos])
AM_INIT_AUTOMAKE([-Wall foreign dist-xz])
AC_CONFIG_HEADERS([config.h:config.in])
AC_USE_SYSTEM_EXTENSIONS
AC_PROG_CC
AC_PROG_CC_C_O
AC_PROG_CC_C99
AC_PROG_INSTALL
AC_C_INLINE
AC_C_RESTRICT
AC_TYPE_UINT8_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_FUNC_MALLOC
AC_FUNC_MMAP
AC_FUNC_REALLOC
AC_HEADER_STDBOOL
AC_SYS_LARGEFILE
AX_PROG_XSLTPROC
if test "$XSLTPROC" = "" ; then
	AC_MSG_ERROR([Cannot find xsltproc.])
fi
AX_CHECK_DOCBOOK_XSLT
if test "x$HAVE_DOCBOOK_XSLT_current" != "xyes"; then
	AC_MSG_ERROR([Cannot find docbook-xsl.])
fi
AC_CHECK_LIB(iw,iw_get_kernel_we_version, [have_libiw=yes],
	     [AC_MSG_ERROR([Cannot find libiw.])])
	LIBS+=" -liw"
AC_CHECK_LIB(cap,cap_get_proc, [have_libcap=yes],
	     [AC_MSG_ERROR([Cannot find libcap.])])
	LIBS+=" -lcap"
AC_CHECK_LIB(pcap,pcap_create, [have_pcap=yes],
	     [AC_MSG_ERROR([Cannot find libpcap.])])
	LIBS+=" -lpcap"
AC_CHECK_LIB(pciaccess,pci_system_init, [have_pciaccess=yes],
	     [AC_MSG_ERROR([Cannot find libpciaccess.])])
	LIBS+=" -lpciaccess"
PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES(LIBNL3, libnl-3.0 >= 3.0, [have_libnl3=yes])
	CFLAGS+=" $LIBNL3_CFLAGS"
	LIBS+=" $LIBNL3_LIBS"
PKG_CHECK_MODULES(LIBNL3GEN, libnl-genl-3.0 >= 3.0, [have_libnl3gen=yes] )
	CFLAGS+=" $LIBNL3GEN_CFLAGS"
	LIBS+=" $LIBNL3GEN_LIBS"
# no libsysfs.pc anywhere but debian. kill off use FIXME
PKG_CHECK_MODULES(LIBSYSFS, libsysfs, [have_libsysfs=yes], [No sysfs pkg-config])
  CFLAGS+=" $LIBSYSFS_CFLAGS"
  LIBS+=" $LIBSYSFS_LIBS"
if test "$have_libsysfs" != "yes" ; then
  AC_CHECK_LIB(sysfs,sysfs_open_device, ([AC_DEFINE([HAVE_SYSFS], [1], [Use sysfs.h from sysfs/]), [have_libsysfs=yes]]),
	     [AC_MSG_ERROR([Cannot find libsysfs.])])
  LIBS+=" -lsysfs"
fi
PKG_CHECK_MODULES([notcurses], [notcurses >= 1.4.3], [have_notcurses=yes])
  CFLAGS+=" $notcurses_CFLAGS"
  LIBS+=" $notcurses_LIBS"
PKG_CHECK_MODULES(LIBZ, zlib, [have_zlib=yes])
	CFLAGS+=" $LIBZ_CFLAGS"
	LIBS+=" $LIBZ_LIBS"
AX_LIB_READLINE
AC_CHECK_LIB([readline], [main],
	      [CONFIGURED_UIS+=" tty"],
	      [AC_MSG_WARN([Cannot find readline headers. Will not build TTY client.])])
AX_WITH_CURSES
AX_WITH_CURSES_PANEL
if test "x$ax_cv_panel" != xyes; then
  PKG_CHECK_MODULES([libpanel], [panel], [have_panel=yes], [have_panel=no])
  if test "$have_panel" = "yes" ; then
    AC_DEFINE([HAVE_PANEL], [1], [Use libpanel from ncursesw])
    CFLAGS+=" $libpanel_CFLAGS"
    LIBS+=" $libpanel_LIBS"
	  CONFIGURED_UIS+=" ncurses"
  else
	  AC_MSG_WARN([Cannot find the Curses Panel library. Will not build the Ncurses client.])
  fi
else
	CONFIGURED_UIS+=" ncurses"
fi
AC_SUBST([CFLAGS])
AC_SUBST([CONFIGURED_UIS])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
