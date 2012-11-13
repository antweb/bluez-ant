/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2009  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2011  BMW Car IT GmbH. All rights reserved.
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

#include <errno.h>

#include <bluetooth/uuid.h>

#include <glib.h>
#include <gdbus.h>

#include "../src/adapter.h"
#include "../src/device.h"
#include "../src/dbus-common.h"
#include "../src/profile.h"

#include "glib-helper.h"
#include "log.h"
#include "error.h"
#include "device.h"
#include "avdtp.h"
#include "media.h"
#include "transport.h"
#include "a2dp.h"
#include "avrcp.h"
#include "manager.h"

#define MEDIA_INTERFACE "org.bluez.Media"
#define MEDIA_ENDPOINT_INTERFACE "org.bluez.MediaEndpoint"
#define MEDIA_PLAYER_INTERFACE "org.bluez.MediaPlayer"

#define REQUEST_TIMEOUT (3 * 1000)		/* 3 seconds */

struct media_adapter {
	bdaddr_t		src;		/* Adapter address */
	char			*path;		/* Adapter path */
	GSList			*endpoints;	/* Endpoints list */
	GSList			*players;	/* Players list */
};

struct endpoint_request {
	struct media_endpoint	*endpoint;
	DBusMessage		*msg;
	DBusPendingCall		*call;
	media_endpoint_cb_t	cb;
	GDestroyNotify		destroy;
	void			*user_data;
};

struct media_endpoint {
	struct a2dp_sep		*sep;
	char			*sender;	/* Endpoint DBus bus id */
	char			*path;		/* Endpoint object path */
	char			*uuid;		/* Endpoint property UUID */
	uint8_t			codec;		/* Endpoint codec */
	uint8_t			*capabilities;	/* Endpoint property capabilities */
	size_t			size;		/* Endpoint capabilities size */
	guint			hs_watch;
	guint			ag_watch;
	guint			watch;
	GSList			*requests;
	struct media_adapter	*adapter;
	GSList			*transports;
};

struct media_player {
	struct media_adapter	*adapter;
	struct avrcp_player	*player;
	char			*sender;	/* Player DBus bus id */
	char			*path;		/* Player object path */
	GHashTable		*settings;	/* Player settings */
	GHashTable		*track;		/* Player current track */
	guint			watch;
	guint			property_watch;
	guint			track_watch;
	char			*status;
	uint32_t		position;
	uint32_t		duration;
	uint8_t			volume;
	GTimer			*timer;
};

static GSList *adapters = NULL;

static void endpoint_request_free(struct endpoint_request *request)
{
	if (request->call)
		dbus_pending_call_unref(request->call);

	if (request->destroy)
		request->destroy(request->user_data);

	dbus_message_unref(request->msg);
	g_free(request);
}

static void media_endpoint_cancel(struct endpoint_request *request)
{
	struct media_endpoint *endpoint = request->endpoint;

	if (request->call)
		dbus_pending_call_cancel(request->call);

	endpoint->requests = g_slist_remove(endpoint->requests, request);

	endpoint_request_free(request);
}

static void media_endpoint_cancel_all(struct media_endpoint *endpoint)
{
	while (endpoint->requests != NULL)
		media_endpoint_cancel(endpoint->requests->data);
}

static void media_endpoint_destroy(struct media_endpoint *endpoint)
{
	DBG("sender=%s path=%s", endpoint->sender, endpoint->path);

	media_endpoint_cancel_all(endpoint);

	g_slist_free_full(endpoint->transports,
				(GDestroyNotify) media_transport_destroy);

	g_dbus_remove_watch(btd_get_dbus_connection(), endpoint->watch);
	g_free(endpoint->capabilities);
	g_free(endpoint->sender);
	g_free(endpoint->path);
	g_free(endpoint->uuid);
	g_free(endpoint);
}

static void media_endpoint_remove(struct media_endpoint *endpoint)
{
	struct media_adapter *adapter = endpoint->adapter;

	if (endpoint->sep) {
		a2dp_remove_sep(endpoint->sep);
		return;
	}

	info("Endpoint unregistered: sender=%s path=%s", endpoint->sender,
			endpoint->path);

	adapter->endpoints = g_slist_remove(adapter->endpoints, endpoint);

	media_endpoint_destroy(endpoint);
}

static void media_endpoint_exit(DBusConnection *connection, void *user_data)
{
	struct media_endpoint *endpoint = user_data;

	endpoint->watch = 0;
	media_endpoint_remove(endpoint);
}

static void clear_configuration(struct media_endpoint *endpoint,
					struct media_transport *transport)
{
	DBusMessage *msg;
	const char *path;

	msg = dbus_message_new_method_call(endpoint->sender, endpoint->path,
						MEDIA_ENDPOINT_INTERFACE,
						"ClearConfiguration");
	if (msg == NULL) {
		error("Couldn't allocate D-Bus message");
		goto done;
	}

	path = media_transport_get_path(transport);
	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID);
	g_dbus_send_message(btd_get_dbus_connection(), msg);
done:
	endpoint->transports = g_slist_remove(endpoint->transports, transport);
	media_transport_destroy(transport);
}

static void clear_endpoint(struct media_endpoint *endpoint)
{
	media_endpoint_cancel_all(endpoint);

	while (endpoint->transports != NULL)
		clear_configuration(endpoint, endpoint->transports->data);
}

