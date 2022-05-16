/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2022 Benjamin Berg <bberg@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <string.h>

#include <gudev/gudev.h>
#include "up-device.h"
#include "up-config.h"
#include "up-enumerator-udev.h"

#include "up-device-supply.h"
#include "up-device-hid.h"
#include "up-device-wup.h"

struct _UpEnumeratorUdev {
	UpEnumerator parent;

	GUdevClient *udev;

	/* Contains either a GUdevDevice or a UpDevice wrapping it. */
	GHashTable *known;
	GHashTable *siblings;
};

G_DEFINE_TYPE (UpEnumeratorUdev, up_enumerator_udev, UP_TYPE_ENUMERATOR)

static char*
device_parent_id (GUdevDevice *dev)
{
	g_autoptr(GUdevDevice) parent = NULL;
	const char *subsystem;

	parent = g_udev_device_get_parent (dev);
	if (!parent)
		return NULL;

	subsystem = g_udev_device_get_subsystem (parent);

	/* Refusing using certain subsystems as parents. */
	if (g_strcmp0 (subsystem, "platform") == 0)
		return NULL;

	/* Continue walk if the parent is a "hid" or "input" device  */
	if (g_strcmp0 (subsystem, "hid") == 0 ||
	    g_strcmp0 (subsystem, "input") == 0)
		return device_parent_id (parent);

	/* Also skip over USB interfaces, we care about full devices */
	if (g_strcmp0 (subsystem, "usb") == 0 &&
	    g_strcmp0 (g_udev_device_get_devtype (parent), "usb_interface") == 0)
		return device_parent_id (parent);

	return g_strdup (g_udev_device_get_sysfs_path (parent));
}

static gpointer
is_macbook (gpointer data)
{
	g_autofree char *product = NULL;

	if (!g_file_get_contents ("/sys/devices/virtual/dmi/id/product_name", &product, NULL, NULL) ||
	    product == NULL)
		return GINT_TO_POINTER(FALSE);
	return GINT_TO_POINTER(g_str_has_prefix (product, "MacBook"));
}

static UpDevice *
device_new (UpEnumeratorUdev *self, GUdevDevice *native)
{
	UpDaemon *daemon;
	const gchar *subsys;
	const gchar *native_path;

	daemon = up_enumerator_get_daemon (UP_ENUMERATOR (self));

	subsys = g_udev_device_get_subsystem (native);
	if (g_strcmp0 (subsys, "power_supply") == 0) {
		return g_initable_new (UP_TYPE_DEVICE_SUPPLY, NULL, NULL,
		                       "daemon", daemon,
		                       "native", native,
		                       "ignore-system-percentage", GPOINTER_TO_INT (is_macbook (NULL)),
		                       NULL);

	} else if (g_strcmp0 (subsys, "tty") == 0) {
		return g_initable_new (UP_TYPE_DEVICE_WUP, NULL, NULL,
		                       "daemon", daemon,
		                       "native", native,
		                       NULL);

	} else if (g_strcmp0 (subsys, "usbmisc") == 0) {
#ifdef HAVE_IDEVICE
		UpDevice *device;

		device = g_initable_new (UP_TYPE_DEVICE_IDEVICE, NULL, NULL,
		                         "daemon", daemon,
		                         "native", native,
		                         NULL);
		if (device)
			return device;
#endif /* HAVE_IDEVICE */

		return g_initable_new (UP_TYPE_DEVICE_HID, NULL, NULL,
		                       "daemon", daemon,
		                       "native", native,
		                       NULL);

	} else if (g_strcmp0 (subsys, "input") == 0) {
		/* Ignore, we only resolve them to see siblings. */
		return NULL;
	} else {
		native_path = g_udev_device_get_sysfs_path (native);
		g_warning ("native path %s (%s) ignoring", native_path, subsys);
		return NULL;
	}
}

