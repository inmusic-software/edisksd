// SPDX-License-Identifier: LGPL-2.1+
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/mman.h>

#include <ell/ell.h>
#include <libudev.h>

struct udev {
	int refcount;

	struct l_hashmap *devices;
	struct l_queue *monitors;
};

struct udev_list_entry {
	struct udev_list_entry *next;
	char *name;
	char *value;
};

struct udev_device_internal {
	struct udev *udev;

	char *devpath;
	char *subsystem;
	char *devtype;
	char *syspath;
	char *sysname;
	char *sysnum;
	char *devnode;
	dev_t devnum;

	struct udev_list_entry *properties;

	bool visible;
};

struct udev_device {
	struct udev_device_internal *internal;
	const char *action;

	int refcount;
};

struct udev_monitor {
	struct udev *udev;
	struct l_queue *device_queue;
	int event_fd;

	int refcount;
};

struct udev_enumerate {
	struct udev *udev;
	struct udev_list_entry *list;

	int refcount;
};

struct monitor_action_ctx {
	struct udev_device_internal *device;
	const char *action;
};

static struct disabled_mapping {
	void *address;
	size_t length;
	int prot;

	struct disabled_mapping *next;
} *disabled_mappings;

/*
 * Change any executable mappings from libudev.so to PROT_NONE to prevent
 * all access to the "real" udev functions and ensure that if the program
 * under test uses anything we haven't shimmed then an error occurs;
 */
static __attribute__((constructor)) void hide_libudev(void)
{
	size_t line_len = 0;
	char *line = NULL;
	FILE *maps;
	ssize_t n;

	maps = fopen("/proc/self/maps", "r");
	if (!maps)
		abort();

	for (;;) {
		unsigned long long start, end;
		struct disabled_mapping *m;
		char r, w, x, p;

		n = getline(&line, &line_len, maps);
		if (n < 0)
			break;

		if (!strstr(line, "/libudev.so"))
			continue;

		if (sscanf(line, "%llx-%llx %c%c%c%c", &start, &end,
			   &r, &w, &x, &p) != 6)
			continue;

		if (x != 'x')
			continue;

		m = calloc(1, sizeof(*m));
		m->address = (void *) start;
		m->length = end - start;
		m->prot = ((r == 'r') ? PROT_READ : 0) |
			  ((w == 'w') ? PROT_WRITE : 0) |
			  PROT_EXEC;
		m->next = disabled_mappings;
		disabled_mappings = m;

		mprotect(m->address, m->length, PROT_NONE);
	}

	free(line);
	fclose(maps);
}

/*
 * libudev.so has its own destructor functions which the linker will try to
 * run on exit.  For a clean exit, the relevant mappings need to be made
 * executable again.
 */
static __attribute__((destructor)) void show_libudev(void)
{
	while (disabled_mappings) {
		struct disabled_mapping *m;

		m = disabled_mappings;
		disabled_mappings = m->next;

		mprotect(m->address, m->length, m->prot);
		free(m);
	}
}

static struct udev_device_internal *device_from_syspath(struct udev *udev,
							const char *syspath)
{
	return l_hashmap_lookup(udev->devices, syspath);
}

static struct udev_list_entry *device_find_property(struct udev_device_internal *device,
						    const char *name)
{
	struct udev_list_entry *prop;

	for (prop = device->properties; prop; prop = prop->next)
		if (!strcmp(prop->name, name))
			return prop;

	return NULL;
}

static void set_device_property(struct udev_device_internal *device,
				const char *name, const char *value)
{
	struct udev_list_entry *prop;

	prop = device_find_property(device, name);
	if (!prop) {
		prop = l_new(struct udev_list_entry, 1);
		prop->name = l_strdup(name);
		prop->next = device->properties;
		device->properties = prop;
	}

	l_free(prop->value);
	prop->value = l_strdup(value);
}

static void monitor_device_action(void *data, void *user_data)
{
	struct udev_monitor *monitor = data;
	struct monitor_action_ctx *ctx = user_data;
	struct udev_device *device;

	device = l_new(struct udev_device, 1);
	device->internal = ctx->device;
	device->action = ctx->action;
	device->refcount = 1;

	l_queue_push_tail(monitor->device_queue, device);
}

