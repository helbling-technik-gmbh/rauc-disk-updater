/**
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2020 Helbling Technik GmbH
 *
 * @file rauc-disk-updater.c
 * @author Johannes Fischer <johannes.fischer@helbling.de>
 * @date 2020-04-04
 * @brief Rauc Disk Updater
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>

#include "udev.h"
#include "de-helbling-disk-updater-gen.h"
#include "de-pengutronix-rauc-gen.h"

#define VERSION 1.0

static gboolean opt_version = FALSE;
static gchar *script_file = NULL;

#define DISK_ID(d) g_udev_device_get_property(device, "ID_PART_TABLE_UUID")
#define NEW_DISK_ID(d) g_strdup(DISK_ID(d))

typedef DiskUpdaterBundle Bundle;

typedef struct
{
	GMainLoop *loop;
	gint exit_code;
	GDBusConnection *dbus_connection;
	DiskUpdater *disk_updater;
	UdevMonitor *monitor;
	RaucInstaller *installer;
	gchar *compatible; /* system compatible */

	guint bundle_dbus_count;
	guint device_count;
	
	GHashTable *bundles_by_disk;
} MainContext;


/* Commandline options */
static GOptionEntry entries[] =
	{
	 { "script", 's', G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &script_file,
	   "Script file", NULL },
	 { "version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_version,
	   "Version information", NULL },
	 { NULL }
	};

/**
 * @brief Callback of dbus interface for installing a bundle 
 *
 * @param[in] bundle 
 * @param[in] dbus method invocation
 * @param[in] MainContext struct
 */
static void
on_dbus_install (Bundle *interface,
                 GDBusMethodInvocation *invocation,
                 gpointer user_data)
{
	MainContext *context = (MainContext*) user_data;
	const gchar *path = disk_updater_bundle_get_path(interface);
	GError *error = NULL;
	
	g_message("Install bundle %s", path);
	if (! rauc_installer_call_install_sync(context->installer, path, NULL, &error)) {
		g_warning("Failed %s\n", error->message);
		g_dbus_method_invocation_take_error(invocation, error);
	} else {
		g_dbus_method_invocation_return_value(invocation, NULL);
	}
}

/**
 * @brief Validates if a file is a rauc bundle
 *
 * If the file is a bundle, a dbus interface for this bundle is published.
 *
 * @param[in] MainContext struct
 * @param[in] cancellable for stopping the validation
 * @param[in] Path to the file
 * @return NULL or bundle dbus interface
 */
static Bundle *
check_rauc_bundle(MainContext *context,
                  GCancellable *cancellable,
                  const gchar *path)
{
	GError *error = NULL;
	gchar *compatible = NULL;
	gchar *version = NULL;
	gchar *interface_path;
	Bundle *bundle = NULL;
		
	/* filter for suffix .raucb */
	if(! g_str_has_suffix(path, ".raucb"))
		goto out;
	
	/* query version and compatible string from bundle */
	if (!rauc_installer_call_info_sync(context->installer,
	                                   path,
	                                   &compatible,
	                                   &version,
	                                   cancellable,
	                                   &error)) {
		g_warning("Failed to verify %s", path);
		g_clear_error(&error);
		goto out;
	}
	
	/* filter bundles with matching compatible string */
	if (g_strcmp0(context->compatible, compatible)) {
		g_message("Ignore %s with unknown compatible %s",
		          path, compatible);
		goto out;
	}

	g_message("%10s %s (%s)", "found", path, version);

	/* set up new dbus interface for bundle */
	bundle = disk_updater_bundle_skeleton_new();
	disk_updater_bundle_set_version(bundle, version);
	disk_updater_bundle_set_path(bundle, path);
	g_signal_connect (bundle,
	                  "handle-install",
	                  G_CALLBACK (on_dbus_install),
	                  context);
	
	/* publish dbus interface for found bundle */	
	interface_path = g_strdup_printf("/de/helbling/DiskUpdater/bundles/%d",
	                                 ++(context->bundle_dbus_count));
	g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(bundle),
	                                 context->dbus_connection,
	                                 interface_path,
	                                 NULL);
 out:	
	g_free(compatible);
	g_free(version);
	g_free(interface_path);
	return bundle;
}

