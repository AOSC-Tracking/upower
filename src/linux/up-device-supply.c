/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 David Zeuthen <davidz@redhat.com>
 * Copyright (C) 2008 Richard Hughes <richard@hughsie.com>
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

#include "config.h"

#include <string.h>
#include <math.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>
#include <glib/gi18n-lib.h>
#include <glib-object.h>
#include <gudev/gudev.h>

#include "up-config.h"
#include "up-types.h"
#include "up-constants.h"
#include "up-device-supply.h"
#include "up-common.h"

enum {
	PROP_0,
	PROP_IGNORE_SYSTEM_PERCENTAGE
};

#define UP_DEVICE_SUPPLY_CHARGED_THRESHOLD	90.0f	/* % */

#define UP_DEVICE_SUPPLY_COLDPLUG_UNITS_CHARGE		TRUE
#define UP_DEVICE_SUPPLY_COLDPLUG_UNITS_ENERGY		FALSE

/* number of old energy values to keep cached */
#define UP_DEVICE_SUPPLY_ENERGY_OLD_LENGTH		4

struct UpDeviceSupplyPrivate
{
	gboolean		 has_coldplug_values;
	gboolean		 coldplug_units;
	gdouble			*energy_old;
	GTimeVal		*energy_old_timespec;
	guint			 energy_old_first;
	gdouble			 rate_old;
	gint64			 fast_repoll_until;
	gboolean		 disable_battery_poll; /* from configuration */
	gboolean		 shown_invalid_voltage_warning;
	gboolean		 ignore_system_percentage;
};

G_DEFINE_TYPE_WITH_PRIVATE (UpDeviceSupply, up_device_supply, UP_TYPE_DEVICE)

static gboolean		 up_device_supply_refresh	 	(UpDevice *device,
								 UpRefreshReason reason);
static void		 up_device_supply_update_poll_frequency	(UpDevice       *device,
								 UpDeviceState   state,
								 UpRefreshReason reason);
static UpDeviceKind	 up_device_supply_guess_type		(GUdevDevice *native,
								 const char *native_path);

static gboolean
up_device_supply_refresh_line_power (UpDeviceSupply *supply,
				     UpRefreshReason reason)
{
	UpDevice *device = UP_DEVICE (supply);
	GUdevDevice *native;
	gboolean online_old, online_new;

	/* get new AC value */
	native = G_UDEV_DEVICE (up_device_get_native (device));

	g_object_get (device,
		      "online", &online_old,
		      NULL);
	online_new = g_udev_device_get_sysfs_attr_as_int_uncached (native, "online");
	/* Avoid notification if the value did not change. */
	if (online_old != online_new)
		g_object_set (device,
			      "online", online_new,
			      NULL);

	return TRUE;
}

/**
 * up_device_supply_reset_values:
 **/
static void
up_device_supply_reset_values (UpDeviceSupply *supply)
{
	UpDevice *device = UP_DEVICE (supply);
	guint i;

	supply->priv->has_coldplug_values = FALSE;
	supply->priv->coldplug_units = UP_DEVICE_SUPPLY_COLDPLUG_UNITS_ENERGY;
	supply->priv->rate_old = 0;

	for (i = 0; i < UP_DEVICE_SUPPLY_ENERGY_OLD_LENGTH; ++i) {
		supply->priv->energy_old[i] = 0.0f;
		supply->priv->energy_old_timespec[i].tv_sec = 0;
	}
	supply->priv->energy_old_first = 0;

	/* reset to default */
	g_object_set (device,
		      "vendor", NULL,
		      "model", NULL,
		      "serial", NULL,
		      "update-time", (guint64) 0,
		      "online", FALSE,
		      "energy", (gdouble) 0.0,
		      "is-present", FALSE,
		      "is-rechargeable", FALSE,
		      "has-history", FALSE,
		      "has-statistics", FALSE,
		      "state", UP_DEVICE_STATE_UNKNOWN,
		      "capacity", (gdouble) 0.0,
		      "energy-empty", (gdouble) 0.0,
		      "energy-full", (gdouble) 0.0,
		      "energy-full-design", (gdouble) 0.0,
		      "energy-rate", (gdouble) 0.0,
		      "voltage", (gdouble) 0.0,
		      "time-to-empty", (gint64) 0,
		      "time-to-full", (gint64) 0,
		      "percentage", (gdouble) 0.0,
		      "temperature", (gdouble) 0.0,
		      "technology", UP_DEVICE_TECHNOLOGY_UNKNOWN,
		      "charge-cycles", -1,
		      NULL);
}

/**
 * up_device_supply_get_on_battery:
 **/
static gboolean
up_device_supply_get_on_battery (UpDevice *device, gboolean *on_battery)
{
	UpDeviceSupply *supply = UP_DEVICE_SUPPLY (device);
	UpDeviceKind type;
	UpDeviceState state;
	gboolean is_power_supply;
	gboolean is_present;

	g_return_val_if_fail (UP_IS_DEVICE_SUPPLY (supply), FALSE);
	g_return_val_if_fail (on_battery != NULL, FALSE);

	g_object_get (device,
		      "type", &type,
		      "state", &state,
		      "is-present", &is_present,
		      "power-supply", &is_power_supply,
		      NULL);

	if (!is_power_supply)
		return FALSE;
	if (type != UP_DEVICE_KIND_BATTERY)
		return FALSE;
	if (state == UP_DEVICE_STATE_UNKNOWN)
		return FALSE;
	if (!is_present)
		return FALSE;

	*on_battery = (state == UP_DEVICE_STATE_DISCHARGING);
	return TRUE;
}

/**
 * up_device_supply_get_online:
 **/