static void endpoint_reply(DBusPendingCall *call, void *user_data)
{
	struct endpoint_request *request = user_data;
	struct media_endpoint *endpoint = request->endpoint;
	DBusMessage *reply;
	DBusError err;
	gboolean value;
	void *ret = NULL;
	int size = -1;

	/* steal_reply will always return non-NULL since the callback
	 * is only called after a reply has been received */
	reply = dbus_pending_call_steal_reply(call);

	dbus_error_init(&err);
	if (dbus_set_error_from_message(&err, reply)) {
		error("Endpoint replied with an error: %s",
				err.name);

		/* Clear endpoint configuration in case of NO_REPLY error */
		if (dbus_error_has_name(&err, DBUS_ERROR_NO_REPLY)) {
			if (request->cb)
				request->cb(endpoint, NULL, size,
							request->user_data);
			clear_endpoint(endpoint);
			dbus_message_unref(reply);
			dbus_error_free(&err);
			return;
		}

		dbus_error_free(&err);
		goto done;
	}

	if (dbus_message_is_method_call(request->msg, MEDIA_ENDPOINT_INTERFACE,
				"SelectConfiguration")) {
		DBusMessageIter args, array;
		uint8_t *configuration;

		dbus_message_iter_init(reply, &args);

		dbus_message_iter_recurse(&args, &array);

		dbus_message_iter_get_fixed_array(&array, &configuration, &size);

		ret = configuration;
		goto done;
	} else  if (!dbus_message_get_args(reply, &err, DBUS_TYPE_INVALID)) {
		error("Wrong reply signature: %s", err.message);
		dbus_error_free(&err);
		goto done;
	}

	size = 1;
	value = TRUE;
	ret = &value;

done:
	dbus_message_unref(reply);

	if (request->cb)
		request->cb(endpoint, ret, size, request->user_data);

	endpoint->requests = g_slist_remove(endpoint->requests, request);
	endpoint_request_free(request);
}

static gboolean media_endpoint_async_call(DBusMessage *msg,
					struct media_endpoint *endpoint,
					media_endpoint_cb_t cb,
					void *user_data,
					GDestroyNotify destroy)
{
	struct endpoint_request *request;

	request = g_new0(struct endpoint_request, 1);

	/* Timeout should be less than avdtp request timeout (4 seconds) */
	if (dbus_connection_send_with_reply(btd_get_dbus_connection(),
						msg, &request->call,
						REQUEST_TIMEOUT) == FALSE) {
		error("D-Bus send failed");
		g_free(request);
		return FALSE;
	}

	dbus_pending_call_set_notify(request->call, endpoint_reply, request,
									NULL);

	request->endpoint = endpoint;
	request->msg = msg;
	request->cb = cb;
	request->destroy = destroy;
	request->user_data = user_data;

	endpoint->requests = g_slist_append(endpoint->requests, request);

	DBG("Calling %s: name = %s path = %s", dbus_message_get_member(msg),
			dbus_message_get_destination(msg),
			dbus_message_get_path(msg));

	return TRUE;
}

static gboolean select_configuration(struct media_endpoint *endpoint,
						uint8_t *capabilities,
						size_t length,
						media_endpoint_cb_t cb,
						void *user_data,
						GDestroyNotify destroy)
{
	DBusMessage *msg;

	msg = dbus_message_new_method_call(endpoint->sender, endpoint->path,
						MEDIA_ENDPOINT_INTERFACE,
						"SelectConfiguration");
	if (msg == NULL) {
		error("Couldn't allocate D-Bus message");
		return FALSE;
	}

	dbus_message_append_args(msg, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
					&capabilities, length,
					DBUS_TYPE_INVALID);

	return media_endpoint_async_call(msg, endpoint, cb, user_data, destroy);
}

static gint transport_device_cmp(gconstpointer data, gconstpointer user_data)
{
	struct media_transport *transport = (struct media_transport *) data;
	const struct audio_device *device = user_data;

	if (device == media_transport_get_dev(transport))
		return 0;

	return -1;
}

static struct media_transport *find_device_transport(
					struct media_endpoint *endpoint,
					struct audio_device *device)
{
	GSList *match;

	match = g_slist_find_custom(endpoint->transports, device,
							transport_device_cmp);
	if (match == NULL)
		return NULL;

	return match->data;
}

static gboolean set_configuration(struct media_endpoint *endpoint,
					struct audio_device *device,
					uint8_t *configuration, size_t size,
					media_endpoint_cb_t cb,
					void *user_data,
					GDestroyNotify destroy)
{
	DBusMessage *msg;
	const char *path;
	DBusMessageIter iter;
	struct media_transport *transport;

	transport = find_device_transport(endpoint, device);

	if (transport != NULL)
		return FALSE;

	transport = media_transport_create(endpoint, device,
						configuration, size);
	if (transport == NULL)
		return FALSE;

	msg = dbus_message_new_method_call(endpoint->sender, endpoint->path,
						MEDIA_ENDPOINT_INTERFACE,
						"SetConfiguration");
	if (msg == NULL) {
		error("Couldn't allocate D-Bus message");
		media_transport_destroy(transport);
		return FALSE;
	}

	endpoint->transports = g_slist_append(endpoint->transports, transport);

	dbus_message_iter_init_append(msg, &iter);

	path = media_transport_get_path(transport);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);

	transport_get_properties(transport, &iter);

	return media_endpoint_async_call(msg, endpoint, cb, user_data, destroy);
}