static int set_special_string(struct udev_device_internal *device,
			      const char *value, uintptr_t offset)
{
	char **ptr = (void *) ((char *) &device + offset);

	l_free(*ptr);
	*ptr = l_strdup(value);
	return 0;
}

static int set_devnum(struct udev_device_internal *device,
		      const char *value, uintptr_t offset)
{
	unsigned int major, minor;

	if (sscanf(value, "%u:%u", &major, &minor) != 2) {
		warnx("invalid device number (expect maj:min): %s", value);
		return 1;
	}

	device->devnum = makedev(major, minor);
	set_device_property(device, "MAJOR", l_strdup_printf("%u", major));
	set_device_property(device, "MINOR", l_strdup_printf("%u", minor));

	return 0;
}

#define F_OFFLINE	(1U << 0)
static const struct {
	const char *name;
	int (*apply)(struct udev_device_internal *, const char *, uintptr_t);
	uintptr_t arg;
	unsigned int flags;
} special_properties[] = {
#define SET_STRING(p, n, f)	{ #p, set_special_string, \
				  offsetof(struct udev_device_internal, n), \
				  f, }
	SET_STRING(DEVPATH, devpath, F_OFFLINE),
	SET_STRING(SUBSYSTEM, subsystem, F_OFFLINE),
	SET_STRING(DEVTYPE, devtype, F_OFFLINE),
	SET_STRING(DEVTYPE, devtype, F_OFFLINE),
	SET_STRING(P, syspath, F_OFFLINE),
	SET_STRING(N, sysname, F_OFFLINE),
	SET_STRING(SYSNUM, sysnum, F_OFFLINE),
	SET_STRING(DEVNODE, devnode, F_OFFLINE),
	{ "DEVNUM", set_devnum, 0, F_OFFLINE, },
};

static int apply_properties(struct udev_device_internal *device,
			    char **argv, int argc)
{
	int ret = 0;

	while (argc--) {
		char *name = *argv++;
		char *value = strchr(name, '=');
		size_t i;

		if (!value) {
			warnx("invalid property spec: %s", name);
			ret = 1;
			continue;
		}

		*value++ = '\0';

		for (i = 0; i < L_ARRAY_SIZE(special_properties); i++)
			if (!strcmp(special_properties[i].name, name))
				break;

		if (i < L_ARRAY_SIZE(special_properties)) {
			if (special_properties[i].flags & F_OFFLINE &&
			    device->visible) {
				warnx("property %s cannot be changed on active device",
				      name);
				ret = 1;
				continue;
			}

			if (special_properties[i].apply(device,
							name,
							special_properties[i].arg)) {
				ret = 1;
			}
		} else {
			set_device_property(device, name, value);
		}
	}

	return ret;
}

static int cmd_create(struct udev *udev, char **argv, int argc)
{
	struct udev_device_internal *device;
	const char *syspath;

	if (argc < 2) {
		warnx("create requires a syspath");
		return 64;
	}

	syspath = argv[1];
	device = device_from_syspath(udev, syspath);
	if (device) {
		warnx("device '%s' already exists", syspath);
		return 1;
	}

	device = l_new(struct udev_device_internal, 1);
	device->udev = udev;
	device->syspath = l_strdup(syspath);

	return apply_properties(device, argv + 2, argc - 2);
}

static int cmd_update(struct udev *udev, char **argv, int argc)
{
	struct monitor_action_ctx ctx = { .action = "change", };
	const char *syspath;
	int ret;

	if (argc < 2) {
		warnx("add requires a syspath");
		return 64;
	}

	syspath = argv[1];
	ctx.device = device_from_syspath(udev, syspath);
	if (!ctx.device) {
		warnx("device '%s' does not exist", syspath);
		return 1;
	}

	ret = apply_properties(ctx.device, argv + 2, argc - 2);

	if (ctx.device->visible)
		l_queue_foreach(udev->monitors, monitor_device_action, &ctx);
	return ret;
}