static gboolean
up_device_supply_get_online (UpDevice *device, gboolean *online)
{
	UpDeviceSupply *supply = UP_DEVICE_SUPPLY (device);
	UpDeviceKind type;
	gboolean online_tmp;

	g_return_val_if_fail (UP_IS_DEVICE_SUPPLY (supply), FALSE);
	g_return_val_if_fail (online != NULL, FALSE);

	g_object_get (device,
		      "type", &type,
		      "online", &online_tmp,
		      NULL);

	if (type != UP_DEVICE_KIND_LINE_POWER)
		return FALSE;

	*online = online_tmp;

	return TRUE;
}

/**
 * up_device_supply_push_new_energy:
 *
 * Store the new energy in the list of old energies of the supply, so
 * it can be used to determine the energy rate.
 */
static gboolean
up_device_supply_push_new_energy (UpDeviceSupply *supply, gdouble energy)
{
	guint first = supply->priv->energy_old_first;
	guint new_position = (first + UP_DEVICE_SUPPLY_ENERGY_OLD_LENGTH - 1) %
		UP_DEVICE_SUPPLY_ENERGY_OLD_LENGTH;

	/* check if the energy value has changed and, if that's the case,
	 * store the new values in the buffer. */
	if (supply->priv->energy_old[first] != energy) {
		supply->priv->energy_old[new_position] = energy;
		g_get_current_time (&supply->priv->energy_old_timespec[new_position]);
		supply->priv->energy_old_first = new_position;
		return TRUE;
	}

	return FALSE;
}

/**
 * up_device_supply_calculate_rate:
 **/
static gdouble
up_device_supply_calculate_rate (UpDeviceSupply *supply, gdouble energy)
{
	gdouble rate = 0.0f;
	gdouble sum_x = 0.0f; /* sum of the squared times difference */
	GTimeVal now;
	guint i;
	guint valid_values = 0;

	/* get the time difference from now and use linear regression to determine
	 * the discharge rate of the battery. */
	g_get_current_time (&now);

	/* store the data on the new energy received */
	up_device_supply_push_new_energy (supply, energy);

	if (energy < 0.1f)
		return 0.0f;

	if (supply->priv->energy_old[supply->priv->energy_old_first] < 0.1f)
		return 0.0f;

	/* don't use the new point obtained since it may cause instability in
	 * the estimate */
	i = supply->priv->energy_old_first;
	now = supply->priv->energy_old_timespec[i];
	do {
		/* only use this value if it seems valid */
		if (supply->priv->energy_old_timespec[i].tv_sec && supply->priv->energy_old[i]) {
			/* This is the square of t_i^2 */
			sum_x += (now.tv_sec - supply->priv->energy_old_timespec[i].tv_sec) *
				(now.tv_sec - supply->priv->energy_old_timespec[i].tv_sec);

			/* Sum the module of the energy difference */
			rate += fabs ((supply->priv->energy_old_timespec[i].tv_sec - now.tv_sec) *
				      (energy - supply->priv->energy_old[i]));
			valid_values++;
		}

		/* get the next element in the circular buffer */
		i = (i + 1) % UP_DEVICE_SUPPLY_ENERGY_OLD_LENGTH;
	} while (i != supply->priv->energy_old_first);

	/* Check that at least 3 points were involved in computation */
	if (sum_x == 0.0f || valid_values < 3)
		return supply->priv->rate_old;

	/* Compute the discharge per hour, and not per second */
	rate /= sum_x / SECONDS_PER_HOUR_F;

	/* if the rate is zero, use the old rate. It will usually happens if no
	 * data is in the buffer yet. If the rate is too high, i.e. more than,
	 * 100W don't use it. */
	if (rate == 0.0f || rate > 100.0f)
		return supply->priv->rate_old;

	return rate;
}

/**
 * up_device_supply_convert_device_technology:
 **/
static UpDeviceTechnology
up_device_supply_convert_device_technology (const gchar *type)
{
	if (type == NULL)
		return UP_DEVICE_TECHNOLOGY_UNKNOWN;
	/* every case combination of Li-Ion is commonly used.. */
	if (g_ascii_strcasecmp (type, "li-ion") == 0 ||
	    g_ascii_strcasecmp (type, "lion") == 0)
		return UP_DEVICE_TECHNOLOGY_LITHIUM_ION;
	if (g_ascii_strcasecmp (type, "pb") == 0 ||
	    g_ascii_strcasecmp (type, "pbac") == 0)
		return UP_DEVICE_TECHNOLOGY_LEAD_ACID;
	if (g_ascii_strcasecmp (type, "lip") == 0 ||
	    g_ascii_strcasecmp (type, "lipo") == 0 ||
	    g_ascii_strcasecmp (type, "li-poly") == 0)
		return UP_DEVICE_TECHNOLOGY_LITHIUM_POLYMER;
	if (g_ascii_strcasecmp (type, "nimh") == 0)
		return UP_DEVICE_TECHNOLOGY_NICKEL_METAL_HYDRIDE;
	if (g_ascii_strcasecmp (type, "life") == 0)
		return UP_DEVICE_TECHNOLOGY_LITHIUM_IRON_PHOSPHATE;
	return UP_DEVICE_TECHNOLOGY_UNKNOWN;
}

/**
 * up_device_supply_get_string:
 **/
static gchar *
up_device_supply_get_string (GUdevDevice *native, const gchar *key)
{
	gchar *value;

	/* get value, and strip to remove spaces */
	value = g_strdup (g_udev_device_get_sysfs_attr_uncached (native, key));
	if (value)
		g_strstrip (value);

	/* no value */
	if (value == NULL)
		goto out;

	/* empty value */
	if (value[0] == '\0') {
		g_free (value);
		value = NULL;
		goto out;
	}
out:
	return value;
}

/**
 * up_device_supply_get_design_voltage:
 **/