static void release_endpoint(struct media_endpoint *endpoint)
{
	DBusMessage *msg;

	DBG("sender=%s path=%s", endpoint->sender, endpoint->path);

	/* already exit */
	if (endpoint->watch == 0)
		goto done;

	clear_endpoint(endpoint);

	msg = dbus_message_new_method_call(endpoint->sender, endpoint->path,
						MEDIA_ENDPOINT_INTERFACE,
						"Release");
	if (msg == NULL) {
		error("Couldn't allocate D-Bus message");
		return;
	}

	g_dbus_send_message(btd_get_dbus_connection(), msg);

done:
	media_endpoint_remove(endpoint);
}

static const char *get_name(struct a2dp_sep *sep, void *user_data)
{
	struct media_endpoint *endpoint = user_data;

	return endpoint->sender;
}

static size_t get_capabilities(struct a2dp_sep *sep, uint8_t **capabilities,
							void *user_data)
{
	struct media_endpoint *endpoint = user_data;

	*capabilities = endpoint->capabilities;
	return endpoint->size;
}

struct a2dp_config_data {
	struct a2dp_setup *setup;
	a2dp_endpoint_config_t cb;
};

struct a2dp_select_data {
	struct a2dp_setup *setup;
	a2dp_endpoint_select_t cb;
};

static void select_cb(struct media_endpoint *endpoint, void *ret, int size,
							void *user_data)
{
	struct a2dp_select_data *data = user_data;

	data->cb(data->setup, ret, size);
}

static int select_config(struct a2dp_sep *sep, uint8_t *capabilities,
				size_t length, struct a2dp_setup *setup,
				a2dp_endpoint_select_t cb, void *user_data)
{
	struct media_endpoint *endpoint = user_data;
	struct a2dp_select_data *data;

	data = g_new0(struct a2dp_select_data, 1);
	data->setup = setup;
	data->cb = cb;

	if (select_configuration(endpoint, capabilities, length,
					select_cb, data, g_free) == TRUE)
		return 0;

	g_free(data);
	return -ENOMEM;
}

static void config_cb(struct media_endpoint *endpoint, void *ret, int size,
							void *user_data)
{
	struct a2dp_config_data *data = user_data;

	data->cb(data->setup, ret ? TRUE : FALSE);
}

static int set_config(struct a2dp_sep *sep, struct audio_device *dev,
				uint8_t *configuration, size_t length,
				struct a2dp_setup *setup,
				a2dp_endpoint_config_t cb,
				void *user_data)
{
	struct media_endpoint *endpoint = user_data;
	struct a2dp_config_data *data;

	data = g_new0(struct a2dp_config_data, 1);
	data->setup = setup;
	data->cb = cb;

	if (set_configuration(endpoint, dev, configuration, length,
					config_cb, data, g_free) == TRUE)
		return 0;

	g_free(data);
	return -ENOMEM;
}

static void clear_config(struct a2dp_sep *sep, void *user_data)
{
	struct media_endpoint *endpoint = user_data;

	clear_endpoint(endpoint);
}

static void set_delay(struct a2dp_sep *sep, uint16_t delay, void *user_data)
{
	struct media_endpoint *endpoint = user_data;

	if (endpoint->transports == NULL)
		return;

	media_transport_update_delay(endpoint->transports->data, delay);
}

static struct a2dp_endpoint a2dp_endpoint = {
	.get_name = get_name,
	.get_capabilities = get_capabilities,
	.select_configuration = select_config,
	.set_configuration = set_config,
	.clear_configuration = clear_config,
	.set_delay = set_delay
};

static void a2dp_destroy_endpoint(void *user_data)
{
	struct media_endpoint *endpoint = user_data;

	endpoint->sep = NULL;
	release_endpoint(endpoint);
}

static gboolean endpoint_init_a2dp_source(struct media_endpoint *endpoint,
						gboolean delay_reporting,
						int *err)
{
	endpoint->sep = a2dp_add_sep(&endpoint->adapter->src,
					AVDTP_SEP_TYPE_SOURCE, endpoint->codec,
					delay_reporting, &a2dp_endpoint,
					endpoint, a2dp_destroy_endpoint, err);
	if (endpoint->sep == NULL)
		return FALSE;

	return TRUE;
}

static gboolean endpoint_init_a2dp_sink(struct media_endpoint *endpoint,
						gboolean delay_reporting,
						int *err)
{
	endpoint->sep = a2dp_add_sep(&endpoint->adapter->src,
					AVDTP_SEP_TYPE_SINK, endpoint->codec,
					delay_reporting, &a2dp_endpoint,
					endpoint, a2dp_destroy_endpoint, err);
	if (endpoint->sep == NULL)
		return FALSE;

	return TRUE;
}

static struct media_endpoint *media_adapter_find_endpoint(
						struct media_adapter *adapter,
						const char *sender,
						const char *path,
						const char *uuid)
{
	GSList *l;

	for (l = adapter->endpoints; l; l = l->next) {
		struct media_endpoint *endpoint = l->data;

		if (sender && g_strcmp0(endpoint->sender, sender) != 0)
			continue;

		if (path && g_strcmp0(endpoint->path, path) != 0)
			continue;

		if (uuid && g_strcmp0(endpoint->uuid, uuid) != 0)
			continue;

		return endpoint;
	}

	return NULL;
}

