/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2010  Nokia Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2011  Texas Instruments, Inc.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <bluetooth/uuid.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "../src/adapter.h"
#include "../src/device.h"

#include "log.h"
#include "error.h"
#include "device.h"
#include "manager.h"
#include "avctp.h"
#include "control.h"
#include "sdpd.h"
#include "glib-helper.h"
#include "dbus-common.h"

static unsigned int avctp_id = 0;

struct control {
	struct avctp *session;
	gboolean target;
	DBusMessage *connect;
};

static void state_changed(struct audio_device *dev, avctp_state_t old_state,
				avctp_state_t new_state, void *user_data)
{
	DBusConnection *conn = btd_get_dbus_connection();
	struct control *control = dev->control;
	const char *path = device_get_path(dev->btd_dev);

	switch (new_state) {
	case AVCTP_STATE_DISCONNECTED:
		control->session = NULL;

		if (control->connect) {
			DBusMessage *reply = btd_error_failed(control->connect,
							"Unable to connect");

			g_dbus_send_message(btd_get_dbus_connection(), reply);
			dbus_message_unref(control->connect);
			control->connect = NULL;
		}

		if (old_state != AVCTP_STATE_CONNECTED)
			break;

		g_dbus_emit_signal(conn, path, AUDIO_CONTROL_INTERFACE,
					"Disconnected", DBUS_TYPE_INVALID);
		g_dbus_emit_property_changed(conn, path,
					AUDIO_CONTROL_INTERFACE, "Connected");

		break;
	case AVCTP_STATE_CONNECTING:
		if (control->session)
			break;

		control->session = avctp_get(&dev->src, &dev->dst);

		break;
	case AVCTP_STATE_CONNECTED:
		if (control->connect) {
			g_dbus_send_reply(conn, control->connect,
							DBUS_TYPE_INVALID);
			dbus_message_unref(control->connect);
			control->connect = NULL;
		}

		g_dbus_emit_signal(conn, path,
				AUDIO_CONTROL_INTERFACE, "Connected",
				DBUS_TYPE_INVALID);
		g_dbus_emit_property_changed(conn, path,
					AUDIO_CONTROL_INTERFACE, "Connected");
		break;
	default:
		return;
	}
}

static DBusMessage *control_is_connected(DBusConnection *conn,
						DBusMessage *msg,
						void *data)
{
	struct audio_device *device = data;
	struct control *control = device->control;
	DBusMessage *reply;
	dbus_bool_t connected;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	connected = (control->session != NULL);

	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &connected,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *control_connect(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct audio_device *device = data;
	struct control *control = device->control;

	if (control->session)
		return btd_error_already_connected(msg);

	if (!control->target)
		return btd_error_not_supported(msg);

	if (control->connect)
		return btd_error_in_progress(msg);

	control->session = avctp_connect(&device->src, &device->dst);
	if (!control->session)
		return btd_error_failed(msg, "Unable to connect");

	control->connect = dbus_message_ref(msg);

	return NULL;
}

static DBusMessage *control_disconnect(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct audio_device *device = data;
	struct control *control = device->control;

	if (!control->session)
		return btd_error_not_connected(msg);

	avctp_disconnect(control->session);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *key_pressed(DBusConnection *conn, DBusMessage *msg,
						uint8_t op, void *data)
{
	struct audio_device *device = data;
	struct control *control = device->control;
	int err;

	if (!control->session)
		return btd_error_not_connected(msg);

	if (!control->target)
		return btd_error_not_supported(msg);

	err = avctp_send_passthrough(control->session, op);
	if (err < 0)
		return btd_error_failed(msg, strerror(-err));

	return dbus_message_new_method_return(msg);
}

static DBusMessage *control_volume_up(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	return key_pressed(conn, msg, AVC_VOLUME_UP, data);
}

static DBusMessage *control_volume_down(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	return key_pressed(conn, msg, AVC_VOLUME_DOWN, data);
}

static DBusMessage *control_play(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	return key_pressed(conn, msg, AVC_PLAY, data);
}

static DBusMessage *control_pause(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	return key_pressed(conn, msg, AVC_PAUSE, data);
}

static DBusMessage *control_stop(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	return key_pressed(conn, msg, AVC_STOP, data);
}

static DBusMessage *control_next(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	return key_pressed(conn, msg, AVC_FORWARD, data);
}

static DBusMessage *control_previous(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	return key_pressed(conn, msg, AVC_BACKWARD, data);
}

static gboolean control_property_get_connected(
					const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct audio_device *device = data;
	dbus_bool_t value = (device->control->session != NULL);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);

	return TRUE;
}

static const GDBusMethodTable control_methods[] = {
	{ GDBUS_DEPRECATED_METHOD("IsConnected",
				NULL, GDBUS_ARGS({ "connected", "b" }),
				control_is_connected) },
	{ GDBUS_ASYNC_METHOD("Connect", NULL, NULL, control_connect) },
	{ GDBUS_METHOD("Disconnect", NULL, NULL, control_disconnect) },
	{ GDBUS_METHOD("Play", NULL, NULL, control_play) },
	{ GDBUS_METHOD("Pause", NULL, NULL, control_pause) },
	{ GDBUS_METHOD("Stop", NULL, NULL, control_stop) },
	{ GDBUS_METHOD("Next", NULL, NULL, control_next) },
	{ GDBUS_METHOD("Previous", NULL, NULL, control_previous) },
	{ GDBUS_METHOD("VolumeUp", NULL, NULL, control_volume_up) },
	{ GDBUS_METHOD("VolumeDown", NULL, NULL, control_volume_down) },
	{ }
};

static const GDBusSignalTable control_signals[] = {
	{ GDBUS_DEPRECATED_SIGNAL("Connected", NULL) },
	{ GDBUS_DEPRECATED_SIGNAL("Disconnected", NULL) },
	{ }
};

static const GDBusPropertyTable control_properties[] = {
	{ "Connected", "b", control_property_get_connected },
	{ }
};

static void path_unregister(void *data)
{
	struct audio_device *dev = data;
	struct control *control = dev->control;

	DBG("Unregistered interface %s on path %s",
		AUDIO_CONTROL_INTERFACE, device_get_path(dev->btd_dev));

	if (control->session)
		avctp_disconnect(control->session);

	if (control->connect)
		dbus_message_unref(control->connect);

	g_free(control);
	dev->control = NULL;
}

void control_unregister(struct audio_device *dev)
{
	g_dbus_unregister_interface(btd_get_dbus_connection(),
						device_get_path(dev->btd_dev),
						AUDIO_CONTROL_INTERFACE);
}

void control_update(struct control *control, GSList *uuids)
{
	if (g_slist_find_custom(uuids, AVRCP_TARGET_UUID, bt_uuid_strcmp))
		control->target = TRUE;
}

struct control *control_init(struct audio_device *dev, GSList *uuids)
{
	struct control *control;

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
					device_get_path(dev->btd_dev),
					AUDIO_CONTROL_INTERFACE,
					control_methods, control_signals,
					control_properties, dev,
					path_unregister))
		return NULL;

	DBG("Registered interface %s on path %s",
		AUDIO_CONTROL_INTERFACE, device_get_path(dev->btd_dev));

	control = g_new0(struct control, 1);

	control_update(control, uuids);

	if (!avctp_id)
		avctp_id = avctp_add_state_cb(state_changed, NULL);

	return control;
}

gboolean control_is_active(struct audio_device *dev)
{
	struct control *control = dev->control;

	if (control && control->session)
		return TRUE;

	return FALSE;
}