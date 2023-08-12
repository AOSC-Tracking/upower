/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 Richard Hughes <richard@hughsie.com>
 *               2010 Alex Murray <murray.alex@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>

#include "up-kbd-backlight.h"
#include "up-daemon.h"
#include "up-types.h"

static void     up_kbd_backlight_finalize   (GObject	*object);
static void     up_kbd_backlight_device_finalize   (GObject	*object);

struct UpKbdBacklightPrivate
{
	GPtrArray		 devices;
	gint			 max_brightness;
};

struct UpKbdBacklightDevicePrivate
{
	gchar 			*name;
	gint			 fd;
	gint			 fd_hw_changed;
	GIOChannel		*channel_hw_changed;
	gint			 max_brightness;
};

G_DEFINE_TYPE_WITH_PRIVATE (UpKbdBacklight, up_kbd_backlight, UP_TYPE_EXPORTED_KBD_BACKLIGHT_SKELETON)
G_DEFINE_TYPE_WITH_PRIVATE (UpKbdBacklightDevice, up_kbd_backlight_device, G_TYPE_OBJECT)

/**
 * up_kbd_backlight_emit_change:
 **/
static void
up_kbd_backlight_emit_change(UpKbdBacklight *kbd_backlight, int value, const char *source)
{
	up_exported_kbd_backlight_emit_brightness_changed (UP_EXPORTED_KBD_BACKLIGHT (kbd_backlight), value);
	up_exported_kbd_backlight_emit_brightness_changed_with_source (UP_EXPORTED_KBD_BACKLIGHT (kbd_backlight), value, source);
}

/**
 * up_kbd_backlight_device_brightness_read:
 **/
gint
up_kbd_backlight_device_brightness_read (UpKbdBacklightDevice *kbd_backlight_device, int fd)
{
	gchar buf[16];
	gchar *end = NULL;
	ssize_t len;
	gint64 brightness = -1;

	g_return_val_if_fail (fd >= 0, brightness);

	lseek (fd, 0, SEEK_SET);
	len = read (fd, buf, G_N_ELEMENTS (buf) - 1);

	if (len > 0) {
		buf[len] = '\0';
		brightness = g_ascii_strtoll (buf, &end, 10);

		if (brightness < 0 ||
		    brightness > kbd_backlight_device->priv->max_brightness ||
		    end == buf) {
			brightness = -1;
			g_warning ("failed to convert brightness for device %s: %s", kbd_backlight_device->priv->name, buf);
		}
	}

	return brightness;
}

/**
 * up_kbd_backlight_device_brightness_write:
 **/
gboolean
up_kbd_backlight_device_brightness_write (UpKbdBacklightDevice *kbd_backlight_device, gint value)
{
	gchar *text = NULL;
	gint retval;
	gint length;
	gboolean ret = TRUE;

	/* write new values to backlight */
	if (kbd_backlight_device->priv->fd < 0) {
		g_warning ("cannot write to device %s as file not open", kbd_backlight_device->priv->name);
		ret = FALSE;
		goto out;
	}

	/* limit to between 0 and max */
	value = CLAMP (value, 0, kbd_backlight_device->priv->max_brightness);

	/* convert to text */
	text = g_strdup_printf ("%i", value);
	length = strlen (text);

	/* write to file */
	lseek (kbd_backlight_device->priv->fd, 0, SEEK_SET);
	retval = write (kbd_backlight_device->priv->fd, text, length);
	if (retval != length) {
		g_warning ("writing '%s' to device %s failed", text, kbd_backlight_device->priv->name);
		ret = FALSE;
		goto out;
	}

out:
	g_free (text);
	return ret;
}

/**
 * up_kbd_backlight_get_brightness:
 *
 * Gets the current brightness
 **/