static bool endpoint_properties_exists(const char *uuid,
						struct btd_device *dev,
						void *user_data)
{
	struct media_adapter *adapter = user_data;
	struct btd_adapter *btd_adapter = device_get_adapter(dev);
	const bdaddr_t *src = adapter_get_address(btd_adapter);

	if (bacmp(&adapter->src, src) != 0)
		return false;

	if (media_adapter_find_endpoint(adapter, NULL, NULL, uuid) == NULL)
		return false;

	return true;
}

static void append_endpoint(struct media_endpoint *endpoint,
						DBusMessageIter *dict)
{
	DBusMessageIter entry, var, props;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
							NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
						&endpoint->sender);

	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a{sv}",
								&var);

	dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY,
					DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_STRING_AS_STRING
					DBUS_TYPE_VARIANT_AS_STRING
					DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
					&props);

	dict_append_entry(&props, "Path", DBUS_TYPE_OBJECT_PATH,
							&endpoint->path);
	dict_append_entry(&props, "Codec", DBUS_TYPE_BYTE, &endpoint->codec);
	dict_append_array(&props, "Capabilities", DBUS_TYPE_BYTE,
				&endpoint->capabilities, endpoint->size);

	dbus_message_iter_close_container(&var, &props);
	dbus_message_iter_close_container(&entry, &var);
	dbus_message_iter_close_container(dict, &entry);
}

static bool endpoint_properties_get(const char *uuid,
						struct btd_device *dev,
						DBusMessageIter *iter,
						void *user_data)
{
	struct media_adapter *adapter = user_data;
	DBusMessageIter dict;
	GSList *l;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_STRING_AS_STRING
					DBUS_TYPE_VARIANT_AS_STRING
					DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
					&dict);

	for (l = adapter->endpoints; l; l = l->next) {
		struct media_endpoint *endpoint = l->data;

		if (g_strcmp0(endpoint->uuid, uuid) != 0)
			continue;

		append_endpoint(endpoint, &dict);
	}

	dbus_message_iter_close_container(iter, &dict);

	return true;
}

static struct media_endpoint *media_endpoint_create(struct media_adapter *adapter,
						const char *sender,
						const char *path,
						const char *uuid,
						gboolean delay_reporting,
						uint8_t codec,
						uint8_t *capabilities,
						int size,
						int *err)
{
	struct media_endpoint *endpoint;
	gboolean succeeded;

	endpoint = g_new0(struct media_endpoint, 1);
	endpoint->sender = g_strdup(sender);
	endpoint->path = g_strdup(path);
	endpoint->uuid = g_strdup(uuid);
	endpoint->codec = codec;

	if (size > 0) {
		endpoint->capabilities = g_new(uint8_t, size);
		memcpy(endpoint->capabilities, capabilities, size);
		endpoint->size = size;
	}

	endpoint->adapter = adapter;

	if (strcasecmp(uuid, A2DP_SOURCE_UUID) == 0)
		succeeded = endpoint_init_a2dp_source(endpoint,
							delay_reporting, err);
	else if (strcasecmp(uuid, A2DP_SINK_UUID) == 0)
		succeeded = endpoint_init_a2dp_sink(endpoint,
							delay_reporting, err);
	else if (strcasecmp(uuid, HFP_AG_UUID) == 0 ||
					strcasecmp(uuid, HSP_AG_UUID) == 0)
		succeeded = TRUE;
	else if (strcasecmp(uuid, HFP_HS_UUID) == 0 ||
					strcasecmp(uuid, HSP_HS_UUID) == 0)
		succeeded = TRUE;
	else {
		succeeded = FALSE;

		if (err)
			*err = -EINVAL;
	}

	if (!succeeded) {
		media_endpoint_destroy(endpoint);
		return NULL;
	}

	endpoint->watch = g_dbus_add_disconnect_watch(btd_get_dbus_connection(),
						sender, media_endpoint_exit,
						endpoint, NULL);

	if (media_adapter_find_endpoint(adapter, NULL, NULL, uuid) == NULL) {
		btd_profile_add_custom_prop(uuid, "a{sv}", "MediaEndpoints",
						endpoint_properties_exists,
						endpoint_properties_get,
						adapter);
	}

	adapter->endpoints = g_slist_append(adapter->endpoints, endpoint);
	info("Endpoint registered: sender=%s path=%s", sender, path);

	if (err)
		*err = 0;
	return endpoint;
}

static int parse_properties(DBusMessageIter *props, const char **uuid,
				gboolean *delay_reporting, uint8_t *codec,
				uint8_t **capabilities, int *size)
{
	gboolean has_uuid = FALSE;
	gboolean has_codec = FALSE;

	while (dbus_message_iter_get_arg_type(props) == DBUS_TYPE_DICT_ENTRY) {
		const char *key;
		DBusMessageIter value, entry;
		int var;

		dbus_message_iter_recurse(props, &entry);
		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);

		var = dbus_message_iter_get_arg_type(&value);
		if (strcasecmp(key, "UUID") == 0) {
			if (var != DBUS_TYPE_STRING)
				return -EINVAL;
			dbus_message_iter_get_basic(&value, uuid);
			has_uuid = TRUE;
		} else if (strcasecmp(key, "Codec") == 0) {
			if (var != DBUS_TYPE_BYTE)
				return -EINVAL;
			dbus_message_iter_get_basic(&value, codec);
			has_codec = TRUE;
		} else if (strcasecmp(key, "DelayReporting") == 0) {
			if (var != DBUS_TYPE_BOOLEAN)
				return -EINVAL;
			dbus_message_iter_get_basic(&value, delay_reporting);
		} else if (strcasecmp(key, "Capabilities") == 0) {
			DBusMessageIter array;

			if (var != DBUS_TYPE_ARRAY)
				return -EINVAL;

			dbus_message_iter_recurse(&value, &array);
			dbus_message_iter_get_fixed_array(&array, capabilities,
							size);
		}

		dbus_message_iter_next(props);
	}

	return (has_uuid && has_codec) ? 0 : -EINVAL;
}