static gdouble
up_device_supply_get_design_voltage (UpDeviceSupply *device,
				     GUdevDevice *native)
{
	gdouble voltage;
	gchar *device_type = NULL;

	/* design maximum */
	voltage = g_udev_device_get_sysfs_attr_as_double_uncached (native, "voltage_max_design") / 1000000.0;
	if (voltage > 1.00f) {
		g_debug ("using max design voltage");
		goto out;
	}

	/* design minimum */
	voltage = g_udev_device_get_sysfs_attr_as_double_uncached (native, "voltage_min_design") / 1000000.0;
	if (voltage > 1.00f) {
		g_debug ("using min design voltage");
		goto out;
	}

	/* current voltage */
	voltage = g_udev_device_get_sysfs_attr_as_double_uncached (native, "voltage_present") / 1000000.0;
	if (voltage > 1.00f) {
		g_debug ("using present voltage");
		goto out;
	}

	/* current voltage, alternate form */
	voltage = g_udev_device_get_sysfs_attr_as_double_uncached (native, "voltage_now") / 1000000.0;
	if (voltage > 1.00f) {
		g_debug ("using present voltage (alternate)");
		goto out;
	}

	/* is this a USB device? */
	device_type = up_device_supply_get_string (native, "type");
	if (device_type != NULL && g_ascii_strcasecmp (device_type, "USB") == 0) {
		g_debug ("USB device, so assuming 5v");
		voltage = 5.0f;
		goto out;
	}

	/* no valid value found; display a warning the first time for each
	 * device */
	if (!device->priv->shown_invalid_voltage_warning) {
		device->priv->shown_invalid_voltage_warning = TRUE;
		g_warning ("no valid voltage value found for device %s, assuming 10V",
			   g_udev_device_get_sysfs_path (native));
	}
	/* completely guess, to avoid getting zero values */
	g_debug ("no voltage values for device %s, using 10V as approximation",
		 g_udev_device_get_sysfs_path (native));
	voltage = 10.0f;
out:
	g_free (device_type);
	return voltage;
}

static gboolean
up_device_supply_units_changed (UpDeviceSupply *supply,
				GUdevDevice    *native)
{
	if (supply->priv->coldplug_units == UP_DEVICE_SUPPLY_COLDPLUG_UNITS_CHARGE)
		if (g_udev_device_has_sysfs_attr_uncached (native, "charge_now") ||
		    g_udev_device_has_sysfs_attr_uncached (native, "charge_avg"))
			return FALSE;
	if (supply->priv->coldplug_units == UP_DEVICE_SUPPLY_COLDPLUG_UNITS_ENERGY)
		if (g_udev_device_has_sysfs_attr_uncached (native, "energy_now") ||
		    g_udev_device_has_sysfs_attr_uncached (native, "energy_avg"))
			return FALSE;
	return TRUE;
}

static UpDeviceState
up_device_supply_get_state (GUdevDevice *native)
{
	UpDeviceState state;
	gchar *status;

	status = up_device_supply_get_string (native, "status");
	if (status == NULL ||
	    g_ascii_strcasecmp (status, "unknown") == 0 ||
	    *status == '\0') {
		state = UP_DEVICE_STATE_UNKNOWN;
	} else if (g_ascii_strcasecmp (status, "charging") == 0)
		state = UP_DEVICE_STATE_CHARGING;
	else if (g_ascii_strcasecmp (status, "discharging") == 0)
		state = UP_DEVICE_STATE_DISCHARGING;
	else if (g_ascii_strcasecmp (status, "full") == 0)
		state = UP_DEVICE_STATE_FULLY_CHARGED;
	else if (g_ascii_strcasecmp (status, "empty") == 0)
		state = UP_DEVICE_STATE_EMPTY;
	else if (g_ascii_strcasecmp (status, "not charging") == 0)
		state = UP_DEVICE_STATE_PENDING_CHARGE;
	else {
		g_warning ("unknown status string: %s", status);
		state = UP_DEVICE_STATE_UNKNOWN;
	}

	g_free (status);

	return state;
}

static gdouble
sysfs_get_capacity_level (GUdevDevice   *native,
			  UpDeviceLevel *level)
{
	char *str;
	gdouble ret = -1.0;
	guint i;
	struct {
		const char *str;
		gdouble percentage;
		UpDeviceLevel level;
	} levels[] = {
		/* In order of most likely to least likely,
		 * Keep in sync with up_daemon_compute_warning_level() */
		{ "Normal",    55.0, UP_DEVICE_LEVEL_NORMAL },
		{ "High",      70.0, UP_DEVICE_LEVEL_HIGH },
		{ "Low",       10.0, UP_DEVICE_LEVEL_LOW },
		{ "Critical",   5.0, UP_DEVICE_LEVEL_CRITICAL },
		{ "Full",     100.0, UP_DEVICE_LEVEL_FULL },
		{ "Unknown",   50.0, UP_DEVICE_LEVEL_UNKNOWN }
	};
	guint len;

	g_return_val_if_fail (level != NULL, -1.0);

	if (!g_udev_device_has_sysfs_attr_uncached (native, "capacity_level")) {
		g_debug ("capacity_level doesn't exist, skipping");
		*level = UP_DEVICE_LEVEL_NONE;
		return -1.0;
	}

	*level = UP_DEVICE_LEVEL_UNKNOWN;
	str = g_strdup (g_udev_device_get_sysfs_attr_uncached (native, "capacity_level"));
	if (!str) {
		g_debug ("Failed to read capacity_level!");
		return ret;
	}

	len = strlen(str);
	str[len -1] = '\0';
	for (i = 0; i < G_N_ELEMENTS(levels); i++) {
		if (strcmp (levels[i].str, str) == 0) {
			ret = levels[i].percentage;
			*level = levels[i].level;
			break;
		}
	}

	if (ret < 0.0)
		g_debug ("Could not find a percentage for capacity level '%s'", str);

	g_free (str);
	return ret;
}