static int cmd_add(struct udev *udev, char **argv, int argc)
{
	struct monitor_action_ctx ctx = { .action = "add", };
	const char *syspath;

	if (argc < 2) {
		warnx("add requires a syspath");
		return 64;
	}

	syspath = argv[1];
	ctx.device = device_from_syspath(udev, syspath);
	if (!ctx.device) {
		warnx("device '%s' does not exist", syspath);
		return 1;
	}

	if (ctx.device->visible) {
		warnx("device '%s' is already added", syspath);
		return 0;
	}

	// FIXME: check all required properties exist

	ctx.device->visible = true;
	l_queue_foreach(udev->monitors, monitor_device_action, &ctx);
	return 0;
}

static int cmd_remove(struct udev *udev, char **argv, int argc)
{
	struct monitor_action_ctx ctx = { .action = "remove", };
	const char *syspath;

	if (argc < 2) {
		warnx("add requires a syspath");
		return 64;
	}

	syspath = argv[1];
	ctx.device = device_from_syspath(udev, syspath);
	if (!ctx.device) {
		warnx("device '%s' does not exist", syspath);
		return 1;
	}

	if (!ctx.device->visible) {
		warnx("device '%s' is already removed", syspath);
		return 0;
	}

	ctx.device->visible = false;
	l_queue_foreach(udev->monitors, monitor_device_action, &ctx);
	return 0;
}

static const struct {
	const char *name;
	int (*fn)(struct udev *udev, char **argv, int argc);
} commands[] = {
	{ "create", cmd_create, },
	{ "update", cmd_update, },
	{ "add", cmd_add, },
	{ "remove", cmd_remove, },
};

static int run_udev_command(struct udev *udev, char **argv, int argc)
{
	size_t i;
	int ret;

	if (argc < 1)
		return 1;

	for (i = 0; i < L_ARRAY_SIZE(commands); i++)
		if (!strcmp(argv[0], commands[i].name))
			break;

	if (i == L_ARRAY_SIZE(commands)) {
		warnx("unknown command: %s", argv[0]);
		return 127;
	}

	ret = commands[i].fn(udev, argv, argc);
	if (ret)
		warnx("command failed");

	return ret;
}

static void run_udev_commands(struct udev *udev, FILE *f)
{
	size_t line_len = 0;
	char *line = NULL;
	ssize_t n;

	for (;;) {
		char **argv;
		int argc;

		n = getline(&line, &line_len, f);
		if (n <= 0)
			break;

		while (line[--n] == '\n')
			line[n] = '\0';

		argv = l_parse_args(line, &argc);
		if (!argv) {
			warnx("bad command: %s", line);
			continue;
		}

		run_udev_command(udev, argv, argc);
	}

	free(line);
}

struct udev *udev_new(void)
{
	struct udev *udev;
	char *init_script;

	udev = calloc(1, sizeof(*udev));
	if (!udev)
		return NULL;

	udev->refcount = 1;
	udev->devices = l_hashmap_string_new();
	udev->monitors = l_queue_new();

	init_script = getenv("LIBUDEV_SHIM_INIT_SCRIPT");
	if (init_script) {
		FILE *f = fopen(init_script, "r");

		if (!f)
			warn("failed to open %s", init_script);

		run_udev_commands(udev, f);
		fclose(f);
	}

	/*
	 * ASSUME: we are preloaded in a process with an ELL main loop and
	 * l_main_init() has been called before udev_new().
	 */
	// FIXME: set up UNIX command socket

	return udev;
}

struct udev *udev_unref(struct udev *udev)
{
	if (--udev->refcount > 0)
		return NULL;

	free(udev);

	return NULL;
}

static void free_udev_list(struct udev_list_entry *entry)
{
	struct udev_list_entry *next;

	for (;;) {
		if (!entry)
			break;

		next = entry->next;

		l_free(entry->name);
		l_free(entry->value);
		l_free(entry);

		entry = next;
	}
}

struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *entry)
{
	return entry->next;
}

const char *udev_list_entry_get_name(struct udev_list_entry *entry)
{
	return entry->name;
}

struct udev_device *udev_device_ref(struct udev_device *device)
{
	device->refcount++;
	return device;
}