static DBusMessage *register_endpoint(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct media_adapter *adapter = data;
	DBusMessageIter args, props;
	const char *sender, *path, *uuid;
	gboolean delay_reporting = FALSE;
	uint8_t codec;
	uint8_t *capabilities;
	int size = 0;
	int err;

	sender = dbus_message_get_sender(msg);

	dbus_message_iter_init(msg, &args);

	dbus_message_iter_get_basic(&args, &path);
	dbus_message_iter_next(&args);

	if (media_adapter_find_endpoint(adapter, sender, path, NULL) != NULL)
		return btd_error_already_exists(msg);

	dbus_message_iter_recurse(&args, &props);
	if (dbus_message_iter_get_arg_type(&props) != DBUS_TYPE_DICT_ENTRY)
		return btd_error_invalid_args(msg);

	if (parse_properties(&props, &uuid, &delay_reporting, &codec,
						&capabilities, &size) < 0)
		return btd_error_invalid_args(msg);

	if (media_endpoint_create(adapter, sender, path, uuid, delay_reporting,
				codec, capabilities, size, &err) == NULL) {
		if (err == -EPROTONOSUPPORT)
			return btd_error_not_supported(msg);
		else
			return btd_error_invalid_args(msg);
	}

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *unregister_endpoint(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct media_adapter *adapter = data;
	struct media_endpoint *endpoint;
	const char *sender, *path;

	if (!dbus_message_get_args(msg, NULL,
				DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	sender = dbus_message_get_sender(msg);

	endpoint = media_adapter_find_endpoint(adapter, sender, path, NULL);
	if (endpoint == NULL)
		return btd_error_does_not_exist(msg);

	media_endpoint_remove(endpoint);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static struct media_player *media_adapter_find_player(
						struct media_adapter *adapter,
						const char *sender,
						const char *path)
{
	GSList *l;

	for (l = adapter->players; l; l = l->next) {
		struct media_player *mp = l->data;

		if (sender && g_strcmp0(mp->sender, sender) != 0)
			continue;

		if (path && g_strcmp0(mp->path, path) != 0)
			continue;

		return mp;
	}

	return NULL;
}

static void release_player(struct media_player *mp)
{
	DBusMessage *msg;

	DBG("sender=%s path=%s", mp->sender, mp->path);

	msg = dbus_message_new_method_call(mp->sender, mp->path,
						MEDIA_PLAYER_INTERFACE,
						"Release");
	if (msg == NULL) {
		error("Couldn't allocate D-Bus message");
		return;
	}

	g_dbus_send_message(btd_get_dbus_connection(), msg);
}

static void media_player_free(gpointer data)
{
	DBusConnection *conn = btd_get_dbus_connection();
	struct media_player *mp = data;
	struct media_adapter *adapter = mp->adapter;

	if (mp->player) {
		adapter->players = g_slist_remove(adapter->players, mp);
		release_player(mp);
	}

	g_dbus_remove_watch(conn, mp->watch);
	g_dbus_remove_watch(conn, mp->property_watch);
	g_dbus_remove_watch(conn, mp->track_watch);

	if (mp->track)
		g_hash_table_unref(mp->track);

	if (mp->settings)
		g_hash_table_unref(mp->settings);

	g_timer_destroy(mp->timer);
	g_free(mp->sender);
	g_free(mp->path);
	g_free(mp->status);
	g_free(mp);
}

static void media_player_destroy(struct media_player *mp)
{
	struct media_adapter *adapter = mp->adapter;

	DBG("sender=%s path=%s", mp->sender, mp->path);

	if (mp->player) {
		struct avrcp_player *player = mp->player;
		mp->player = NULL;
		adapter->players = g_slist_remove(adapter->players, mp);
		avrcp_unregister_player(player);
		return;
	}

	media_player_free(mp);
}

static void media_player_remove(struct media_player *mp)
{
	info("Player unregistered: sender=%s path=%s", mp->sender, mp->path);

	media_player_destroy(mp);
}

static GList *list_settings(void *user_data)
{
	struct media_player *mp = user_data;

	DBG("");

	if (mp->settings == NULL)
		return NULL;

	return g_hash_table_get_keys(mp->settings);
}

static const char *get_setting(const char *key, void *user_data)
{
	struct media_player *mp = user_data;

	DBG("%s", key);

	return g_hash_table_lookup(mp->settings, key);
}

static int set_setting(const char *key, const char *value, void *user_data)
{
	struct media_player *mp = user_data;
	DBusMessage *msg;
	DBusMessageIter iter, var;

	DBG("%s = %s", key, value);

	if (!g_hash_table_lookup(mp->settings, key))
		return -EINVAL;

	msg = dbus_message_new_method_call(mp->sender, mp->path,
						MEDIA_PLAYER_INTERFACE,
						"SetProperty");
	if (msg == NULL) {
		error("Couldn't allocate D-Bus message");
		return -ENOMEM;
	}

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &key);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
						DBUS_TYPE_STRING_AS_STRING,
						&var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &value);
	dbus_message_iter_close_container(&iter, &var);

	g_dbus_send_message(btd_get_dbus_connection(), msg);

	return 0;
}

static GList *list_metadata(void *user_data)
{
	struct media_player *mp = user_data;

	DBG("");

	if (mp->track == NULL)
		return NULL;

	return g_hash_table_get_keys(mp->track);
}

static uint64_t get_uid(void *user_data)
{
	struct media_player *mp = user_data;

	DBG("%p", mp->track);

	if (mp->track == NULL)
		return UINT64_MAX;

	return 0;
}

static const char *get_metadata(const char *key, void *user_data)
{
	struct media_player *mp = user_data;

	DBG("%s", key);

	if (mp->track == NULL)
		return NULL;

	return g_hash_table_lookup(mp->track, key);
}

static const char *get_status(void *user_data)
{
	struct media_player *mp = user_data;

	return mp->status;
}

static uint32_t get_position(void *user_data)
{
	struct media_player *mp = user_data;
	double timedelta;
	uint32_t sec, msec;

	if (g_strcmp0(mp->status, "playing") != 0)
		return mp->position;

	timedelta = g_timer_elapsed(mp->timer, NULL);

	sec = (uint32_t) timedelta;
	msec = (uint32_t) ((timedelta - sec) * 1000);

	return mp->position + sec * 1000 + msec;
}

static uint32_t get_duration(void *user_data)
{
	struct media_player *mp = user_data;

	return mp->duration;
}

static void set_volume(uint8_t volume, struct audio_device *dev, void *user_data)
{
	struct media_player *mp = user_data;
	GSList *l;

	if (mp->volume == volume)
		return;

	mp->volume = volume;

	for (l = mp->adapter->endpoints; l; l = l->next) {
		struct media_endpoint *endpoint = l->data;
		struct media_transport *transport;

		/* Volume is A2DP only */
		if (endpoint->sep == NULL)
			continue;

		transport = find_device_transport(endpoint, dev);
		if (transport == NULL)
			continue;

		media_transport_update_volume(transport, volume);
	}
}

static struct avrcp_player_cb player_cb = {
	.list_settings = list_settings,
	.get_setting = get_setting,
	.set_setting = set_setting,
	.list_metadata = list_metadata,
	.get_uid = get_uid,
	.get_metadata = get_metadata,
	.get_position = get_position,
	.get_duration = get_duration,
	.get_status = get_status,
	.set_volume = set_volume
};

static void media_player_exit(DBusConnection *connection, void *user_data)
{
	struct media_player *mp = user_data;

	mp->watch = 0;
	media_player_remove(mp);
}

static gboolean set_status(struct media_player *mp, DBusMessageIter *iter)
{
	const char *value;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_STRING)
		return FALSE;

	dbus_message_iter_get_basic(iter, &value);
	DBG("Status=%s", value);

	if (g_strcmp0(mp->status, value) == 0)
		return TRUE;

	mp->position = get_position(mp);
	g_timer_start(mp->timer);

	g_free(mp->status);
	mp->status = g_strdup(value);

	avrcp_player_event(mp->player, AVRCP_EVENT_STATUS_CHANGED, mp->status);

	return TRUE;
}

static gboolean set_position(struct media_player *mp, DBusMessageIter *iter)
{
	uint32_t value;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_UINT32)
			return FALSE;

	dbus_message_iter_get_basic(iter, &value);
	DBG("Position=%u", value);

	mp->position = value;
	g_timer_start(mp->timer);

	if (!mp->position) {
		avrcp_player_event(mp->player,
					AVRCP_EVENT_TRACK_REACHED_START, NULL);
		return TRUE;
	}

	/*
	 * If position is the maximum value allowed or greater than track's
	 * duration, we send a track-reached-end event.
	 */
	if (mp->position == UINT32_MAX || mp->position >= mp->duration)
		avrcp_player_event(mp->player, AVRCP_EVENT_TRACK_REACHED_END,
									NULL);

	return TRUE;
}