static gboolean
up_device_supply_refresh_battery (UpDeviceSupply *supply,
				  UpRefreshReason reason)
{
	gchar *technology_native = NULL;
	gdouble voltage_design;
	UpDeviceState old_state;
	UpDeviceState state;
	UpDevice *device = UP_DEVICE (supply);
	GUdevDevice *native;
	gboolean is_present;
	gdouble energy;
	gdouble energy_full;
	gdouble energy_full_design;
	gdouble energy_rate;
	gdouble capacity = 100.0f;
	gdouble percentage = 0.0f;
	gdouble voltage;
	gint64 time_to_empty;
	gint64 time_to_full;
	gdouble temp;
	int charge_cycles = -1;
	gchar *manufacturer = NULL;
	gchar *model_name = NULL;
	gchar *serial_number = NULL;
	UpDaemon *daemon;
	gboolean ac_online = FALSE;
	gboolean has_ac = FALSE;
	gboolean online;
	const gchar *ideapad_path;
	gboolean conservation_mode_on = FALSE;
	UpDeviceList *devices_list;
	GPtrArray *devices;
	guint i;

	native = G_UDEV_DEVICE (up_device_get_native (device));

	/* have we just been removed? */
	if (g_udev_device_has_sysfs_attr_uncached (native, "present")) {
		is_present = g_udev_device_get_sysfs_attr_as_boolean_uncached (native, "present");
	} else {
		/* when no present property exists, handle as present */
		is_present = TRUE;
	}
	g_object_set (device, "is-present", is_present, NULL);
	if (!is_present) {
		up_device_supply_reset_values (supply);
		goto out;
	}

	/* get the current charge */
	energy = g_udev_device_get_sysfs_attr_as_double_uncached (native, "energy_now") / 1000000.0;
	if (energy < 0.01)
		energy = g_udev_device_get_sysfs_attr_as_double_uncached (native, "energy_avg") / 1000000.0;

	/* used to convert A to W later */
	voltage_design = up_device_supply_get_design_voltage (supply, native);

	/* initial values */
	if (!supply->priv->has_coldplug_values ||
	    up_device_supply_units_changed (supply, native)) {

		/* the ACPI spec is bad at defining battery type constants */
		technology_native = up_device_supply_get_string (native, "technology");
		g_object_set (device, "technology", up_device_supply_convert_device_technology (technology_native), NULL);

		/* get values which may be blank */
		manufacturer = up_device_supply_get_string (native, "manufacturer");
		model_name = up_device_supply_get_string (native, "model_name");
		serial_number = up_device_supply_get_string (native, "serial_number");

		/* some vendors fill this with binary garbage */
		up_make_safe_string (manufacturer);
		up_make_safe_string (model_name);
		up_make_safe_string (serial_number);

		g_object_set (device,
			      "vendor", manufacturer,
			      "model", model_name,
			      "serial", serial_number,
			      "is-rechargeable", TRUE, /* assume true for laptops */
			      "has-history", TRUE,
			      "has-statistics", TRUE,
			      NULL);

		/* these don't change at runtime */
		energy_full = g_udev_device_get_sysfs_attr_as_double_uncached (native, "energy_full") / 1000000.0;
		energy_full_design = g_udev_device_get_sysfs_attr_as_double_uncached (native, "energy_full_design") / 1000000.0;

		/* convert charge to energy */
		if (energy_full < 0.01) {
			energy_full = g_udev_device_get_sysfs_attr_as_double_uncached (native, "charge_full") / 1000000.0;
			energy_full_design = g_udev_device_get_sysfs_attr_as_double_uncached (native, "charge_full_design") / 1000000.0;
			energy_full *= voltage_design;
			energy_full_design *= voltage_design;
			supply->priv->coldplug_units = UP_DEVICE_SUPPLY_COLDPLUG_UNITS_CHARGE;
		}

		/* the last full should not be bigger than the design */
		if (energy_full > energy_full_design)
			g_warning ("energy_full (%f) is greater than energy_full_design (%f)",
				     energy_full, energy_full_design);

		/* some systems don't have this */
		if (energy_full < 0.01 && energy_full_design > 0.01) {
			g_warning ("correcting energy_full (%f) using energy_full_design (%f)",
				     energy_full, energy_full_design);
			energy_full = energy_full_design;
		}

		/* calculate how broken our battery is */
		if (energy_full > 0) {
			capacity = (energy_full / energy_full_design) * 100.0f;
			if (capacity < 0)
				capacity = 0.0;
			if (capacity > 100.0)
				capacity = 100.0;
		}
		g_object_set (device, "capacity", capacity, NULL);

		/* we only coldplug once, as these values will never change */
		supply->priv->has_coldplug_values = TRUE;
	} else {
		/* get the old full */
		g_object_get (device,
			      "energy-full", &energy_full,
			      "energy-full-design", &energy_full_design,
			      NULL);
	}

	state = up_device_supply_get_state (native);

	/* this is the new value in uW */
	if (g_udev_device_has_sysfs_attr (native, "power_now")) {
		energy_rate = fabs (g_udev_device_get_sysfs_attr_as_double_uncached (native, "power_now") / 1000000.0);
	} else {
		gdouble charge_full;

		/* convert charge to energy */
		if (energy < 0.01) {
			energy = g_udev_device_get_sysfs_attr_as_double_uncached (native, "charge_now") / 1000000.0;
			if (energy < 0.01)
				energy = g_udev_device_get_sysfs_attr_as_double_uncached (native, "charge_avg") / 1000000.0;
			energy *= voltage_design;
		}

		charge_full = g_udev_device_get_sysfs_attr_as_double_uncached (native, "charge_full") / 1000000.0;
		if (charge_full < 0.01)
			charge_full = g_udev_device_get_sysfs_attr_as_double_uncached (native, "charge_full_design") / 1000000.0;

		/* If charge_full exists, then current_now is always reported in uA.
		 * In the legacy case, where energy only units exist, and power_now isn't present
		 * current_now is power in uW. */
		energy_rate = fabs (g_udev_device_get_sysfs_attr_as_double_uncached (native, "current_now") / 1000000.0);
		if (charge_full != 0)
			energy_rate *= voltage_design;
	}

	/* some batteries don't update last_full attribute */
	if (energy > energy_full) {
		g_warning ("energy %f bigger than full %f", energy, energy_full);
		energy_full = energy;
	}

	/* present voltage */
	voltage = g_udev_device_get_sysfs_attr_as_double_uncached (native, "voltage_now") / 1000000.0;
	if (voltage < 0.01)
		voltage = g_udev_device_get_sysfs_attr_as_double_uncached (native, "voltage_avg") / 1000000.0;

	/* ACPI gives out the special 'Ones' value for rate when it's unable
	 * to calculate the true rate. We should set the rate zero, and wait
	 * for the BIOS to stabilise. */
	if (energy_rate == 0xffff)
		energy_rate = 0;

	/* Ensure less than 300W, above the 240W possible with USB Power Delivery */
	if (energy_rate > 300)
		energy_rate = 0;

	/* the hardware reporting failed -- try to calculate this */
	if (energy_rate < 0.01)
		energy_rate = up_device_supply_calculate_rate (supply, energy);

	/* get a precise percentage */
        if (!supply->priv->ignore_system_percentage &&
            g_udev_device_has_sysfs_attr_uncached (native, "capacity")) {
		percentage = g_udev_device_get_sysfs_attr_as_double_uncached (native, "capacity");
		percentage = CLAMP(percentage, 0.0f, 100.0f);
                /* for devices which provide capacity, but not {energy,charge}_now */
                if (energy < 0.1f && energy_full > 0.0f)
                    energy = energy_full * percentage / 100;
        } else if (energy_full > 0.0f) {
		percentage = 100.0 * energy / energy_full;
		percentage = CLAMP(percentage, 0.0f, 100.0f);
	}

	/* Some devices report "Not charging" when the battery is full and AC
	 * power is connected. In this situation we should report fully-charged
	 * instead of pending-charge. */
	if (state == UP_DEVICE_STATE_PENDING_CHARGE && percentage == 100.0)
		state = UP_DEVICE_STATE_FULLY_CHARGED;

	/* the battery isn't charging or discharging, it's just
	 * sitting there half full doing nothing: try to guess a state */
	if (state == UP_DEVICE_STATE_UNKNOWN) {
		daemon = up_device_get_daemon (device);

		/* If we have any online AC, assume charging, otherwise
		 * discharging */
		devices_list = up_daemon_get_device_list (daemon);
		devices = up_device_list_get_array (devices_list);
		for (i=0; i < devices->len; i++) {
			if (up_device_get_online ((UpDevice *) g_ptr_array_index (devices, i), &online)) {
			       has_ac = TRUE;
				if (online) {
					ac_online = TRUE;
				}
				break;
			}
		}
		g_ptr_array_unref (devices);
		g_object_unref (devices_list);

		/* On some Lenovo laptops, this battery state might indicate the conservation
		 * mode is enabled. When the ideapad_laptop kernel module is loaded, the status
		 * of this battery feature is available on sysfs. */
		ideapad_path = "/sys/bus/platform/drivers/ideapad_acpi/VPC2004:00";
		if (sysfs_file_exists(ideapad_path, "conservation_mode"))
			conservation_mode_on = sysfs_get_bool(ideapad_path, "conservation_mode");

		if (has_ac) {
			if (ac_online) {
				if (conservation_mode_on)
					state = UP_DEVICE_STATE_PENDING_CHARGE;
				else if (percentage > UP_DEVICE_SUPPLY_CHARGED_THRESHOLD)
					state = UP_DEVICE_STATE_FULLY_CHARGED;
				else
					state = UP_DEVICE_STATE_CHARGING;
			} else {
				if (percentage < 1.0f)
					state = UP_DEVICE_STATE_EMPTY;
				else
					state = UP_DEVICE_STATE_DISCHARGING;
			}
		} else {
			/* only guess when we have only one battery */
			if (up_daemon_get_number_devices_of_type (daemon, UP_DEVICE_KIND_BATTERY)  == 1) {
				if (percentage < 1.0f)
					state = UP_DEVICE_STATE_EMPTY;
				else
					state = UP_DEVICE_STATE_DISCHARGING;
			}

			/* if we have multiple batteries and don't know their
			 * state, give up and leave it as "unknown". */
		}

		/* print what we did */
		g_debug ("guessing battery state '%s': AC present: %i, AC online: %i",
			   up_device_state_to_string (state), has_ac, ac_online);

		g_object_unref (daemon);
	}

	/* if empty, and BIOS does not know what to do */
	if (state == UP_DEVICE_STATE_UNKNOWN && energy < 0.01) {
		g_warning ("Setting %s state empty as unknown and very low",
			   g_udev_device_get_sysfs_path (native));
		state = UP_DEVICE_STATE_EMPTY;
	}

	/* some batteries give out massive rate values when nearly empty */
	if (energy < 0.1f)
		energy_rate = 0.0f;

	/* calculate a quick and dirty time remaining value */
	time_to_empty = 0;
	time_to_full = 0;
	if (energy_rate > 0) {
		if (state == UP_DEVICE_STATE_DISCHARGING)
			time_to_empty = 3600 * (energy / energy_rate);
		else if (state == UP_DEVICE_STATE_CHARGING)
			time_to_full = 3600 * ((energy_full - energy) / energy_rate);
		/* TODO: need to factor in battery charge metrics */
	}

	/* check the remaining time is under a set limit, to deal with broken
	   primary batteries rate */
	if (time_to_empty > (240 * 60 * 60)) /* ten days for discharging */
		time_to_empty = 0;
	if (time_to_full > (20 * 60 * 60)) /* 20 hours for charging */
		time_to_full = 0;

	/* get temperature */
	temp = g_udev_device_get_sysfs_attr_as_double_uncached (native, "temp") / 10.0;

	/* charge_cycles is -1 if:
	 * cycle_count is -1 (unknown)
	 * cycle_count is 0 (shouldn't be used by conforming implementations)
	 * cycle_count is absent (unsupported) */
	if (g_udev_device_has_sysfs_attr_uncached (native, "cycle_count")) {
		charge_cycles = g_udev_device_get_sysfs_attr_as_int_uncached (native, "cycle_count");
		if (charge_cycles == 0)
			charge_cycles = -1;
	}

	/* check if the energy value has changed and, if that's the case,
	 * store the new values in the buffer. */
	if (up_device_supply_push_new_energy (supply, energy))
		supply->priv->rate_old = energy_rate;

	/* we changed state */
	g_object_get (device, "state", &old_state, NULL);
	if (old_state != state) {
		for (i = 0; i < UP_DEVICE_SUPPLY_ENERGY_OLD_LENGTH; ++i) {
			supply->priv->energy_old[i] = 0.0f;
			supply->priv->energy_old_timespec[i].tv_sec = 0;

		}
		supply->priv->energy_old_first = 0;
	}

	g_object_set (device,
		      "energy", energy,
		      "energy-full", energy_full,
		      "energy-full-design", energy_full_design,
		      "energy-rate", energy_rate,
		      "percentage", percentage,
		      "state", state,
		      "voltage", voltage,
		      "time-to-empty", time_to_empty,
		      "time-to-full", time_to_full,
		      "temperature", temp,
		      "charge-cycles", charge_cycles,
		      NULL);

	/* Setup unknown poll again if needed */
	up_device_supply_update_poll_frequency (device, state, reason);

out:
	g_free (technology_native);
	g_free (manufacturer);
	g_free (model_name);
	g_free (serial_number);
	return TRUE;
}

