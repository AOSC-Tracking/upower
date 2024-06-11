/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2011 Richard Hughes <richard@hughsie.com>
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

#include <glib-object.h>
#include <gio/gio.h>

#include "up-config.h"

static void     up_config_finalize	(GObject     *object);

/**
 * UpConfigPrivate:
 *
 * Private #UpConfig data
 **/
struct _UpConfigPrivate
{
	GKeyFile			*keyfile;
};

G_DEFINE_TYPE_WITH_PRIVATE (UpConfig, up_config, G_TYPE_OBJECT)

static gpointer up_config_object = NULL;

/**
 * up_config_get_boolean:
 **/
gboolean
up_config_get_boolean (UpConfig *config, const gchar *key)
{
	return g_key_file_get_boolean (config->priv->keyfile,
				       "UPower", key, NULL);
}

/**
 * up_config_get_uint:
 **/
guint
up_config_get_uint (UpConfig *config, const gchar *key)
{
	int val;

	val = g_key_file_get_integer (config->priv->keyfile,
				      "UPower", key, NULL);
	if (val < 0)
		return 0;

	return val;
}

/**
 * up_config_get_string:
 **/
gchar *
up_config_get_string (UpConfig *config, const gchar *key)
{
	return g_key_file_get_string (config->priv->keyfile,
				      "UPower", key, NULL);
}

/**
 * up_config_class_init:
 **/
static void
up_config_class_init (UpConfigClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = up_config_finalize;
}

/**
 * up_config_init:
 **/
static void
up_config_init (UpConfig *config)
{
	gboolean allow_risky_critical_action = FALSE;
	g_autofree gchar *critical_action = NULL;
	GError *error = NULL;
	g_autofree gchar *filename = NULL;
	gboolean ret;

	config->priv = up_config_get_instance_private (config);
	config->priv->keyfile = g_key_file_new ();

	filename = g_strdup (g_getenv ("UPOWER_CONF_FILE_NAME"));
	if (filename == NULL)
		filename = g_build_filename (PACKAGE_SYSCONF_DIR,"UPower", "UPower.conf", NULL);

	/* load */
	ret = g_key_file_load_from_file (config->priv->keyfile,
					 filename,
					 G_KEY_FILE_NONE,
					 &error);

	if (!ret) {
		g_warning ("failed to load config file '%s': %s",
			   filename, error->message);
		g_error_free (error);
	}

	/* Warn for any dangerous configurations */
	critical_action = up_config_get_string (config, "CriticalPowerAction");
	allow_risky_critical_action = up_config_get_boolean (config, "AllowRiskyCriticalPowerAction");

	if (!g_strcmp0 (critical_action, "Suspend") || !g_strcmp0 (critical_action, "Ignore")) {
		if (allow_risky_critical_action) {
			g_warning ("The \"%s\" CriticalPowerAction setting is considered risky:"
				   " abrupt power loss due to battery exhaustion may lead to data"
				   " corruption. Use AllowRiskyCriticalPowerAction=false to disable"
				   " support for risky settings.", critical_action);
		} else {
			g_warning ("The \"%s\" CriticalPowerAction setting is considered risky:"
				   " abrupt power loss due to battery exhaustion may lead to data"
				   " corruption. The system will perform \"HybridSleep\" instead."
				   " Use AllowRiskyCriticalPowerAction=true to enable support for"
				   " risky settings.", critical_action);
		}
	}
}

/**
 * up_config_finalize:
 **/
static void
up_config_finalize (GObject *object)
{
	UpConfig *config = UP_CONFIG (object);
	UpConfigPrivate *priv = config->priv;

	g_key_file_free (priv->keyfile);

	G_OBJECT_CLASS (up_config_parent_class)->finalize (object);
}

/**
 * up_config_new:
 **/
UpConfig *
up_config_new (void)
{
	if (up_config_object != NULL) {
		g_object_ref (up_config_object);
	} else {
		up_config_object = g_object_new (UP_TYPE_CONFIG, NULL);
		g_object_add_weak_pointer (up_config_object, &up_config_object);
	}
	return UP_CONFIG (up_config_object);
}

