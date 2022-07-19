AC_PREREQ(2.60)
AC_INIT(edisksd, 0.7)

AC_CONFIG_HEADERS(config.h)
AC_CONFIG_AUX_DIR(build-aux)
AC_CONFIG_MACRO_DIR(build-aux)

AM_INIT_AUTOMAKE([foreign subdir-objects color-tests silent-rules
		  tar-pax no-dist-gzip dist-xz])

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AM_MAINTAINER_MODE

AC_PREFIX_DEFAULT(/usr/local)

PKG_PROG_PKG_CONFIG

if (test "${CFLAGS}" = ""); then
	CFLAGS="-Wall -O2"
fi
if (test "$USE_MAINTAINER_MODE" = "yes"); then
	CFLAGS+=" -Werror -Wextra"
	CFLAGS+=" -Wno-unused-parameter"
	CFLAGS+=" -Wdeclaration-after-statement"
	CFLAGS+=" -Wmissing-declarations"
	CFLAGS+=" -Wredundant-decls"
	CFLAGS+=" -Wvariadic-macros"
	CFLAGS+=" -Wformat -Wformat-security"
fi

AC_LANG([C])

AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MKDIR_P
AC_PROG_LN_S

AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

LT_PREREQ(2.2)
LT_INIT([disable-static])

AC_MSG_CHECKING([for pthreads support])
pthread_save_CFLAGS="$CFLAGS"
CFLAGS="$CFLAGS -pthread"
AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <pthread.h>
				 static void *start_routine(void *a) { return a; }],
				[pthread_t t;
				 pthread_create(&t, NULL, start_routine, NULL);
				 pthread_join(t, NULL);])],
	       [pthread_found=yes],
	       [AC_MSG_ERROR([pthreads not found])])
AC_MSG_RESULT([yes])
CFLAGS="$pthread_save_CFLAGS"

AC_SUBST(PTHREAD_CFLAGS, [-pthread])
AC_SUBST(PTHREAD_LIBS, [])

PKG_CHECK_MODULES(ELL, ell >= 0.5, dummy=yes,
		  AC_MSG_ERROR([Embedded Linux Library >= 0.5 is required]))
AC_SUBST(ELL_CFLAGS)
AC_SUBST(ELL_LIBS)

PKG_CHECK_MODULES(UDEV, libudev >= 220, dummy=yes,
		  AC_MSG_ERROR([libudev >= 220 is required]))
AC_SUBST(UDEV_CFLAGS)
AC_SUBST(UDEV_LIBS)

PKG_CHECK_MODULES(LIBMOUNT, mount >= 2.30, dummy=yes,
		  AC_MSG_ERROR([libmount >= 2.30 is required]))
AC_SUBST(LIBMOUNT_CFLAGS)
AC_SUBST(LIBMOUNT_LIBS)

PKG_CHECK_MODULES(FUSE3, fuse3 >= 3.10, have_fuse3=yes, have_fuse3=no)
AC_SUBST(FUSE3_CFLAGS)
AC_SUBST(FUSE3_LIBS)
AM_CONDITIONAL([HAVE_FUSE3], test "${have_fuse3}" = "yes")

AC_DEFINE_UNQUOTED(DAEMON_CONFIGDIR, "/etc/edisksd",
		   [Directory for disks daemon configuration])

AC_ARG_ENABLE([dbus-policy], AS_HELP_STRING([--disable-dbus-policy],
	      [do not install D-Bus system policy files]),
	      [enable_dbus_policy=${enableval}])
AM_CONDITIONAL(DBUS_POLICY, test "${enable_dbus_policy}" != "no")

AC_ARG_WITH([dbus-datadir], AS_HELP_STRING([--with-dbus-datadir=DIR],
	    [path to D-Bus data directory]),
	    [path_dbus_datadir=${withval}])
if (test "${enable_dbus_policy}" != "no" && test -z "${path_dbus_datadir}"); then
	AC_MSG_CHECKING([D-Bus data directory])
	path_dbus_datadir="`$PKG_CONFIG --variable=datadir dbus-1`"
	if (test -z "${path_dbus_datadir}"); then
		path_dbus_datadir="`$PKG_CONFIG --variable=sysconfdir dbus-1`"
		if (test -z "${path_dbus_datadir}"); then
			AC_MSG_ERROR([D-Bus data directory is required])
		fi
	fi
	AC_MSG_RESULT([${path_dbus_datadir}])
fi
AC_SUBST(DBUS_DATADIR, [${path_dbus_datadir}])

AC_ARG_ENABLE([systemd-service], AS_HELP_STRING([--disable-systemd-service],
	      [do not install systemd service files]),
	      [enable_systemd_service=${enableval}])
AM_CONDITIONAL(SYSTEMD_SERVICE, test "${enable_systemd_service}" != "no")

AC_ARG_WITH([dbus-busdir], AS_HELP_STRING([--with-dbus-busdir=DIR],
	    [path to D-Bus bus services directory]),
	    [path_dbus_busdir=${withval}])
if (test "${enable_systemd_service}" != "no" && test -z "${path_dbus_busdir}"); then
	AC_MSG_CHECKING([D-Bus bus services directory])
	path_dbus_busdir="`$PKG_CONFIG --variable=system_bus_services_dir dbus-1`"
	if (test -z "${path_dbus_busdir}"); then
		AC_MSG_ERROR([D-Bus bus services directory is required])
	fi
	AC_MSG_RESULT([${path_dbus_busdir}])
fi
AC_SUBST(DBUS_BUSDIR, [${path_dbus_busdir}])

AC_ARG_WITH([systemd-unitdir], AS_HELP_STRING([--with-systemd-unitdir=DIR],
	    [path to systemd unit directory]),
	    [path_systemd_unitdir=${withval}])
if (test "${enable_systemd_service}" != "no" && test -z "${path_systemd_unitdir}"); then
	AC_MSG_CHECKING([systemd unit directory])
	path_systemd_unitdir="`$PKG_CONFIG --variable=systemdsystemunitdir systemd`"
	if (test -z "${path_systemd_unitdir}"); then
		AC_MSG_ERROR([systemd unit directory is required])
	fi
	AC_MSG_RESULT([${path_systemd_unitdir}])
fi
AC_SUBST(SYSTEMD_UNITDIR, [${path_systemd_unitdir}])

AC_ARG_WITH([udevdir], AS_HELP_STRING([--with-udevdir=DIR],
	    [path to udev directory]),
	    [path_udevdir=${withval}])
if (test -z "${path_udevdir}"); then
	AC_MSG_CHECKING([udev directory])
	path_udevdir="`$PKG_CONFIG --variable=udevdir udev`"
	if (test -z "${path_udevdir}"); then
		AC_MSG_ERROR([udev directory is required])
	fi
	AC_MSG_RESULT([${path_udevdir}])
fi
AC_SUBST(UDEV_DIR, [${path_udevdir}])

AC_CONFIG_FILES(Makefile)

AC_OUTPUT

AC_MSG_RESULT([
	$PACKAGE_NAME $VERSION
])
