/**
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2020 Helbling Technik GmbH
 *
 * @file udev.c
 * @author Johannes Fischer <johannes.fischer@helbling.de>
 * @date 2020-04-04
 * @brief UdevMonitor class for mounting and umounting devices
 *
 * Usage:
 * ------
 * 
 * > UdevMonitor *monitor = udev_monitor_new (void);
 * > g_signal_connect (monitor, "attach", (GCallback)on_attach, data);
 * > ...
 * > g_signal_handlers_disconnect_by_data(monitor, data);
 * > udev_monitor_quit(monitor);
 * > g_object_unref(monitor);
 *
 * Signals
 * -------
 *
 * attach   UdevMonitor *monitor
 *          GUdevDevice *device
 *          GSList of gchar *mount_points
 *          GCancellable *cancellable
 *
 * detach   UdevMonitor *monitor
 *          GSList of gchar *mount_points
 *          GUdevDevice *device
 */

#include <sys/mount.h>
#include <errno.h>
#include "udev.h"
#include <gio/gio.h>

#define DISK_ID(d) g_udev_device_get_property(device, "ID_PART_TABLE_UUID")
#define NEW_DISK_ID(d) g_strdup(DISK_ID(d))

#define UDEV_TIMEOUT 1.0f
typedef struct
{
	gboolean attached;
	GUdevDevice *gudev_device;
	GCancellable *cancellable;
	GSList *partitions; /* GUdevDevice */
	GSList *mount_points; /*gchar */
	GTimer *initialized;
} Disk;