static void
uevent_signal_handler_cb (UpEnumeratorUdev *self,
                          const gchar      *action,
                          GUdevDevice      *device,
                          GUdevClient      *client)
{
	const char *device_key = g_udev_device_get_sysfs_path (device);

	/* Work around the fact that we don't get a REMOVE event in some cases. */
	if (g_strcmp0 (g_udev_device_get_subsystem (device), "power_supply") == 0)
		device_key = g_udev_device_get_name (device);

	/* It appears that we may not always receive an "add" event. As such,
	 * treat "add"/"change" in the same way, by first checking if we have
	 * seen the device.
	 * Even worse, we may not get a "remove" event in some odd cases, so
	 * if there is an "add" but we find the device (as the power_supply
	 * node has the same name), then remove it first before adding the
	 * new one.
	 */
	if (g_strcmp0 (action, "change") == 0 || g_strcmp0 (action, "add") == 0) {
		GObject *obj;

		obj = g_hash_table_lookup (self->known, device_key);
		if (UP_IS_DEVICE (obj) && g_strcmp0 (action, "add") == 0 &&
		    g_strcmp0 (g_udev_device_get_sysfs_path (device),
		               g_udev_device_get_sysfs_path (G_UDEV_DEVICE (up_device_get_native (UP_DEVICE (obj))))) != 0) {
			uevent_signal_handler_cb (self, "remove", device, client);
			obj = NULL;
		}

		if (!obj) {
			g_autoptr(UpDevice) up_dev = NULL;
			g_autofree char *parent_id = NULL;

			up_dev = device_new (self, device);

			/* We work with `obj` further down, which is the UpDevice
			 * if we have it, or the GUdevDevice if not. */
			if (up_dev)
				obj = G_OBJECT (up_dev);
			else
				obj = G_OBJECT (device);
			g_hash_table_insert (self->known, (char*) device_key, g_object_ref (obj));

			/* Fire relevant sibling events and insert into lookup table */
			parent_id = device_parent_id (device);
			g_debug ("device %s has parent id: %s", device_key, parent_id);
			if (parent_id) {
				gboolean insert = FALSE;
				GPtrArray *devices;
				int i;

				devices = g_hash_table_lookup (self->siblings, parent_id);
				if (!devices) {
					insert = TRUE;
					devices = g_ptr_array_new_with_free_func (g_object_unref);
				}

				for (i = 0; i < devices->len; i++) {
					GObject *sibling = g_ptr_array_index (devices, i);

					if (up_dev)
						up_device_sibling_discovered (up_dev, sibling);
					if (UP_IS_DEVICE (sibling))
						up_device_sibling_discovered (UP_DEVICE (sibling), obj);
				}

				g_ptr_array_add (devices, g_object_ref (obj));
				if (insert)
					g_hash_table_insert (self->siblings, g_strdup (parent_id), devices);
			}

			if (up_dev)
				g_signal_emit_by_name (self, "device-added", up_dev);

		} else {
			if (!UP_IS_DEVICE (obj))
				return;

			g_debug ("refreshing device for path %s", g_udev_device_get_sysfs_path (device));
			if (!up_device_refresh_internal (UP_DEVICE (obj), UP_REFRESH_EVENT))
				g_debug ("no changes on %s", up_device_get_object_path (UP_DEVICE (obj)));

		}
	} else if (g_strcmp0 (action, "remove") == 0) {
		g_autoptr(GObject) obj = NULL;
		const char *key = NULL;

		g_debug ("removing device for path %s", g_udev_device_get_sysfs_path (device));
		g_hash_table_steal_extended (self->known, device_key,
		                             (gpointer*) &key, (gpointer*) &obj);

		if (obj) {
			g_autofree char *parent_id = device_parent_id (device);

			/* Remove from siblings table */
			if (parent_id) {
				GPtrArray *devices;

				devices = g_hash_table_lookup (self->siblings, parent_id);

				g_ptr_array_remove_fast (devices, obj);
			}
		}

		if (obj && UP_IS_DEVICE (obj)) {
			g_signal_emit_by_name (self, "device-removed", obj);
		} else
			g_debug ("ignoring remove event on %s", g_udev_device_get_sysfs_path (device));
	}
}

static void
up_enumerator_udev_init (UpEnumeratorUdev *self)
{
	self->known = g_hash_table_new_full (g_str_hash, g_str_equal,
					     NULL, g_object_unref);
	self->siblings = g_hash_table_new_full (g_str_hash, g_str_equal,
						g_free, (GDestroyNotify) g_ptr_array_unref);
}

static void
up_enumerator_udev_initable_init (UpEnumerator *enumerator)
{
	g_autoptr(UpConfig) config = NULL;
	UpEnumeratorUdev *self = UP_ENUMERATOR_UDEV (enumerator);
	GUdevDevice *native;
	guint i;
	const gchar **subsystems;
	/* List "input" first just to avoid some sibling hotplugging later */
	const gchar *subsystems_no_wup[] = {"input", "power_supply", "usbmisc", NULL};
	const gchar *subsystems_wup[] = {"input", "power_supply", "usbmisc", "tty", NULL};

	config = up_config_new ();
	if (up_config_get_boolean (config, "EnableWattsUpPro"))
		subsystems = subsystems_wup;
	else
		subsystems = subsystems_no_wup;

	self->udev = g_udev_client_new (subsystems);
	g_signal_connect_swapped (self->udev, "uevent",
				  G_CALLBACK (uevent_signal_handler_cb), self);

	/* Emulate hotplug for existing devices */
	for (i = 0; subsystems[i] != NULL; i++) {
		g_autolist(GUdevDevice) devices = NULL;
		GList *l;

		g_debug ("registering subsystem : %s", subsystems[i]);
		devices = g_udev_client_query_by_subsystem (self->udev, subsystems[i]);
		for (l = devices; l != NULL; l = l->next) {
			native = l->data;
			uevent_signal_handler_cb (self, "add", native, self->udev);
		}
	}
}

static void
up_enumerator_udev_dispose (GObject *obj)
{
	UpEnumeratorUdev *self = UP_ENUMERATOR_UDEV (obj);

	g_clear_object (&self->udev);
	g_hash_table_remove_all (self->known);
	g_hash_table_remove_all (self->siblings);

	G_OBJECT_CLASS (up_enumerator_udev_parent_class)->dispose (obj);
}

static void
up_enumerator_udev_finalize (GObject *obj)
{
	UpEnumeratorUdev *self = UP_ENUMERATOR_UDEV (obj);

	g_clear_pointer (&self->known, g_hash_table_unref);
	g_clear_pointer (&self->siblings, g_hash_table_unref);

	G_OBJECT_CLASS (up_enumerator_udev_parent_class)->finalize (obj);
}

static void
up_enumerator_udev_class_init (UpEnumeratorUdevClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->dispose = up_enumerator_udev_dispose;
	object_class->finalize = up_enumerator_udev_finalize;

	UP_ENUMERATOR_CLASS (klass)->initable_init = up_enumerator_udev_initable_init;
}