/**
 * @brief Search for rauc bundles at a path
 *
 * @param[in] MainContext struct
 * @param[in] cancellable for stopping the search
 * @param[in] path to the search path
 */
static GSList *
find_rauc_bundles(MainContext *context,
                  GCancellable *cancellable,
                  const gchar *path)
{
	GDir *dir;
	GError *error = NULL;
	gchar *file;
	const gchar *name;
	GSList *bundles = NULL;
	Bundle *bundle;

	dir = g_dir_open(path, 0, &error);
	while (!g_cancellable_is_cancelled(cancellable) &&
	       dir != NULL && (name = g_dir_read_name(dir))) {
		
		file = g_strdup_printf("%s/%s",path, name);
		/* do not follow symlinks */
		if (g_file_test (file, G_FILE_TEST_IS_SYMLINK)) {
			continue;
		} else if (g_file_test (file, G_FILE_TEST_IS_DIR)) {
			/* recursive call */
			bundles = g_slist_concat(bundles,find_rauc_bundles(context,
			                                                   cancellable,
			                                                   file));
		} else if (g_file_test (file, G_FILE_TEST_IS_REGULAR)) {
			bundle = check_rauc_bundle(context, cancellable, file);
			if(bundle) {
				bundles = g_slist_prepend(bundles, bundle);
			}
		}
		g_free(file);
	}
	g_dir_close(dir);
	return bundles;
}

/**
 * @brief Execute the script hook and installs a bundle
 *
 * @param[in] MainContext struct
 * @param[in] cancellable for stopping the search
 * @param[in] List of bundles for the installation
 */
static void
run_hook_install(MainContext *context,
                 GCancellable *cancellable,
                 GSList *bundles_head)
{
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) subprocess = NULL;
	GError *error = NULL;
	gboolean res = FALSE;
	gchar *str;
	guint bundle_ctr = 0;
	Bundle *bundle;
	gint index;
	GSList *bundles = bundles_head;
	
	if (script_file == NULL)
		goto out;

	if(bundles == NULL)
		goto out;

	g_debug("Start hook script %s", script_file);
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
	
	while(bundles) {
		bundle = DISK_UPDATER_BUNDLE(bundles->data);
		bundle_ctr += 1;

		str = g_strdup_printf("BUNDLE_PATH_%d",bundle_ctr);

		g_subprocess_launcher_setenv(launcher,
		                             str,
		                             disk_updater_bundle_get_path(bundle),
		                             TRUE);
		g_clear_pointer(&str, g_free);

		str = g_strdup_printf("BUNDLE_VERSION_%d",bundle_ctr);
		g_subprocess_launcher_setenv(launcher,
		                             str,
		                             disk_updater_bundle_get_version(bundle),
		                             TRUE);
		g_clear_pointer(&str, g_free);

		bundles = g_slist_next(bundles);
	}
	
	str = g_strdup_printf("%d",bundle_ctr);
	g_subprocess_launcher_setenv(launcher, "BUNDLES", str, TRUE);
	g_clear_pointer(&str, g_free);
	
	subprocess = g_subprocess_launcher_spawn(launcher, &error, script_file,
	                                         "install", NULL);
	
	if (subprocess == NULL) {
		g_warning("Failed to run script %s", script_file);
		g_clear_error(&error);
		goto out;
	}

	if(! g_subprocess_wait(subprocess, cancellable, &error)) {
		g_subprocess_force_exit(subprocess);
		g_clear_error(&error);
		goto out;
	}
	
	index = g_subprocess_get_exit_status (subprocess);
	if(index == 0) {
		g_warning("Script denied installation");
		goto out;
	}

	bundle = DISK_UPDATER_BUNDLE(g_slist_nth_data(bundles_head, index-1));
	if(!bundle) {
		g_warning("Bundle index out of bounds");
		goto out;
	}

	g_message("Install bundle %s", disk_updater_bundle_get_path(bundle));
	if (! rauc_installer_call_install_sync(context->installer,
	                                       disk_updater_bundle_get_path(bundle),
	                                       cancellable,
	                                       &error)) {
		g_warning("Failed %s\n", error->message);
		g_clear_error(&error);
	}

 out:
	return;
};