static gboolean
up_kbd_backlight_get_brightness (UpExportedKbdBacklight *skeleton,
				 GDBusMethodInvocation *invocation,
				 UpKbdBacklight *kbd_backlight)
{
	gint univ_brightness = -1;

	/* read brightness and check that it is the same for all devices */
	for (guint i = 0; i < (&kbd_backlight->priv->devices)->len; i++) {
		UpKbdBacklightDevice *kbd_backlight_device = (UpKbdBacklightDevice *) g_ptr_array_index (&kbd_backlight->priv->devices, i);

		/* read brightness */
		gint curr_brightness = up_kbd_backlight_device_brightness_read (kbd_backlight_device, kbd_backlight_device->priv->fd);

		if (curr_brightness < 0) {
			g_dbus_method_invocation_return_error (invocation,
						       UP_DAEMON_ERROR, UP_DAEMON_ERROR_GENERAL,
						       "error reading brightness for device %s", kbd_backlight_device->priv->name);
		} else if (univ_brightness < 0) {
			univ_brightness = curr_brightness;
		} else if (univ_brightness != curr_brightness) {
			g_dbus_method_invocation_return_error (invocation,
							UP_DAEMON_ERROR, UP_DAEMON_ERROR_GENERAL,
							"multiple backlights with different brightnesses");
		}
	}

	up_exported_kbd_backlight_complete_get_brightness (skeleton, invocation,
								   univ_brightness);

	return TRUE;

}

/**
 * up_kbd_backlight_get_max_brightness:
 *
 * Gets the max brightness
 **/
static gboolean
up_kbd_backlight_get_max_brightness (UpExportedKbdBacklight *skeleton,
				     GDBusMethodInvocation *invocation,
				     UpKbdBacklight *kbd_backlight)
{
	gint univ_max_brightness = kbd_backlight->priv->max_brightness;

	if (univ_max_brightness < 0) {
					g_dbus_method_invocation_return_error (invocation,
							UP_DAEMON_ERROR, UP_DAEMON_ERROR_GENERAL,
							"multiple backlights with different maximum brightnesses");
	} else {
		up_exported_kbd_backlight_complete_get_max_brightness (skeleton, invocation,
								univ_max_brightness);
	}
	return TRUE;
}

/**
 * up_kbd_backlight_set_brightness:
 **/
static gboolean
up_kbd_backlight_set_brightness (UpExportedKbdBacklight *skeleton,
				 GDBusMethodInvocation *invocation,
				 gint value,
				 UpKbdBacklight *kbd_backlight)
{
	g_debug ("setting brightness to %i", value);

	for (guint i = 0; i < (&kbd_backlight->priv->devices)->len; i++) {
		UpKbdBacklightDevice *kbd_backlight_device = (UpKbdBacklightDevice *) g_ptr_array_index (&kbd_backlight->priv->devices, i);

		gboolean ret = up_kbd_backlight_device_brightness_write (kbd_backlight_device, value);

		if (!ret) {
			g_dbus_method_invocation_return_error (invocation,
						UP_DAEMON_ERROR, UP_DAEMON_ERROR_GENERAL,
						"error writing brightness %d for device %s", value, kbd_backlight_device->priv->name);
		}
	}

	up_kbd_backlight_emit_change(kbd_backlight, value, "external");

	up_exported_kbd_backlight_complete_set_brightness (skeleton, invocation);
	return TRUE;
}

/**
 * up_kbd_backlight_class_init:
 **/
static void
up_kbd_backlight_class_init (UpKbdBacklightClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = up_kbd_backlight_finalize;
}

/**
 * up_kbd_backlight_device_class_init:
 **/
static void
up_kbd_backlight_device_class_init (UpKbdBacklightDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = up_kbd_backlight_device_finalize;
}

/**
 * up_kbd_backlight_event_io:
 **/
static gboolean
up_kbd_backlight_event_io (GIOChannel *channel, GIOCondition condition, gpointer data)
{
	GPtrArray *data_array = (GPtrArray*) data;
	UpKbdBacklight *kbd_backlight = (UpKbdBacklight*) g_ptr_array_index(data_array, 0);
	UpKbdBacklightDevice *kbd_backlight_device = (UpKbdBacklightDevice*) g_ptr_array_index(data_array, 1);
	gint brightness;

	if (!(condition & G_IO_PRI))
		return FALSE;

	brightness = up_kbd_backlight_device_brightness_read (kbd_backlight_device, kbd_backlight_device->priv->fd_hw_changed);
	if (brightness < 0 && errno == ENODEV)
		return FALSE;

	if (brightness >= 0)
		up_kbd_backlight_emit_change (kbd_backlight, brightness, "internal");

	return TRUE;


	return TRUE;
}