static gboolean set_player_property(struct media_player *mp, const char *key,
							DBusMessageIter *entry)
{
	DBusMessageIter var;
	const char *value, *curval;
	GList *settings;

	if (dbus_message_iter_get_arg_type(entry) != DBUS_TYPE_VARIANT)
		return FALSE;

	dbus_message_iter_recurse(entry, &var);

	if (strcasecmp(key, "Status") == 0)
		return set_status(mp, &var);

	if (strcasecmp(key, "Position") == 0)
		return set_position(mp, &var);

	if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
		return FALSE;

	dbus_message_iter_get_basic(&var, &value);

	curval = g_hash_table_lookup(mp->settings, key);
	if (g_strcmp0(curval, value) == 0)
		return TRUE;

	DBG("%s=%s", key, value);

	g_hash_table_replace(mp->settings, g_strdup(key), g_strdup(value));

	settings = list_settings(mp);

	avrcp_player_event(mp->player, AVRCP_EVENT_SETTINGS_CHANGED, settings);

	g_list_free(settings);

	return TRUE;
}

static gboolean property_changed(DBusConnection *connection, DBusMessage *msg,
							void *user_data)
{
	struct media_player *mp = user_data;
	DBusMessageIter iter;
	const char *property;

	DBG("sender=%s path=%s", mp->sender, mp->path);

	dbus_message_iter_init(msg, &iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
		error("Unexpected signature in %s.%s signal",
					dbus_message_get_interface(msg),
					dbus_message_get_member(msg));
		return TRUE;
	}

	dbus_message_iter_get_basic(&iter, &property);

	dbus_message_iter_next(&iter);

	set_player_property(mp, property, &iter);

	return TRUE;
}

