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

#include "up-constants.h"
#include "up-config.h"
#include "up-device-battery.h"

/* Chosen to be quite big, in case there was a lot of re-polling */
#define MAX_ESTIMATION_POINTS 15

typedef struct {
	UpBatteryValues hw_data[MAX_ESTIMATION_POINTS];
	gint hw_data_last;
	gint hw_data_len;

	gboolean present;
	gboolean units_changed_warning;

	/* static values (only changed if plugged/unplugged) */
	gboolean disable_battery_poll;
	gdouble voltage_design;
	UpBatteryUnits units;

	/* mostly static values */
	gdouble energy_full;
	gdouble energy_full_reported;
	gdouble energy_design;
	gint charge_cycles;

	gboolean trust_power_measurement;
	gint64 last_power_discontinuity;

	/* dynamic values */
	gint64 fast_repoll_until;
	gboolean repoll_needed;

	/* state path */
	const char* state_dir;
} UpDeviceBatteryPrivate;

G_DEFINE_TYPE_EXTENDED (UpDeviceBattery, up_device_battery, UP_TYPE_DEVICE, 0,
                        G_ADD_PRIVATE (UpDeviceBattery))

static gboolean
up_device_battery_get_on_battery (UpDevice *device, gboolean *on_battery)
{
	UpDeviceState state;

	g_return_val_if_fail (on_battery != NULL, FALSE);

	g_object_get (device,
		      "state", &state,
		      NULL);

	*on_battery = (state == UP_DEVICE_STATE_DISCHARGING);

	return TRUE;
}

static gdouble
up_device_battery_charge_to_energy (UpDeviceBattery *self, gdouble charge)
{
	UpDeviceBatteryPrivate *priv = up_device_battery_get_instance_private (self);

	/* We want to work with energy internally.
	 * Note that this is a pretty bad way of estimating the energy,
	 * we just assume that the voltage is always the same, which is
	 * obviously not true. The voltage depends on at least:
	 *  - output current
	 *  - temperature
	 *  - charge
	 * The easiest way to improve this would likely be "machine learning",
	 * i.e. statistics through which we can calculate the actual
	 * performance based on the factors we have.
	 */
	return priv->voltage_design * charge;
}

static void
up_device_battery_estimate_power (UpDeviceBattery *self, UpBatteryValues *cur)
{
	UpDeviceBatteryPrivate *priv = up_device_battery_get_instance_private (self);
	UpDeviceState reported_state;
	UpBatteryValues *ref = NULL;
	gdouble energy_rate = 0.0;
	gint64 ref_td = 999 * G_USEC_PER_SEC; /* We need to be able to do math with this */
	gint i;

	/* Same item, but it is copied in already. */
	g_assert (cur->ts_us != priv->hw_data[priv->hw_data_last].ts_us);
	reported_state = cur->state;

	if (cur->state != UP_DEVICE_STATE_CHARGING &&
	    cur->state != UP_DEVICE_STATE_DISCHARGING &&
	    cur->state != UP_DEVICE_STATE_UNKNOWN)
		return;

	for (i = 0; i < priv->hw_data_len; i++) {
		int pos = (priv->hw_data_last - i + G_N_ELEMENTS (priv->hw_data)) % G_N_ELEMENTS (priv->hw_data);
		gint64 td;

		/* Stop searching if the hardware state changed. */
		if (priv->hw_data[pos].state != reported_state)
			break;

		td = cur->ts_us - priv->hw_data[pos].ts_us;
		/* At least 15 seconds worth of data. */
		if (td < 15 * G_USEC_PER_SEC)
			continue;

		/* Stop searching if the new reference is further away from the long timeout. */
		if (ABS(UP_DAEMON_LONG_TIMEOUT * G_USEC_PER_SEC - ABS (td)) > ABS(UP_DAEMON_SHORT_TIMEOUT * G_USEC_PER_SEC - ref_td))
			break;

		ref_td = td;
		ref = &priv->hw_data[pos];
	}

	/* We rely solely on battery reports here, with dynamic power
	 * usage (in particular during resume), lets just wait for a
	 * bit longer before reporting anything to the user.
	 *
	 * Alternatively, we could assume that some old estimate for the
	 * energy rate remains stable and do a time estimate based on that.
	 *
	 * For now, this is better than what we used to do.
	 */
	if (!ref) {
		priv->repoll_needed = TRUE;
		return;
	}

	/* energy is in Wh, rate in W */
	energy_rate = (cur->energy.cur - ref->energy.cur) / (ref_td / ((gdouble) 3600 * G_USEC_PER_SEC));

	/* Try to guess charge/discharge state based on rate.
	 * Note that the history is discarded when the AC is plugged, as such
	 * we should only err on the side of showing CHARGING for too long.
	 */
	if (cur->state == UP_DEVICE_STATE_UNKNOWN) {
		/* Consider a rate of 0.5W as "no change", otherwise set CHARGING/DISCHARGING */
		if (ABS(energy_rate) < 0.5)
			return;
		else if (energy_rate < 0.0)
			cur->state = UP_DEVICE_STATE_DISCHARGING;
		else
			cur->state = UP_DEVICE_STATE_CHARGING;
	}

	/* QUIRK: No good reason, but define rate to be positive. */
	if (cur->state == UP_DEVICE_STATE_DISCHARGING)
		energy_rate *= -1.0;

	/* This hopefully gives us sane values, but lets print a message if not. */
	if (energy_rate < 0.1 || energy_rate > 300) {
		g_debug ("The estimated %scharge rate is %fW, which is not realistic",
			   cur->state == UP_DEVICE_STATE_DISCHARGING ? "dis" : "",
			   energy_rate);
		energy_rate = 0;
	}

	cur->energy.rate = energy_rate;
}