/**
 * @brief Free a bundle interface
 *
 * @param[in] bundle dbus interface
 */
static void
free_bundle(gpointer data)
{
	g_dbus_interface_skeleton_unexport((GDBusInterfaceSkeleton *)data);
	g_object_unref(data);
}

/**
 * @brief foreach-callback for freeing a list of bundles
 *
 * @param[in] List of bundles
 */
static void
bundles_destroyed(gpointer data)
{
	g_slist_free_full((GSList *)data, free_bundle);
}

/**
 * @brief Signal callback for an plugged in device
 *
 * This function is executed in a separate thread of the UdevMonitor. If the
 * device is removed again, the cancellable is set.

 * @param[in] UdevMonitor instance
 * @param[in] GUDevDevice struct of the block device
 * @param[in] Mountpoints of the partitions
 * @param[in] cancellable for stopping the operation
 * @param[in] MainContext struct
 */
static void
on_attach(UdevMonitor *monitor,
         GUdevDevice *device,
         gpointer *mount_points,
         GCancellable *cancellable,
         gpointer user_data)
{
	GSList *mount_point = (GSList *)mount_points;
	MainContext *context = (MainContext*) user_data;
	GSList *bundles = NULL;
	
	disk_updater_set_device_count(context->disk_updater, ++(context->device_count));
	disk_updater_set_status(context->disk_updater, "scanning");

	while(mount_point && !g_cancellable_is_cancelled(cancellable)) {
		bundles = g_slist_concat(bundles,
		                         find_rauc_bundles(context,
		                                           cancellable,
		                                           mount_point->data));
		mount_point = g_slist_next(mount_point);
	}
	g_hash_table_insert(context->bundles_by_disk,
	                    NEW_DISK_ID(device),
	                    bundles);
	disk_updater_set_status(context->disk_updater, "idle");   
	
	/* start script install hook*/
	if(!g_cancellable_is_cancelled(cancellable)) {
		run_hook_install(context, cancellable, bundles);	
	}
}


/**
 * @brief Signal callback for a removed device
 *
 * This function is executed in a separate thread of the UdevMonitor.
 *
 * @param[in] UdevMonitor instance
 * @param[in] GUDevDevice instance
 * @param[in] MainContext struct
 */
static void
on_detach(UdevMonitor *monitor,
          GUdevDevice *device,
          gpointer *mount_points,
          gpointer user_data)
{
	//	g_debug("%10s %s", "detached", DEVICE_ID(device));
	MainContext *context = (MainContext*) user_data;

	context->device_count--;
	disk_updater_set_device_count(context->disk_updater, context->device_count);
	if(context->device_count == 0) {
		/* reset bundle counter used for generating bundle interfaces */
		context->bundle_dbus_count = 0;
	}
		
	g_hash_table_remove (context->bundles_by_disk, DISK_ID(device));
}

/**
 * @brief Callback for successful acquiring the bus
 *
 * @param[in] Dbus connection
 * @param[in] Dbus name
 * @param[in] MainContext struct
 */
static void
on_bus_acquired(GDBusConnection *connection,
                 const gchar *name,
                 gpointer user_data)
{
	DiskUpdater *disk_updater;
	MainContext *context = (MainContext*) user_data;
	context->dbus_connection = connection;
	
	disk_updater = disk_updater_skeleton_new();
	context->disk_updater = disk_updater;
	g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(disk_updater),
	                                 connection,
	                                 "/de/helbling/DiskUpdater",
	                                 NULL);
	disk_updater_set_status(disk_updater, "idle");
}

/**
 * @brief Callback for successful acquiring the name
 *
 * @param[in] Dbus connection
 * @param[in] Dbus name
 * @param[in] MainContext struct
 */