static gboolean parse_player_metadata(struct media_player *mp,
							DBusMessageIter *iter)
{
	DBusMessageIter dict;
	DBusMessageIter var;
	GHashTable *track;
	int ctype;
	gboolean title = FALSE;
	uint64_t uid;

	ctype = dbus_message_iter_get_arg_type(iter);
	if (ctype != DBUS_TYPE_ARRAY)
		return FALSE;

	dbus_message_iter_recurse(iter, &dict);

	track = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
								g_free);

	while ((ctype = dbus_message_iter_get_arg_type(&dict)) !=
							DBUS_TYPE_INVALID) {
		DBusMessageIter entry;
		const char *key;
		const char *string;
		char valstr[20];
		char *value;
		uint32_t num;
		int type;

		if (ctype != DBUS_TYPE_DICT_ENTRY)
			goto parse_error;

		dbus_message_iter_recurse(&dict, &entry);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			goto parse_error;

		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			goto parse_error;

		dbus_message_iter_recurse(&entry, &var);

		type = dbus_message_iter_get_arg_type(&var);

		if (strcasecmp(key, "Title") == 0) {
			if (type != DBUS_TYPE_STRING)
				goto parse_error;
			title = TRUE;
			dbus_message_iter_get_basic(&var, &string);
		} else if (strcasecmp(key, "Artist") == 0) {
			if (type != DBUS_TYPE_STRING)
				goto parse_error;

			dbus_message_iter_get_basic(&var, &string);
		} else if (strcasecmp(key, "Album") == 0) {
			if (type != DBUS_TYPE_STRING)
				goto parse_error;

			dbus_message_iter_get_basic(&var, &string);
		} else if (strcasecmp(key, "Genre") == 0) {
			if (type != DBUS_TYPE_STRING)
				goto parse_error;

			dbus_message_iter_get_basic(&var, &string);
		} else if (strcasecmp(key, "Duration") == 0) {
			if (type != DBUS_TYPE_UINT32)
				goto parse_error;

			dbus_message_iter_get_basic(&var, &num);
			mp->duration = num;
		} else if (strcasecmp(key, "Track") == 0) {
			if (type != DBUS_TYPE_UINT32)
				goto parse_error;

			dbus_message_iter_get_basic(&var, &num);
		} else if (strcasecmp(key, "NumberOfTracks") == 0) {
			if (type != DBUS_TYPE_UINT32)
				goto parse_error;

			dbus_message_iter_get_basic(&var, &num);
		} else
			goto parse_error;

		switch (type) {
		case DBUS_TYPE_STRING:
			value = g_strdup(string);
			break;
		default:
			snprintf(valstr, 20, "%u", num);
			value = g_strdup(valstr);
		}

		DBG("%s=%s", key, value);
		g_hash_table_replace(track, g_strdup(key), value);
		dbus_message_iter_next(&dict);
	}

	if (g_hash_table_size(track) == 0) {
		g_hash_table_unref(track);
		track = NULL;
	} else if (title == FALSE)
		g_hash_table_insert(track, g_strdup("Title"), g_strdup(""));

	if (mp->track != NULL)
		g_hash_table_unref(mp->track);

	mp->track = track;
	mp->position = 0;
	g_timer_start(mp->timer);
	uid = get_uid(mp);

	avrcp_player_event(mp->player, AVRCP_EVENT_TRACK_CHANGED, &uid);
	avrcp_player_event(mp->player, AVRCP_EVENT_TRACK_REACHED_START,
								NULL);

	return TRUE;

parse_error:
	if (track)
		g_hash_table_unref(track);

	return FALSE;
}

static gboolean track_changed(DBusConnection *connection, DBusMessage *msg,
							void *user_data)
{
	struct media_player *mp = user_data;
	DBusMessageIter iter;

	DBG("sender=%s path=%s", mp->sender, mp->path);

	dbus_message_iter_init(msg, &iter);

	if (parse_player_metadata(mp, &iter) == FALSE) {
		error("Unexpected signature in %s.%s signal",
					dbus_message_get_interface(msg),
					dbus_message_get_member(msg));
	}

	return TRUE;
}

static struct media_player *media_player_create(struct media_adapter *adapter,
						const char *sender,
						const char *path,
						int *err)
{
	DBusConnection *conn = btd_get_dbus_connection();
	struct media_player *mp;

	mp = g_new0(struct media_player, 1);
	mp->adapter = adapter;
	mp->sender = g_strdup(sender);
	mp->path = g_strdup(path);
	mp->timer = g_timer_new();

	mp->watch = g_dbus_add_disconnect_watch(conn, sender,
						media_player_exit, mp,
						NULL);
	mp->property_watch = g_dbus_add_signal_watch(conn, sender,
						path, MEDIA_PLAYER_INTERFACE,
						"PropertyChanged",
						property_changed,
						mp, NULL);
	mp->track_watch = g_dbus_add_signal_watch(conn, sender,
						path, MEDIA_PLAYER_INTERFACE,
						"TrackChanged",
						track_changed,
						mp, NULL);
	mp->player = avrcp_register_player(&adapter->src, &player_cb, mp,
						media_player_free);
	if (!mp->player) {
		if (err)
			*err = -EPROTONOSUPPORT;
		media_player_destroy(mp);
		return NULL;
	}

	mp->settings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
								g_free);

	adapter->players = g_slist_append(adapter->players, mp);

	info("Player registered: sender=%s path=%s", sender, path);

	if (err)
		*err = 0;

	return mp;
}