static gboolean
up_device_supply_refresh_device (UpDeviceSupply *supply,
				 UpRefreshReason reason)
{
	UpDeviceState state;
	UpDevice *device = UP_DEVICE (supply);
	GUdevDevice *native;
	gdouble percentage = 0.0f;
	UpDeviceLevel level = UP_DEVICE_LEVEL_NONE;

	native = G_UDEV_DEVICE (up_device_get_native (device));

	/* initial values */
	if (!supply->priv->has_coldplug_values) {
		gchar *model_name;
		gchar *serial_number;

		/* get values which may be blank */
		model_name = up_device_supply_get_string (native, "model_name");
		serial_number = up_device_supply_get_string (native, "serial_number");

		/* some vendors fill this with binary garbage */
		up_make_safe_string (model_name);
		up_make_safe_string (serial_number);

		g_object_set (device,
			      "is-present", TRUE,
			      "model", model_name,
			      "serial", serial_number,
			      "is-rechargeable", TRUE,
			      "has-history", TRUE,
			      "has-statistics", TRUE,
			      NULL);

		/* we only coldplug once, as these values will never change */
		supply->priv->has_coldplug_values = TRUE;

		g_free (model_name);
		g_free (serial_number);
	}

	/* get a precise percentage */
	percentage = g_udev_device_get_sysfs_attr_as_double_uncached (native, "capacity");
	if (percentage == 0.0f)
		percentage = sysfs_get_capacity_level (native, &level);

	if (percentage < 0.0) {
		/* Probably talking to the device over Bluetooth */
		state = UP_DEVICE_STATE_UNKNOWN;
		g_object_set (device, "state", state, NULL);
		return FALSE;
	}

	state = up_device_supply_get_state (native);

	/* Override whatever the device might have told us
	 * because a number of them are always discharging */
	if (percentage == 100.0)
		state = UP_DEVICE_STATE_FULLY_CHARGED;

	g_object_set (device,
		      "percentage", percentage,
		      "battery-level", level,
		      "state", state,
		      NULL);

	return TRUE;
}