static void
up_device_battery_update_poll_frequency (UpDeviceBattery *self,
					 UpDeviceState    state,
					 UpRefreshReason  reason)
{
	UpDeviceBatteryPrivate *priv = up_device_battery_get_instance_private (self);
	gint slow_poll_timeout;

	if (priv->disable_battery_poll)
		return;

	slow_poll_timeout = priv->repoll_needed ? UP_DAEMON_ESTIMATE_TIMEOUT : UP_DAEMON_SHORT_TIMEOUT;
	priv->repoll_needed = FALSE;

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
		g_object_set (self, "poll-timeout", UP_DAEMON_UNKNOWN_TIMEOUT, NULL);
		priv->fast_repoll_until = g_get_monotonic_time () + UP_DAEMON_UNKNOWN_POLL_TIME * G_USEC_PER_SEC;

	} else if (priv->fast_repoll_until == 0) {
		/* Not fast-repolling, check poll timeout is as expected */
		gint poll_timeout;
		g_object_get (self, "poll-timeout", &poll_timeout, NULL);
		if (poll_timeout != slow_poll_timeout)
			g_object_set (self, "poll-timeout", slow_poll_timeout, NULL);

	} else if (priv->fast_repoll_until < g_get_monotonic_time ()) {
		g_debug ("unknown_poll: stopping fast repoll (giving up)");
		priv->fast_repoll_until = 0;
		g_object_set (self, "poll-timeout", slow_poll_timeout, NULL);
	}
}