/**
 * up_kbd_backlight_sort_devices:
 **/
gint up_kbd_backlight_sort_devices (gconstpointer a, gconstpointer b) {
	gchar *fn_a = g_utf8_collate_key_for_filename((*(UpKbdBacklightDevice **) a)->priv->name, -1);
	gchar *fn_b = g_utf8_collate_key_for_filename((*(UpKbdBacklightDevice **) b)->priv->name, -1);
	return g_strcmp0(fn_a, fn_b);
}

/**
 * up_kbd_backlight_find:
 **/
static gboolean
up_kbd_backlight_find (UpKbdBacklight *kbd_backlight)
{
	gboolean ret;
	gboolean found = FALSE;
	GDir *dir;
	const gchar *filename;
	gchar *end = NULL;
	gchar *dir_path = NULL;
	gchar *path_max = NULL;
	gchar *path_now = NULL;
	gchar *path_hw_changed = NULL;
	gchar *buf_max = NULL;
	gchar *buf_now = NULL;
	GError *error = NULL;

	/* open directory */
	dir = g_dir_open ("/sys/class/leds", 0, &error);
	if (dir == NULL) {
		if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			g_warning ("failed to open directory: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* find all led devices that are a keyboard device */
	while ((filename = g_dir_read_name (dir)) != NULL) {
		if (g_strstr_len (filename, -1, "kbd_backlight") != NULL) {
			dir_path = g_build_filename ("/sys/class/leds",
						    filename, NULL);
		} else {
			continue;
		}

		/* nothing found */
		if (dir_path == NULL)
			goto out;

		/* register device */
		UpKbdBacklightDevice *kbd_backlight_device = (UpKbdBacklightDevice*) up_kbd_backlight_device_new();
		kbd_backlight_device->priv = up_kbd_backlight_device_get_instance_private (kbd_backlight_device);
		kbd_backlight_device->priv->name = g_strdup(filename);
		g_ptr_array_add(&kbd_backlight->priv->devices, kbd_backlight_device);

		/* read max brightness and check if it is the same for all devices */
		path_max = g_build_filename (dir_path, "max_brightness", NULL);
		ret = g_file_get_contents (path_max, &buf_max, NULL, &error);
		if (!ret) {
			g_warning ("failed to get max brightness for device %s: %s", filename, error->message);
			g_error_free (error);
			found = FALSE;
			goto out;
		}
		kbd_backlight_device->priv->max_brightness = g_ascii_strtoull (buf_max, &end, 10);
		if (kbd_backlight_device->priv->max_brightness == 0 && end == buf_max) {
			g_warning ("failed to convert max brightness for %s: %s", filename, buf_max);
			found = FALSE;
			goto out;
		}
		if (kbd_backlight->priv->max_brightness == 0) {
			kbd_backlight->priv->max_brightness = kbd_backlight_device->priv->max_brightness;
		}
		if (kbd_backlight->priv->max_brightness != kbd_backlight_device->priv->max_brightness) {
			kbd_backlight->priv->max_brightness = -1;
			g_warning("multiple backlights with different maximum brightnesses");
		}

		/* open the brightness file for read and write operations */
		path_now = g_build_filename (dir_path, "brightness", NULL);
		kbd_backlight_device->priv->fd = open (path_now, O_RDWR);

		/* read brightness and check if it has an acceptable value */
		if (up_kbd_backlight_device_brightness_read (kbd_backlight_device, kbd_backlight_device->priv->fd) < 0)
			goto out;

		path_hw_changed = g_build_filename (dir_path, "brightness_hw_changed", NULL);
		kbd_backlight_device->priv->fd_hw_changed = open (path_hw_changed, O_RDONLY);
		if (kbd_backlight_device->priv->fd_hw_changed >= 0) {
			kbd_backlight_device->priv->channel_hw_changed = g_io_channel_unix_new (kbd_backlight_device->priv->fd_hw_changed);
			GPtrArray *data = g_ptr_array_new();
			g_ptr_array_add(data, kbd_backlight);
			g_ptr_array_add(data, kbd_backlight_device);
			g_io_add_watch (kbd_backlight_device->priv->channel_hw_changed,
					G_IO_PRI, up_kbd_backlight_event_io, data);
		}

		/* success */
		found = TRUE;
	}
	/* sort so that keys would light up one next to the other rather than in a random order */
	g_ptr_array_sort(&kbd_backlight->priv->devices, up_kbd_backlight_sort_devices);

out:
	if (dir != NULL)
		g_dir_close (dir);
	g_free (dir_path);
	g_free (path_max);
	g_free (path_now);
	g_free (path_hw_changed);
	g_free (buf_max);
	g_free (buf_now);
	return found;
}

/**
 * up_kbd_backlight_init:
 **/
static void
up_kbd_backlight_init (UpKbdBacklight *kbd_backlight)
{
	kbd_backlight->priv = up_kbd_backlight_get_instance_private (kbd_backlight);

	g_signal_connect (kbd_backlight, "handle-get-brightness",
			  G_CALLBACK (up_kbd_backlight_get_brightness), kbd_backlight);
	g_signal_connect (kbd_backlight, "handle-get-max-brightness",
			  G_CALLBACK (up_kbd_backlight_get_max_brightness), kbd_backlight);
	g_signal_connect (kbd_backlight, "handle-set-brightness",
			  G_CALLBACK (up_kbd_backlight_set_brightness), kbd_backlight);
}

/**
 * up_kbd_backlight_device_init:
 **/
static void
up_kbd_backlight_device_init (UpKbdBacklightDevice *kbd_backlight_device)
{
	kbd_backlight_device->priv = up_kbd_backlight_device_get_instance_private (kbd_backlight_device);
}

/**
 * up_kbd_backlight_finalize:
 **/
static void
up_kbd_backlight_finalize (GObject *object)
{
	g_return_if_fail (object != NULL);
	g_return_if_fail (UP_IS_KBD_BACKLIGHT (object));

	G_OBJECT_CLASS (up_kbd_backlight_parent_class)->finalize (object);
}

/**
 * up_kbd_backlight_device_finalize:
 **/
void
up_kbd_backlight_device_finalize (GObject *object)
{
	UpKbdBacklightDevice *kbd_backlight_device;

	g_return_if_fail (object != NULL);
	g_return_if_fail (UP_IS_KBD_BACKLIGHT_DEVICE (object));

	kbd_backlight_device = UP_KBD_BACKLIGHT_DEVICE (object);
	kbd_backlight_device->priv = up_kbd_backlight_device_get_instance_private (kbd_backlight_device);

	if (kbd_backlight_device->priv->channel_hw_changed) {
		g_io_channel_shutdown (kbd_backlight_device->priv->channel_hw_changed, FALSE, NULL);
		g_io_channel_unref (kbd_backlight_device->priv->channel_hw_changed);
	}

	if (kbd_backlight_device->priv->fd_hw_changed >= 0)
		close (kbd_backlight_device->priv->fd_hw_changed);

	/* close file */
	if (kbd_backlight_device->priv->fd >= 0)
		close (kbd_backlight_device->priv->fd);

	G_OBJECT_CLASS (up_kbd_backlight_device_parent_class)->finalize (object);
}

/**
 * up_kbd_backlight_new:
 **/
UpKbdBacklight *
up_kbd_backlight_new (void)
{
	return g_object_new (UP_TYPE_KBD_BACKLIGHT, NULL);
}

/**
 * up_kbd_backlight_device_new:
 **/
UpKbdBacklightDevice *
up_kbd_backlight_device_new (void)
{
	return g_object_new (UP_TYPE_KBD_BACKLIGHT_DEVICE, NULL);
}


void
up_kbd_backlight_register (UpKbdBacklight *kbd_backlight,
			   GDBusConnection *connection)
{
	GError *error = NULL;

	/* find kbd backlights in sysfs */
	if (!up_kbd_backlight_find (kbd_backlight)) {
		g_debug ("cannot find a keyboard backlight");
		return;
	}

	g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (kbd_backlight),
					  connection,
					  "/org/freedesktop/UPower/KbdBacklight",
					  &error);

	if (error != NULL) {
		g_warning ("Cannot export KbdBacklight object to bus: %s", error->message);
		g_error_free (error);
	}
}