static void
up_device_supply_sibling_discovered (UpDevice *device,
				     GObject  *sibling)
{
	GUdevDevice *input;
	g_autofree char *device_type = NULL;
	UpDeviceKind cur_type, new_type;
	char *model_name;
	char *serial_number;
	int i;
	struct {
		const char *prop;
		UpDeviceKind type;
	} types[] = {
		/* In order of type priority, we never downgrade here (loop aborts). */
		{ "ID_INPUT_TABLET", UP_DEVICE_KIND_TABLET },
		{ "ID_INPUT_TABLET_PAD", UP_DEVICE_KIND_TABLET },
		{ "ID_INPUT_KEYBOARD", UP_DEVICE_KIND_KEYBOARD },
		{ "ID_INPUT_TOUCHPAD", UP_DEVICE_KIND_TOUCHPAD },
		{ "ID_INPUT_MOUSE", UP_DEVICE_KIND_MOUSE },
		{ "ID_INPUT_JOYSTICK", UP_DEVICE_KIND_GAMING_INPUT },
	};

	if (!G_UDEV_IS_DEVICE (sibling))
		return;

	input = G_UDEV_DEVICE (sibling);

	/* Do not process if we already have a "good" guess for the device type. */
	g_object_get (device, "type", &cur_type, NULL);
	if (cur_type == UP_DEVICE_KIND_LINE_POWER)
		return;

	if (g_strcmp0 (g_udev_device_get_subsystem (input), "input") != 0)
		return;

	g_object_get (device,
		      "model", &model_name,
		      "serial", &serial_number,
		      NULL);

	if (model_name == NULL && serial_number == NULL) {
		model_name = up_device_supply_get_string (input, "name");
		serial_number = up_device_supply_get_string (input, "uniq");

		up_make_safe_string (model_name);
		up_make_safe_string (serial_number);

		g_object_set (device,
			      "model", model_name,
			      "serial", serial_number,
			      NULL);

		g_free (model_name);
		g_free (serial_number);
	}

	/* Fall back to "keyboard" if we don't find anything. */
	new_type = UP_DEVICE_KIND_KEYBOARD;

	for (i = 0; i < G_N_ELEMENTS (types); i++) {
		if (types[i].type == cur_type ||
		    g_udev_device_get_property_as_boolean (input, types[i].prop)) {
			new_type = types[i].type;
			break;
		}
	}

	if (cur_type != new_type)
		g_object_set (device, "type", new_type, NULL);
}