void
up_device_battery_report (UpDeviceBattery *self,
			  UpBatteryValues *values,
			  UpRefreshReason  reason)
{
	UpDeviceBatteryPrivate *priv = up_device_battery_get_instance_private (self);
	gint64 time_to_empty = 0;
	gint64 time_to_full = 0;

	if (!priv->present) {
		g_warning ("Got a battery report for a battery that is not present");
		return;
	}

	g_assert (priv->units != UP_BATTERY_UNIT_UNDEFINED);

	values->ts_us = g_get_monotonic_time ();

	/* Discard all old measurements that can't be used for estimations.
	 *
	 * XXX: Should a state change also trigger an update of the timestamp
	 *      that is used to discard power/current measurements?
	 */
	if (reason == UP_REFRESH_RESUME || reason == UP_REFRESH_LINE_POWER) {
		priv->hw_data_len = 0;
		priv->last_power_discontinuity = values->ts_us;
	}

	/* QUIRK:
	 *
	 * There is an old bug where some Lenovo machine switched from reporting
	 * energy to reporting charge numbers. The code used to react by 
	 * reloading everything, however, what apparently happens is that the
	 * *energy* value simply starts being reported through *charge*
	 * attributes.
	 * The original report is
	 *    https://bugzilla.redhat.com/show_bug.cgi?id=587112
	 * and inspecting the numbers it is clear that the values are
	 * really energy values that are unrealistically high as they get
	 * incorrectly multiplied by the voltage.
	 *
	 * Said differently, just assuming the units did *not* change should
	 * give us a saner value. Obviously, things will fall appart if upower
	 * is restarted and this should be fixed in the kernel or firmware.
	 *
	 * Unfortunately, the hardware is quite old (X201s) which makes it hard
	 * to even confirm that the bug was not fixed in the kernel or firmware.
	 *
	 * Note that a race condition could be the user swapping the battery
	 * during suspend and us re-polling energy data before noticing that
	 * the battery has changed.
	 */
	if (G_UNLIKELY (priv->units != values->units)) {
		if (!priv->units_changed_warning) {
			g_warning ("Battery unit type changed, assuming the old unit is still valid. This is likely a firmware or driver issue, please report!");
			priv->units_changed_warning = TRUE;
		}
		values->units = priv->units;
	}

	if (values->units == UP_BATTERY_UNIT_CHARGE) {
		values->units = UP_BATTERY_UNIT_ENERGY;
		values->energy.cur = up_device_battery_charge_to_energy (self, values->charge.cur);
		values->energy.rate = up_device_battery_charge_to_energy (self, values->charge.rate);
	}

	/* QUIRK: Discard weird measurements (like a 300W power usage). */
	if (values->energy.rate > 300)
		values->energy.rate = 0;

	/* Infer current energy if unknown */
	if (values->energy.cur < 0.01 && values->percentage > 0)
		values->energy.cur = priv->energy_full * values->percentage / 100.0;

	/* QUIRK: Increase energy_full if energy.cur is higher */
	if (values->energy.cur > priv->energy_full) {
		priv->energy_full = values->energy.cur;
		g_object_set (self,
		              /* How healthy the battery is (clamp to 100% if it can hold more charge than expected) */
		              "capacity", MIN (priv->energy_full / priv->energy_design * 100.0, 100),
		              "energy-full", priv->energy_full,
		              NULL);
	}

	/* Infer percentage if unknown */
	if (values->percentage <= 0)
		values->percentage = values->energy.cur / priv->energy_full * 100;

	/* NOTE: We used to do more for the UNKNOWN state. However, some of the
	 * logic relies on only one battery device to be present. Plus, it
	 * requires knowing the AC state.
	 * Because of this complexity, the decision was made to only do this
	 * type of inferring inside the DisplayDevice. There we can be sure
	 * about the AC state and we only have "one" battery.
	 */

	/* QUIRK: No good reason, but define rate to be positive.
	 *
	 * It would be sane/reasonable to define it to be negative when
	 * discharging. Only odd thing is that most common hardware appears
	 * to always report positive values, which appeared in DBus unmodified.
	 */
	if (values->state == UP_DEVICE_STATE_DISCHARGING && values->energy.rate < 0)
		values->energy.rate = -values->energy.rate;

	/* NOTE: We got a (likely sane) reading.
	 * Assume power/current readings are accurate from now on. */
	if (values->energy.rate > 0.01)
		priv->trust_power_measurement = TRUE;

	if (priv->trust_power_measurement) {
		/* QUIRK: Do not trust readings after a discontinuity happened */
		if (priv->last_power_discontinuity + UP_DAEMON_DISTRUST_RATE_TIMEOUT * G_USEC_PER_SEC > values->ts_us)
			values->energy.rate = 0.0;
	} else {
		up_device_battery_estimate_power (self, values);
	}


	/* Push into our ring buffer */
	priv->hw_data_last = (priv->hw_data_last + 1) % G_N_ELEMENTS (priv->hw_data);
	priv->hw_data_len = MIN (priv->hw_data_len + 1, G_N_ELEMENTS (priv->hw_data));
	priv->hw_data[priv->hw_data_last] = *values;

	if (values->energy.rate > 0.01) {
		/* Calculate time to full/empty
		 *
		 * Here we could factor in collected data about charge rates
		 * FIXME: Use charge-stop-threshold here
		 */
		if (values->state == UP_DEVICE_STATE_CHARGING)
			time_to_full = 3600 * (priv->energy_full - values->energy.cur) / values->energy.rate;
		else
			time_to_empty = 3600 * values->energy.cur / values->energy.rate;
	} else {
		if (values->state == UP_DEVICE_STATE_CHARGING || values->state == UP_DEVICE_STATE_DISCHARGING)
			priv->repoll_needed = TRUE;
	}

	/* QUIRK: Do a FULL/EMPTY guess if the state is still unknown
	 *        Maybe limit to when we have good estimates
	 *        (would require rate/current information) */
	if (values->state == UP_DEVICE_STATE_UNKNOWN) {
		if (values->percentage >= UP_FULLY_CHARGED_THRESHOLD)
			values->state = UP_DEVICE_STATE_FULLY_CHARGED;
		else if (values->percentage < 1.0)
			values->state = UP_DEVICE_STATE_EMPTY;
	}

	/* QUIRK: Some devices keep reporting PENDING_CHARGE even when full */
	if (values->state == UP_DEVICE_STATE_PENDING_CHARGE && values->percentage >= UP_FULLY_CHARGED_THRESHOLD)
		values->state = UP_DEVICE_STATE_FULLY_CHARGED;

	/* Set the main properties (setting "update-time" last) */
	g_object_set (self,
		      "energy", values->energy.cur,
		      "percentage", values->percentage,
		      "state", values->state,
		      "voltage", values->voltage,
		      "temperature", values->temperature,
		      "energy-rate", values->energy.rate,
		      "time-to-empty", time_to_empty,
		      "time-to-full", time_to_full,
		      /* XXX: Move "update-time" updates elsewhere? */
		      "update-time", (guint64) g_get_real_time () / G_USEC_PER_SEC,
		      NULL);

	up_device_battery_update_poll_frequency (self, values->state, reason);
}