enum
{
  ATTACH,
  DETACH,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct _UdevMonitor
{
	GObject parent_object;
	GUdevClient *gudev_client;
	GAsyncQueue *process_device_queue;
	GThread *process_device_thread;
	GHashTable *disks;
};
G_DEFINE_TYPE(UdevMonitor, udev_monitor, G_TYPE_OBJECT);


/**
 * @brief Checks, if a specific fstype is supported by the OS
 *
 * The `fstype` is compared with entries in a file (normally /proc/filesystems)
 *
 * @param[in] filesystems file
 * @param[in] filesystem type
 * @return TRUE if the fstype is supported, otherwise FALSE
 */
static gboolean
is_in_filesystem_file (const gchar *filesystems_file,
                       const gchar *fstype)
{
	gchar *filesystems = NULL;
	GError *error = NULL;
	gboolean ret = FALSE;
	gchar **lines = NULL;
	guint n;

	if (!g_file_get_contents (filesystems_file,
	                          &filesystems,
	                          NULL, /* gsize *out_length */
	                          &error)) {
		g_warning ("Error reading %s: %s (%s %d)",
		           filesystems_file,
		           error->message,
		           g_quark_to_string (error->domain),
		           error->code);
		g_clear_error (&error);
		goto out;
	}

	lines = g_strsplit (filesystems, "\n", -1);
	for (n = 0; lines != NULL && lines[n] != NULL && !ret; n++) {
		gchar **tokens;
		gint num_tokens;
		g_strdelimit (lines[n], " \t", ' ');
		g_strstrip (lines[n]);
		tokens = g_strsplit (lines[n], " ", -1);
		num_tokens = g_strv_length (tokens);
		if (num_tokens == 1 && g_strcmp0 (tokens[0], fstype) == 0) {
			ret = TRUE;
		}
		g_strfreev (tokens);
	}

 out:
	g_strfreev (lines);
	g_free (filesystems);
	return ret;
}

/**
 * @brief Mounts a partition of a disk
 *
 * @param[in] GUdevDevice instance
 * @param[in] Disk struct
 *
 * On success, the mountpoint will be added to the list `mount_points` of the
 * disk.
 */
static void
mount_partition(gpointer data, gpointer user_data)
{
	GUdevDevice *gudev_device = G_UDEV_DEVICE(data);
	Disk *disk = (Disk *)user_data;
	const gchar* path;
	const gchar* name;
	const gchar* type;
	gchar* mount_dir;

	path = g_udev_device_get_device_file(gudev_device);
	name = g_udev_device_get_name (gudev_device);
	type = g_udev_device_get_property (gudev_device, "ID_FS_TYPE");

	if(!is_in_filesystem_file ("/proc/filesystems", type)) {
		return; /* type not supported by OS */
	}
	
	mount_dir = g_strdup_printf("/run/media/disk-updater/%s", name);
	
	if(g_mkdir_with_parents (mount_dir, 0755) != 0 && errno != EEXIST) {
		g_warning("Could not create directory %s", mount_dir);
		g_free(mount_dir);
		return;
	}
		
	if(0 != mount(path, mount_dir, type, 0 , "")) {
		g_warning("Could not mount %s", path);
		g_free(mount_dir);
		return;
	}

	disk->mount_points = g_slist_prepend(disk->mount_points, mount_dir);
}

/**
 * @brief umount a partition
 *
 * Devices with unset `mount_dir` are ignored.
 *
 * @param[in] gchar-string to the mountpoint
 * @param[in] NULL
 */
static void
umount_partition(gpointer data, gpointer user_data)
{

	gchar *mount_dir = (gchar *)data;
	if(mount_dir != NULL && umount2(mount_dir, MNT_DETACH) != 0 && errno != EINVAL ) {
		g_warning("Could not unmount %s", mount_dir);
	}
}

/**
 * @brief free the disk struct
 *
 * Do not use the disk after calling this function.
 *
 * @param[in] disk struct
 */
static void
free_disk(Disk *disk)
{
	g_slist_foreach(disk->mount_points, umount_partition, NULL);
	g_slist_free_full(disk->mount_points, g_free);
	g_slist_free_full(disk->partitions, g_object_unref);
	g_object_unref(disk->gudev_device);
	g_object_unref(disk->cancellable);
	g_timer_destroy(disk->initialized);
	g_slice_free(Disk, disk);
}

/**
 * @brief Loop for mounting, umounting and emiting signals
 *
 * @param[in] UdevMonitor struct
 * @return NULL on exit
 */
static gpointer
process_disk_thread_func (gpointer user_data)
{
	UdevMonitor *self = UDEV_MONITOR(user_data);
	Disk *disk;
	
	do {
		disk = g_async_queue_pop (self->process_device_queue);
		
		/* used by _finalize() to stop this thread - if received, we can no
		 * longer use @monitor
		 */
		if (disk == (gpointer) 0xdeadbeef)
			goto out;

		if(disk->attached) {
			g_slist_foreach(disk->partitions, mount_partition, disk);

			g_signal_emit (self, signals[ATTACH], 0,
			               disk->gudev_device,
                           disk->mount_points,
			               disk->cancellable);
		} else {
			g_signal_emit (self, signals[DETACH], 0,
			               disk->gudev_device);
			free_disk(disk);
		}
	} while (TRUE);

 out:
	return NULL;
}

/**
 * @brief Delayed function for initializing a disk
 *
 * Not every udev/kernel binds the block device after adding all partition
 * devices. It can not be said, whether all partitions are already added by udev
 * or futher ones will be added. Therefore, this function is called delayed and
 * checks, whether no further partitions were added since one second
 * (UDEV_TIMEOUT). If this is the case, the disk are handed over to the thread,
 * which mounts the partitions.
 *
 * @param[in] UdevMonitor struct
 * @return TRUE, if the function has to be called again, otherwise FALSE.
 */
gboolean
on_disk_initialized(gpointer user_data)
{
	UdevMonitor *self = UDEV_MONITOR(user_data);
	gpointer key, value;
	GHashTableIter iter;
	Disk *disk;
	g_hash_table_iter_init (&iter, self->disks);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		disk = (Disk *)value;
		if(!disk->attached && g_timer_elapsed(disk->initialized, NULL) > UDEV_TIMEOUT) {
			disk->attached = TRUE;
			g_async_queue_push (self->process_device_queue, disk);
			return FALSE;
		}
	}
		return TRUE; /* no disk found..wait another second */
}


/**
 * @brief Callback for udev events
 *
 * @param[in] GUdevClient instance
 * @param[in] action
 * @param[in] device
 * @param[in] UdevMonitor struct
 */
static void
on_uevent (GUdevClient *client,
           const gchar *action,
           GUdevDevice *device,
           gpointer user_data)
{
	UdevMonitor *self = UDEV_MONITOR(user_data);
	Disk *disk = NULL;
	gchar *key = NULL;

	const gchar *subsystem = g_udev_device_get_subsystem(device);
	const gchar *devtype = g_udev_device_get_property(device, "DEVTYPE");
	
	
	if(g_strcmp0 (subsystem, "block"))
		return;

	if(!g_strcmp0 (action, "add")) {	
		if(!g_strcmp0 (devtype, "disk")) {
			/* new disk */
			disk = g_slice_new0 (Disk);
			disk->gudev_device = g_object_ref (device);
			disk->cancellable = g_cancellable_new ();
			disk->attached = FALSE;
			disk->initialized = g_timer_new();
			g_hash_table_insert(self->disks, NEW_DISK_ID(device), disk);
			g_timeout_add_seconds(UDEV_TIMEOUT, on_disk_initialized, self);
		}
		else if(!g_strcmp0 (devtype, "partition")) {			
			/* new partition */
			disk = g_hash_table_lookup(self->disks, DISK_ID(device));			
			if(disk && !disk->attached) {
				g_timer_start(disk->initialized);
				disk->partitions = g_slist_prepend(disk->partitions,
				                                   g_object_ref(device));
			} else {
				g_warning("Ignore partition due to udev timeout");
			}
		}
	}
	else if(!g_strcmp0 (action, "remove")) {
		/* remove disk */
		if(g_hash_table_steal_extended(self->disks,
		                               DISK_ID(device),
		                               (gpointer *) &key,
		                               (gpointer *) &disk)) {
			g_free(key);
			disk->attached = FALSE;
			g_cancellable_cancel(disk->cancellable);
			g_async_queue_push (self->process_device_queue, disk);
		}
	}
}