static void
on_name_acquired(GDBusConnection *connection,
                 const gchar *name,
                 gpointer user_data)
{
	g_debug("Bus name %s aquired", name);
}

/**
 * @brief Callback for loosing the dbus name
 *
 * This exit the program.
 *
 * @param[in] Dbus connection
 * @param[in] Dbus name
 * @param[in] MainContext struct
 */
static void
on_name_lost (GDBusConnection *connection,
              const gchar *name,
              gpointer user_data)
{
	MainContext *context = (MainContext*) user_data;	
	g_warning("Failed to aquire dbus name");
	context->exit_code = 4;
	g_main_loop_quit(context->loop);
}

/**
 * @brief Callback for exiting the program by the SIGTERM signal
 *
 * This exit the program.
 *
 * @param[in] Dbus connection
 * @param[in] Dbus name
 * @param[in] MainContext struct
 */
static gboolean
on_sigterm(gpointer user_data)
{
	MainContext *context = (MainContext*) user_data;
	context->exit_code = 0;
	g_main_loop_quit(context->loop);
	return G_SOURCE_REMOVE;
}


int main(int argc, char **argv) {
	
	gchar **args;
	gint exit_code;
	GError *error = NULL;
	GOptionContext *option_context;
	guint owner_id;
	MainContext *context;

	context = g_slice_new0(MainContext);
	context->bundles_by_disk = g_hash_table_new_full(g_str_hash,
	                                                 g_str_equal,
	                                                 (GDestroyNotify)g_free,
	                                                 bundles_destroyed);
	
	/* Parse parameter */
	args = g_strdupv(argv); /* support unicode filename */
	option_context = g_option_context_new("");
	g_option_context_add_main_entries(option_context, entries, NULL);
	if (!g_option_context_parse_strv(option_context, &args, &error)) {
		g_printerr("Option parsing failed: %s", error->message);
		g_error_free(error);
		context->exit_code = 1;
		goto out;
	}

	if (opt_version) {
		g_print("Version %.1f\n", VERSION);
		goto out;
	}

	if (script_file != NULL && !g_file_test(script_file, G_FILE_TEST_EXISTS)) {
		g_printerr("No such script file: %s\n", script_file);
		context->exit_code = 2;
		goto out;
	}	
	
	/* connect to rauc */
	context->installer =
		rauc_installer_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
		                                   G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
		                                   "de.pengutronix.rauc",
		                                   "/",
		                                   NULL,
		                                   &error);
	if (context->installer == NULL) {
		g_warning("Error creating proxy: %s", error->message);
		g_clear_error(&error);
		context->exit_code = 3;
		goto out;
	}
	/* get system compatible string just once */
	context->compatible = rauc_installer_dup_compatible(context->installer);
	
	/* register monitor for automatically mounted and unmounted devices */
	context->monitor = udev_monitor_new();
	g_signal_connect (context->monitor, "attach", (GCallback)on_attach, context);
	g_signal_connect (context->monitor, "detach", (GCallback)on_detach, context);
	
	context->loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGTERM, on_sigterm, context);
	g_unix_signal_add(SIGINT, on_sigterm, context);
	
	/* aquire dbus name */
	owner_id = g_bus_own_name(G_BUS_TYPE_SYSTEM,
	                          "de.helbling.DiskUpdater",
	                          G_BUS_NAME_OWNER_FLAGS_NONE,
	                          on_bus_acquired,
	                          on_name_acquired,
	                          on_name_lost,
	                          context,
	                          NULL);

	/* enter main loop */
	g_main_loop_run(context->loop);
	
	/* main loop leaved, cleanup */
	g_main_loop_unref(context->loop);	
	
	/* free udev monitor */
	g_signal_handlers_disconnect_by_data(context->monitor, context);
	udev_monitor_quit(context->monitor);
	g_object_unref(context->monitor);

	/* unown dbus name */
	g_bus_unown_name(owner_id);

 out:
	g_option_context_free(option_context);

	exit_code = context->exit_code;
	g_free(context->compatible);
	g_slice_free(MainContext, context);
	return exit_code;
}