static gboolean
get_battery_charge_threshold_state(UpDeviceBattery *self, gchar **battery_name) {
	g_autofree gchar *filename;
	g_autofree gchar *data = NULL;
	gboolean ret;
	gboolean enabled = FALSE;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state_filename = NULL;
	UpDeviceBatteryPrivate *priv = up_device_battery_get_instance_private (self);

	state_filename = g_strdup_printf("%s-threshold-enabled", *battery_name);
	filename = g_build_filename (priv->state_dir, state_filename, NULL);
	ret = g_file_get_contents (filename, &data, NULL, &error);
	if (!ret) {
                enabled = FALSE;
		g_debug ("failed to read battery charge threshold: %s", error->message);
	} else if (g_strcmp0(data, "1") == 0) {
		enabled = TRUE;
	}

	return enabled;

}

void
up_device_battery_update_info (UpDeviceBattery *self, UpBatteryInfo *info)
{
	gboolean charge_threshold_enabled = FALSE;
	UpDeviceBatteryPrivate *priv = up_device_battery_get_instance_private (self);

	/* First, sanitize the information. */
	if (info->present && info->units == UP_BATTERY_UNIT_UNDEFINED) {
		g_warning ("Battery without defined units, assuming unplugged");
		info->present = FALSE;
	}


	/* Still not present, ignore. */
	if (!info->present && !priv->present)
		return;

	/* Emulate an unplug if present but vendor, etc. changed. */
	if (info->present && info->present == priv->present) {
		g_autofree gchar *vendor = NULL;
		g_autofree gchar *model = NULL;
		g_autofree gchar *serial = NULL;

		g_object_get (self,
		              "vendor", &vendor,
		              "model", &model,
		              "serial", &serial,
		              NULL);
		if (g_strcmp0 (vendor, info->vendor) != 0 ||
		    g_strcmp0 (model, info->model) != 0 ||
		    g_strcmp0 (serial, info->serial) != 0) {
			UpBatteryInfo unplugged = { .present = FALSE };
			up_device_battery_update_info (self, &unplugged);
		}
	}

	if (info->present) {
		gdouble energy_full;
		gdouble energy_design;
		gint charge_cycles;

		/* See above, we have a (new) battery plugged in. */
		if (!priv->present) {
	                g_autoptr(GError) error = NULL;
			g_autofree gchar *battery_name = g_strdup_printf("%s-%s", info->model, info->serial);
			charge_threshold_enabled = get_battery_charge_threshold_state(self, &battery_name);
			// TODO: this the correct place?
			UpDeviceClass *klass = UP_DEVICE_GET_CLASS (&self->parent_instance);
			if (!klass->set_battery_charge_thresholds (&self->parent_instance,
								   info->charge_control_start_threshold,
								   info->charge_control_end_threshold,
								   &error)) {
				charge_threshold_enabled = FALSE;
		                g_error ("Cannot set charge thresholds: %s", g_strerror(errno));
                        }

			g_object_set (self,
			              "is-present", TRUE,
			              "vendor", info->vendor,
			              "model", info->model,
			              "serial", info->serial,
			              "technology", info->technology,
			              "has-history", TRUE,
			              "has-statistics", TRUE,
				      "charge-start-threshold", info->charge_control_start_threshold,
				      "charge-end-threshold", info->charge_control_end_threshold,
				      "charge-threshold-enabled", charge_threshold_enabled,
				      "charge-threshold-supported", info->charge_control_supported,
			              NULL);

			priv->present = TRUE;
			priv->units = info->units;
		}

		/* See comment in up_device_battery_report */
		if (priv->units != info->units && !priv->units_changed_warning) {
			g_warning ("Battery unit type changed, assuming the old unit is still valid. This is likely a firmware or driver issue, please report!");
			priv->units_changed_warning = TRUE;
		}

		priv->voltage_design = info->voltage_design;
		if (priv->units == UP_BATTERY_UNIT_CHARGE) {
			energy_full = up_device_battery_charge_to_energy (self, info->charge.full);
			energy_design = up_device_battery_charge_to_energy (self, info->charge.design);
		} else {
			energy_full = info->energy.full;
			energy_design = info->energy.design;
		}

		if (energy_full < 0.01)
			energy_full = energy_design;

		/* Force -1 for unknown value (where 0 is also an unknown value) */
		charge_cycles = info->charge_cycles > 0 ? info->charge_cycles : -1;

		if (energy_full != priv->energy_full_reported || energy_design != priv->energy_design) {
			priv->energy_full = energy_full;
			priv->energy_full_reported = energy_full;
			priv->energy_design = energy_design;

			g_object_set (self,
			              /* How healthy the battery is (clamp to 100% if it can hold more charge than expected) */
			              "capacity", MIN (priv->energy_full / priv->energy_design * 100.0, 100),
			              "energy-full", priv->energy_full,
			              "energy-full-design", priv->energy_design,
			              NULL);
		}

		if (priv->charge_cycles != charge_cycles) {
			priv->charge_cycles = charge_cycles;
			g_object_set (self,
			              "charge-cycles", charge_cycles,
			              NULL);
		}

		/* NOTE: Assume a normal refresh will follow immediately (do not update timestamp). */
	} else {
		priv->present = FALSE;
		priv->trust_power_measurement = FALSE;
		priv->hw_data_len = 0;
		priv->units = UP_BATTERY_UNIT_UNDEFINED;

		g_object_set (self,
		              "is-present", FALSE,
		              "vendor", NULL,
		              "model", NULL,
		              "serial", NULL,
		              "technology", UP_DEVICE_TECHNOLOGY_UNKNOWN,
		              "capacity", (gdouble) 0.0,
		              "energy-full", (gdouble) 0.0,
		              "energy-full-design", (gdouble) 0.0,
		              "charge-cycles", -1,
		              "has-history", FALSE,
		              "has-statistics", FALSE,
		              "update-time", (guint64) g_get_real_time () / G_USEC_PER_SEC,
			      "charge-start-threshold", 0,
			      "charge-end-threshold", 100,
			      "charge-threshold-enabled", FALSE,
			      "charge-threshold-supported", FALSE,
		              NULL);
	}
}