/**
 * @brief foreach-callback for cancelling the operation of a disk
 *
 * Before an disk is detached, pending disk operations are
 * cancelled. 
 *
 * @param[in] DISK_ID
 * @param[in] Disk struct
 * @param[in] NULL
 */
static void
cancel_disk(gpointer key, gpointer value, gpointer user_data)
{
	g_cancellable_cancel(((Disk *)value)->cancellable);
}

/**
 * @brief Stop thread execution
 *
 * Call this function before freeing an UdevMonitor instance.

 * @param[in] UdevMonitor instance
 */
void
udev_monitor_quit(UdevMonitor *self)
{
	/* stop receiving udev singals */
	g_signal_handlers_disconnect_by_data(self->gudev_client, self);
	/* stop thread operations and umount devices */
	g_async_queue_push_front(self->process_device_queue, (gpointer)0xdeadbeef);
	g_hash_table_foreach (self->disks, cancel_disk, NULL);
	g_thread_join(self->process_device_thread);
}


/**
 * @brief Destructor of an UdevMonitor instance
 *
 * This also umount all mounted partitions.

 * @param[in] UdevMonitor instance
 */
static void
udev_monitor_finalize (GObject *gobject)
{
	UdevMonitor *self = UDEV_MONITOR(gobject);
	g_async_queue_unref(self->process_device_queue);
	g_object_unref(self->gudev_client);
	g_hash_table_destroy(self->disks); /* also umount */
	G_OBJECT_CLASS (udev_monitor_parent_class)->finalize (gobject);
}


/**
 * @brief Constructor of the UdevMonitor class
 *
 * @param[in] UdevMonitorClass instance
 */
static void
udev_monitor_class_init (UdevMonitorClass *klass) 
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);	
	object_class->finalize = udev_monitor_finalize;
	
	signals[ATTACH] = g_signal_new ("attach",
	                                G_TYPE_FROM_CLASS (klass),
	                                G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE |
	                                G_SIGNAL_NO_HOOKS,
	                                0 /* class offset */,
	                                NULL /* accumulator */,
	                                NULL /* accumulator data */,
	                                NULL /* C marshaller */,
	                                G_TYPE_NONE /* return_type */,
	                                3     /* n_params */,
	                                G_UDEV_TYPE_DEVICE,
	                                G_TYPE_POINTER,
	                                G_TYPE_CANCELLABLE/* param_types */);

	signals[DETACH] = g_signal_new ("detach",
	                                G_TYPE_FROM_CLASS (klass),
	                                G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE |
	                                G_SIGNAL_NO_HOOKS,
	                                0 /* class offset */,
	                                NULL /* accumulator */,
	                                NULL /* accumulator data */,
	                                NULL /* C marshaller */,
	                                G_TYPE_NONE /* return_type */,
	                                2     /* n_params */,
	                                G_UDEV_TYPE_DEVICE,
	                                G_TYPE_POINTER /* param_types */);

}

/**
 * @brief Constructor of the UdevMonitor
 *
 * @param[in] UdevMonitor instance
 */
static void
udev_monitor_init (UdevMonitor *self)
{
	/* Check the number of items in GUdevClient */
	const gchar *subsystems[] = {"block", NULL};

	self->disks = g_hash_table_new_full(g_str_hash,
	                                    g_str_equal,
	                                    (GDestroyNotify)g_free,
	                                    (GDestroyNotify)free_disk);

	/* get ourselves an udev client */
	self->gudev_client = g_udev_client_new (subsystems);

	g_signal_connect(self->gudev_client,
	                 "uevent",
	                 G_CALLBACK(on_uevent),
	                 self);

	self->process_device_queue = g_async_queue_new ();
	self->process_device_thread = g_thread_new ("process-device",
	                                            process_disk_thread_func,
	                                            self);
}

/**
 * @brief Helper function for constructing an UdevMonitor instance
 *
 * @return UdevMonitor instance
 */
UdevMonitor *
udev_monitor_new (void)
{
	return g_object_new (udev_monitor_get_type(), 0);
}
