ACLOCAL_AMFLAGS = -I build-aux

AM_CFLAGS = @ELL_CFLAGS@

if SYSTEMD_SERVICE
systemd_unitdir = @SYSTEMD_UNITDIR@
systemd_unit_DATA =

dbus_busdir = @DBUS_BUSDIR@
dist_dbus_bus_DATA =
endif

if DBUS_POLICY
dbus_datadir = @DBUS_DATADIR@/dbus-1/system.d
dist_dbus_data_DATA =
endif

udev_rulesdir = @UDEV_DIR@/rules.d
dist_udev_rules_DATA =

bin_PROGRAMS =
libexec_PROGRAMS =
noinst_PROGRAMS =
check_PROGRAMS =
check_LTLIBRARIES =

EXTRA_DIST = src/edisksd.service.in

libexec_PROGRAMS += src/edisksd

src_edisksd_SOURCES = src/main.c src/edisksd.h \
		      src/block.c src/block.h \
		      src/dbus.c src/dbus.h \
		      src/filesystem.c src/filesystem.h \
		      src/mount.c src/mount.h \
		      src/task.c src/task.h \
		      src/udev.c src/udev.h

src_edisksd_CFLAGS = @LIBMOUNT_CFLAGS@ @PTHREAD_CFLAGS@ @UDEV_CFLAGS@
src_edisksd_LDADD = @ELL_LIBS@ @LIBMOUNT_LIBS@ @PTHREAD_LIBS@ @UDEV_LIBS@
src_edisksd_DEPENDENCIES =

bin_PROGRAMS += client/edisksctl

client_edisksctl_SOURCES = client/main.c client/client.h \
			   client/dbus.c client/dbus.h \
			   client/filesystems.c \
			   client/monitor.c \
			   client/mount.c \
			   client/unmount.c

client_edisksctl_LDADD = @ELL_LIBS@

if SYSTEMD_SERVICE
src_edisksd_DEPENDENCIES += src/edisksd.service

systemd_unit_DATA += src/edisksd.service
dist_dbus_bus_DATA += src/io.github.inmusicdev.edisksd.service
endif

if DBUS_POLICY
dist_dbus_data_DATA += src/io.github.inmusicdev.edisksd.conf
endif

dist_udev_rules_DATA += src/80-edisksd.rules

if HAVE_FUSE3
check_PROGRAMS += tests/fuse-proc
tests_fuse_proc_SOURCES = tests/fuse-proc.c \
			  tests/fuse-mountinfo.c
tests_fuse_proc_CFLAGS = @ELL_CFLAGS@ @FUSE3_CFLAGS@ -DFUSE_USE_VERSION=35
tests_fuse_proc_LDADD = @ELL_LIBS@ @FUSE3_LIBS@
endif

check_LTLIBRARIES += tests/libtestpreload.la
tests_libtestpreload_la_SOURCES = tests/udev-shim.c
tests_libtestpreload_la_LIBADD = @ELL_LIBS@
# Pass a dummy -rpath to persuade libtool to really build a shared library.
tests_libtestpreload_la_LDFLAGS = -rpath /nowhere

SED_PROCESS = $(AM_V_GEN)$(MKDIR_P) $(dir $@) && $(SED) \
	      -e 's,@libexecdir\@,$(libexecdir),g' \
	      < $< > $@

%.service: %.service.in Makefile
	$(SED_PROCESS)

install-exec-hook:
	ln -f $(DESTDIR)$(bindir)/edisksctl$(EXEEXT) \
		$(DESTDIR)$(sbindir)/umount.edisksd$(EXEEXT)