struct udev_device *udev_device_unref(struct udev_device *device)
{
	if (--device->refcount > 0)
		return NULL;

	l_free(device);
	return NULL;
}

static void udev_device_queue_unref(void *data)
{
	udev_device_unref(data);
}

struct udev *udev_device_get_udev(struct udev_device *device)
{
	return device->internal->udev;
}

struct udev_device *udev_device_new_from_syspath(struct udev *udev,
						 const char *syspath)
{
	struct udev_device_internal *device_internal;
	struct udev_device *device;

	device_internal = device_from_syspath(udev, syspath);
	if (!device_internal || !device_internal->visible)
		return NULL;

	device = l_new(struct udev_device, 1);
	device->internal = device_internal;

	return udev_device_ref(device);
}

struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev,
						   const char *name)
{
	struct udev_monitor *monitor;

	if (!udev)
		return NULL;

	if (!strcmp(name, "udev"))
		; /* okay */
	else
		return NULL;

	monitor = calloc(1, sizeof(*monitor));
	if (!monitor)
		return NULL;

	monitor->udev = udev;
	monitor->refcount = 1;

	monitor->device_queue = l_queue_new();

	monitor->event_fd = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK);
	if (monitor->event_fd < 0)
		goto error_eventfd;

	l_queue_push_tail(udev->monitors, monitor);

	return monitor;

error_eventfd:
	free(monitor);
	return NULL;
}

struct udev_monitor *udev_monitor_unref(struct udev_monitor *monitor)
{
	if (--monitor->refcount > 0)
		return NULL;

	l_queue_remove(monitor->udev->monitors, monitor);
	l_queue_destroy(monitor->device_queue, udev_device_queue_unref);
	close(monitor->event_fd);
	free(monitor);

	return NULL;
}

int udev_monitor_get_fd(struct udev_monitor *monitor)
{
	return monitor->event_fd;
}

int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *monitor,
						    const char *subsystem,
						    const char *devtype)
{
	/* Only one type of "filter" is supported right now. */
	if (!monitor)
		return -1;
	if (strcmp(subsystem, "block"))
		return -1;
	if (devtype)
		return -1;

	return 0;
}

int udev_monitor_enable_receiving(struct udev_monitor *monitor)
{
	return 0;
}

struct udev_device *udev_monitor_receive_device(struct udev_monitor *monitor)
{
	uint64_t count;

	if (read(monitor->event_fd, &count, sizeof(count)) != sizeof(count))
		return NULL;

	assert(count == 1);
	return l_queue_pop_head(monitor->device_queue);
}

struct udev_enumerate *udev_enumerate_new(struct udev *udev)
{
	struct udev_enumerate *enumerate;

	if (!udev)
		return NULL;

	enumerate = calloc(1, sizeof(*enumerate));
	if (!enumerate)
		return NULL;

	enumerate->udev = udev;
	enumerate->refcount = 1;

	return enumerate;
}

struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *enumerate)
{
	if (--enumerate->refcount > 0)
		return NULL;

	free_udev_list(enumerate->list);
	free(enumerate);

	return NULL;
}

int udev_enumerate_add_match_subsystem(struct udev_enumerate *enumerate,
				       const char *subsystem)
{
	/* Only one type of "filter" is supported right now. */
	if (!enumerate)
		return -1;
	if (strcmp(subsystem, "block"))
		return -1;

	return 0;
}

static void append_device_to_list(const void *key, void *value,
				  void *user_data)
{
	struct udev_device_internal *device = value;
	struct udev_list_entry ***prev = user_data;
	struct udev_list_entry *e;

	e = l_new(struct udev_list_entry, 1);
	e->name = l_strdup(device->syspath);

	(**prev) = e;
	*prev = &e->next;
}

int udev_enumerate_scan_devices(struct udev_enumerate *enumerate)
{
	struct udev_list_entry **prev = &enumerate->list;

	free_udev_list(enumerate->list);

	l_hashmap_foreach(enumerate->udev->devices, append_device_to_list,
			  &prev);

	return 0;
}

struct udev_list_entry *
udev_enumerate_get_list_entry(struct udev_enumerate *enumerate)
{
	return enumerate->list;
}