static gboolean
up_device_battery_set_charge_thresholds(UpDeviceBattery *self, gdouble start, gdouble end, GError **error) {
	UpDeviceClass *klass = UP_DEVICE_GET_CLASS (&self->parent_instance);

	/* not implemented */
	if (klass->set_battery_charge_thresholds == NULL)
		return FALSE;

	return klass->set_battery_charge_thresholds(&self->parent_instance, start, end, error);
}

static gboolean
up_device_battery_charge_threshold_state_write(UpDeviceBattery *self, gboolean enabled, gchar **state_file) {
	gboolean ret;
	g_autofree gchar *filename = NULL;
	g_autoptr(GError) error = NULL;

	UpDeviceBatteryPrivate *priv = up_device_battery_get_instance_private (self);

	filename = g_build_filename (priv->state_dir, state_file, NULL);
	ret = g_file_set_contents (filename, enabled ? "1": "0" , -1, &error);
	if (!ret) {
		g_error ("failed to save battery charge threshold: %s", error->message);
		return FALSE;
	}

	return TRUE;
}

/**
 * up_device_battery_set_charge_limit:
 **/
static gboolean
up_device_battery_set_charge_limit (UpExportedDevice *skeleton,
				    GDBusMethodInvocation *invocation,
				    gboolean enabled,
				    UpDeviceBattery *self)
{
	gboolean ret = FALSE;
	gboolean charge_threshold_enabled;
	gboolean charge_threshold_supported;
	guint charge_start_threshold = 0;
	guint charge_end_threshold = 100;
        g_autofree gchar *model = NULL;
        g_autofree gchar *serial = NULL;
        g_autofree gchar *state_file = NULL;
	g_autoptr(GError) error = NULL;

	g_object_get (self,
		      "charge-threshold-enabled", &charge_threshold_enabled,
		      "charge-threshold-supported", &charge_threshold_supported,
		      "charge-start-threshold", &charge_start_threshold,
		      "charge-end-threshold", &charge_end_threshold,
                      "model", &model,
                      "serial", &serial,
		      NULL);

	if (!charge_threshold_supported) {
		g_dbus_method_invocation_return_error (invocation,
						       UP_DAEMON_ERROR, UP_DAEMON_ERROR_GENERAL,
						       "setting battery charge thresholds is unsupported");
		return TRUE;
	}

        state_file = g_strdup_printf("%s-%s-threshold-enabled", model, serial);
	ret = up_device_battery_charge_threshold_state_write (self, enabled, &state_file);
	if (!ret) {
		g_dbus_method_invocation_return_error (invocation,
						       UP_DAEMON_ERROR, UP_DAEMON_ERROR_GENERAL,
						       "writing charge limits state file '%s' failed", state_file);
		return TRUE;
	}


	if (enabled) {
		ret = up_device_battery_set_charge_thresholds (self, charge_start_threshold, charge_end_threshold, &error);
                if (!ret) {
			g_dbus_method_invocation_return_gerror (invocation, error);
			return TRUE;
                }
	} else {
		ret = up_device_battery_set_charge_thresholds (self, 0, 100, &error);
                if (!ret) {
			g_dbus_method_invocation_return_gerror (invocation, error);
			return TRUE;
                }
	}

	g_object_set(self,
		     "charge-threshold-enabled", enabled,
		     NULL);

	up_exported_device_complete_enable_charge_limit (skeleton, invocation);

	return TRUE;
}

static void
up_device_battery_init (UpDeviceBattery *self)
{
	g_object_set (self,
	              "type", UP_DEVICE_KIND_BATTERY,
	              "power-supply", TRUE,
	              "is-rechargeable", TRUE,
	              NULL);

	g_signal_connect (self, "handle-enable-charge-limit",
			  G_CALLBACK (up_device_battery_set_charge_limit), self);

	UpDeviceBatteryPrivate *priv = up_device_battery_get_instance_private (self);
	if (g_getenv ("UPOWER_HISTORY_DIR")) {
		priv->state_dir = g_getenv ("UPOWER_HISTORY_DIR");
        } else {
		priv->state_dir = HISTORY_DIR;
        }
}

static void
up_device_battery_class_init (UpDeviceBatteryClass *klass)
{
	UpDeviceClass *device_class = UP_DEVICE_CLASS (klass);

	device_class->get_on_battery = up_device_battery_get_on_battery;
}