static gboolean parse_player_properties(struct media_player *mp,
							DBusMessageIter *iter)
{
	DBusMessageIter dict;
	int ctype;

	ctype = dbus_message_iter_get_arg_type(iter);
	if (ctype != DBUS_TYPE_ARRAY)
		return FALSE;

	dbus_message_iter_recurse(iter, &dict);

	while ((ctype = dbus_message_iter_get_arg_type(&dict)) !=
							DBUS_TYPE_INVALID) {
		DBusMessageIter entry;
		const char *key;

		if (ctype != DBUS_TYPE_DICT_ENTRY)
			return FALSE;

		dbus_message_iter_recurse(&dict, &entry);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			return FALSE;

		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);

		if (set_player_property(mp, key, &entry) == FALSE)
			return FALSE;

		dbus_message_iter_next(&dict);
	}

	return TRUE;
}

static DBusMessage *register_player(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct media_adapter *adapter = data;
	struct media_player *mp;
	DBusMessageIter args;
	const char *sender, *path;
	int err;

	sender = dbus_message_get_sender(msg);

	dbus_message_iter_init(msg, &args);

	dbus_message_iter_get_basic(&args, &path);
	dbus_message_iter_next(&args);

	if (media_adapter_find_player(adapter, sender, path) != NULL)
		return btd_error_already_exists(msg);

	mp = media_player_create(adapter, sender, path, &err);
	if (mp == NULL) {
		if (err == -EPROTONOSUPPORT)
			return btd_error_not_supported(msg);
		else
			return btd_error_invalid_args(msg);
	}

	if (parse_player_properties(mp, &args) == FALSE) {
		media_player_destroy(mp);
		return btd_error_invalid_args(msg);
	}

	dbus_message_iter_next(&args);

	if (parse_player_metadata(mp, &args) == FALSE) {
		media_player_destroy(mp);
		return btd_error_invalid_args(msg);
	}

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static DBusMessage *unregister_player(DBusConnection *conn, DBusMessage *msg,
					void *data)
{
	struct media_adapter *adapter = data;
	struct media_player *player;
	const char *sender, *path;

	if (!dbus_message_get_args(msg, NULL,
				DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	sender = dbus_message_get_sender(msg);

	player = media_adapter_find_player(adapter, sender, path);
	if (player == NULL)
		return btd_error_does_not_exist(msg);

	media_player_remove(player);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static const GDBusMethodTable media_methods[] = {
	{ GDBUS_METHOD("RegisterEndpoint",
		GDBUS_ARGS({ "endpoint", "o" }, { "properties", "a{sv}" }),
		NULL, register_endpoint) },
	{ GDBUS_METHOD("UnregisterEndpoint",
		GDBUS_ARGS({ "endpoint", "o" }), NULL, unregister_endpoint) },
	{ GDBUS_METHOD("RegisterPlayer",
		GDBUS_ARGS({ "player", "o" }, { "properties", "a{sv}" },
						{ "metadata", "a{sv}" }),
		NULL, register_player) },
	{ GDBUS_METHOD("UnregisterPlayer",
		GDBUS_ARGS({ "player", "o" }), NULL, unregister_player) },
	{ },
};

static void path_free(void *data)
{
	struct media_adapter *adapter = data;

	while (adapter->endpoints)
		release_endpoint(adapter->endpoints->data);

	while (adapter->players)
		media_player_destroy(adapter->players->data);

	adapters = g_slist_remove(adapters, adapter);

	g_free(adapter->path);
	g_free(adapter);
}

int media_register(const char *path, const bdaddr_t *src)
{
	struct media_adapter *adapter;

	adapter = g_new0(struct media_adapter, 1);
	bacpy(&adapter->src, src);
	adapter->path = g_strdup(path);

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
					path, MEDIA_INTERFACE,
					media_methods, NULL, NULL,
					adapter, path_free)) {
		error("D-Bus failed to register %s path", path);
		path_free(adapter);
		return -1;
	}

	adapters = g_slist_append(adapters, adapter);

	return 0;
}

void media_unregister(const char *path)
{
	GSList *l;

	for (l = adapters; l; l = l->next) {
		struct media_adapter *adapter = l->data;

		if (g_strcmp0(path, adapter->path) == 0) {
			g_dbus_unregister_interface(btd_get_dbus_connection(),
						path, MEDIA_INTERFACE);
			return;
		}
	}
}

struct a2dp_sep *media_endpoint_get_sep(struct media_endpoint *endpoint)
{
	return endpoint->sep;
}

const char *media_endpoint_get_uuid(struct media_endpoint *endpoint)
{
	return endpoint->uuid;
}

uint8_t media_endpoint_get_codec(struct media_endpoint *endpoint)
{
	return endpoint->codec;
}