static UpDeviceKind
up_device_supply_guess_type (GUdevDevice *native,
			     const char *native_path)
{
	gchar *device_type;
	UpDeviceKind type = UP_DEVICE_KIND_UNKNOWN;

	device_type = up_device_supply_get_string (native, "type");
	if (device_type == NULL)
		return type;

	if (g_ascii_strcasecmp (device_type, "mains") == 0) {
		type = UP_DEVICE_KIND_LINE_POWER;
		goto out;
	}

	if (g_ascii_strcasecmp (device_type, "battery") == 0) {
		type = UP_DEVICE_KIND_BATTERY;

	} else if (g_ascii_strcasecmp (device_type, "USB") == 0) {

		/* USB supplies should have a usb_type attribute which we would
		 * ideally decode further.
		 *
		 * For historic reasons, we have a heuristic for wacom tablets
		 * that can be dropped in the future.
		 * As of May 2022, it is expected to be fixed in kernel 5.19.
		 * https://patchwork.kernel.org/project/linux-input/patch/20220407115406.115112-1-hadess@hadess.net/
		 */
		if (g_udev_device_has_sysfs_attr (native, "usb_type") &&
		    g_udev_device_has_sysfs_attr (native, "online"))
			type = UP_DEVICE_KIND_LINE_POWER;
		else if (g_strstr_len (native_path, -1, "wacom_") != NULL)
			type = UP_DEVICE_KIND_TABLET;
		else
			g_warning ("USB power supply %s without usb_type property, please report",
				   native_path);
	} else {
		g_warning ("did not recognise type %s, please report", device_type);
	}

out:
	g_free (device_type);
	return type;
}

/**
 * up_device_supply_coldplug:
 *
 * Return %TRUE on success, %FALSE if we failed to get data and should be removed
 **/
static gboolean
up_device_supply_coldplug (UpDevice *device)
{
	UpDeviceSupply *supply = UP_DEVICE_SUPPLY (device);
	GUdevDevice *native;
	const gchar *native_path;
	const gchar *scope;
	UpDeviceKind type;
	gboolean is_power_supply;

	up_device_supply_reset_values (supply);

	/* detect what kind of device we are */
	native = G_UDEV_DEVICE (up_device_get_native (device));
	native_path = g_udev_device_get_sysfs_path (native);
	if (native_path == NULL) {
		g_warning ("could not get native path for %p", device);
		return FALSE;
	}

	/* try to work out if the device is powering the system */
	scope = g_udev_device_get_sysfs_attr (native, "scope");
	if (scope != NULL && g_ascii_strcasecmp (scope, "device") == 0) {
		is_power_supply = FALSE;
	} else if (scope != NULL && g_ascii_strcasecmp (scope, "system") == 0) {
		is_power_supply = TRUE;
	} else {
		g_debug ("taking a guess for power supply scope");
		is_power_supply = TRUE;
	}

	/* we don't use separate ACs for devices */
	if (is_power_supply == FALSE &&
	    !g_udev_device_has_sysfs_attr_uncached (native, "capacity") &&
	    !g_udev_device_has_sysfs_attr_uncached (native, "capacity_level")) {
		g_debug ("Ignoring device AC, we'll monitor the device battery");
		return FALSE;
	}

	/* try to detect using the device type */
	type = up_device_supply_guess_type (native, native_path);

	/* if reading the device type did not work, use the previous method */
	if (type == UP_DEVICE_KIND_UNKNOWN) {
		if (g_udev_device_has_sysfs_attr_uncached (native, "online")) {
			type = UP_DEVICE_KIND_LINE_POWER;
		} else {
			/* this is a good guess as UPS and CSR are not in the kernel */
			type = UP_DEVICE_KIND_BATTERY;
		}
	}

	/* set the value */
	g_object_set (device,
		     "type", type,
		     "power-supply", is_power_supply,
		     NULL);

	if (type != UP_DEVICE_KIND_LINE_POWER &&
	    type != UP_DEVICE_KIND_BATTERY)
		g_object_set (device, "poll-timeout", UP_DAEMON_SHORT_TIMEOUT, NULL);
	else if (type == UP_DEVICE_KIND_BATTERY &&
		 (!supply->priv->disable_battery_poll || !is_power_supply))
		g_object_set (device, "poll-timeout", UP_DAEMON_SHORT_TIMEOUT, NULL);

	return TRUE;
}

static void
up_device_supply_update_poll_frequency (UpDevice        *device,
					UpDeviceState    state,
					UpRefreshReason  reason)
{
	UpDeviceSupply *supply = UP_DEVICE_SUPPLY (device);

	if (supply->priv->disable_battery_poll)
		return;

	/* We start fast-polling if the reason to update was not a normal POLL
	 * and one of the following holds true:
	 *  1. The current stat is unknown; we hope that this is transient
	 *     and re-poll.
	 *  2. A change occured on a line power supply. This likely means that
	 *     batteries switch between charging/discharging which does not
	 *     always result in a separate uevent.
	 *
	 * For simplicity, we do the fast polling for a specific period of time.
	 * If the reason to do fast-polling was an unknown state, then it would
	 * also be reasonable to stop as soon as we got a proper state.
	 */
	if (reason != UP_REFRESH_POLL &&
	    (state == UP_DEVICE_STATE_UNKNOWN ||
	     reason == UP_REFRESH_LINE_POWER)) {
		g_debug ("unknown_poll: setting up fast re-poll");
		g_object_set (device, "poll-timeout", UP_DAEMON_UNKNOWN_TIMEOUT, NULL);
		supply->priv->fast_repoll_until = g_get_monotonic_time () + UP_DAEMON_UNKNOWN_POLL_TIME * G_USEC_PER_SEC;

	} else if (supply->priv->fast_repoll_until == 0) {
		/* Not fast-repolling, no need to check whether to stop */

	} else if (supply->priv->fast_repoll_until < g_get_monotonic_time ()) {
		g_debug ("unknown_poll: stopping fast repoll (giving up)");
		supply->priv->fast_repoll_until = 0;
		g_object_set (device, "poll-timeout", UP_DAEMON_SHORT_TIMEOUT, NULL);
	}
}

static gboolean
up_device_supply_refresh (UpDevice *device, UpRefreshReason reason)
{
	gboolean updated;
	UpDeviceSupply *supply = UP_DEVICE_SUPPLY (device);
	UpDeviceKind type;
	gboolean is_power_supply = FALSE;

	g_object_get (device,
		      "type", &type,
		      "power-supply", &is_power_supply,
		      NULL);
	if (type == UP_DEVICE_KIND_LINE_POWER) {
		updated = up_device_supply_refresh_line_power (supply, reason);
	} else if (type == UP_DEVICE_KIND_BATTERY && is_power_supply) {
		updated = up_device_supply_refresh_battery (supply, reason);
	} else {
		updated = up_device_supply_refresh_device (supply, reason);
	}

	/* reset time if we got new data */
	if (updated)
		g_object_set (device, "update-time", (guint64) g_get_real_time () / G_USEC_PER_SEC, NULL);

	return updated;
}

/**
 * up_device_supply_init:
 **/
static void
up_device_supply_init (UpDeviceSupply *supply)
{
	UpConfig *config;

	supply->priv = up_device_supply_get_instance_private (supply);

	/* allocate the stats for the battery charging & discharging */
	supply->priv->energy_old = g_new (gdouble, UP_DEVICE_SUPPLY_ENERGY_OLD_LENGTH);
	supply->priv->energy_old_timespec = g_new (GTimeVal, UP_DEVICE_SUPPLY_ENERGY_OLD_LENGTH);

	supply->priv->shown_invalid_voltage_warning = FALSE;

	config = up_config_new ();
	/* Seems that we don't get change uevents from the
	 * kernel on some BIOS types, but if polling
	 * is disabled in the configuration, do nothing */
	supply->priv->disable_battery_poll = up_config_get_boolean (config, "NoPollBatteries");
	g_object_unref (config);
}

/**
 * up_device_supply_finalize:
 **/
static void
up_device_supply_finalize (GObject *object)
{
	UpDeviceSupply *supply;

	g_return_if_fail (object != NULL);
	g_return_if_fail (UP_IS_DEVICE_SUPPLY (object));

	supply = UP_DEVICE_SUPPLY (object);
	g_return_if_fail (supply->priv != NULL);

	g_free (supply->priv->energy_old);
	g_free (supply->priv->energy_old_timespec);

	G_OBJECT_CLASS (up_device_supply_parent_class)->finalize (object);
}

static void
up_device_supply_set_property (GObject        *object,
			       guint           property_id,
			       const GValue   *value,
			       GParamSpec     *pspec)
{
	UpDeviceSupply *supply = UP_DEVICE_SUPPLY (object);

	switch (property_id) {
	case PROP_IGNORE_SYSTEM_PERCENTAGE:
		supply->priv->ignore_system_percentage = g_value_get_boolean (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
up_device_supply_get_property (GObject        *object,
			       guint           property_id,
			       GValue         *value,
			       GParamSpec     *pspec)
{
	UpDeviceSupply *supply = UP_DEVICE_SUPPLY (object);

	switch (property_id) {
	case PROP_IGNORE_SYSTEM_PERCENTAGE:
		g_value_set_flags (value, supply->priv->ignore_system_percentage);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

/**
 * up_device_supply_class_init:
 **/
static void
up_device_supply_class_init (UpDeviceSupplyClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	UpDeviceClass *device_class = UP_DEVICE_CLASS (klass);

	object_class->finalize = up_device_supply_finalize;
	object_class->set_property = up_device_supply_set_property;
	object_class->get_property = up_device_supply_get_property;
	device_class->get_on_battery = up_device_supply_get_on_battery;
	device_class->get_online = up_device_supply_get_online;
	device_class->coldplug = up_device_supply_coldplug;
	device_class->sibling_discovered = up_device_supply_sibling_discovered;
	device_class->refresh = up_device_supply_refresh;

	g_object_class_install_property (object_class, PROP_IGNORE_SYSTEM_PERCENTAGE,
					 g_param_spec_boolean ("ignore-system-percentage",
							       "Ignore system percentage",
							       "Ignore system provided battery percentage",
							       FALSE, G_PARAM_READWRITE));
}
