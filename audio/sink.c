/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2010  Nokia Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
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

#include <stdint.h>
#include <errno.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "logging.h"

#include "device.h"
#include "avdtp.h"
#include "a2dp.h"
#include "error.h"
#include "sink.h"
#include "dbus-common.h"
#include "../src/adapter.h"
#include "../src/device.h"

#define STREAM_SETUP_RETRY_TIMER 2

struct pending_request {
	DBusConnection *conn;
	DBusMessage *msg;
	unsigned int id;
};

struct sink {
	struct audio_device *dev;
	struct avdtp *session;
	struct avdtp_stream *stream;
	unsigned int cb_id;
	guint dc_id;
	guint retry_id;
	avdtp_session_state_t session_state;
	avdtp_state_t stream_state;
	sink_state_t state;
	struct pending_request *connect;
	struct pending_request *disconnect;
	DBusConnection *conn;
};

struct sink_state_callback {
	sink_state_cb cb;
	void *user_data;
	unsigned int id;
};

static GSList *sink_callbacks = NULL;

static unsigned int avdtp_callback_id = 0;

static const char *state2str(sink_state_t state)
{
	switch (state) {
	case SINK_STATE_DISCONNECTED:
		return "disconnected";
	case SINK_STATE_CONNECTING:
		return "connecting";
	case SINK_STATE_CONNECTED:
		return "connected";
	case SINK_STATE_PLAYING:
		return "playing";
	default:
		error("Invalid sink state %d", state);
		return NULL;
	}
}

static void sink_set_state(struct audio_device *dev, sink_state_t new_state)
{
	struct sink *sink = dev->sink;
	const char *state_str;
	sink_state_t old_state = sink->state;
	GSList *l;

	sink->state = new_state;

	state_str = state2str(new_state);
	if (state_str)
		emit_property_changed(dev->conn, dev->path,
					AUDIO_SINK_INTERFACE, "State",
					DBUS_TYPE_STRING, &state_str);

	for (l = sink_callbacks; l != NULL; l = l->next) {
		struct sink_state_callback *cb = l->data;
		cb->cb(dev, old_state, new_state, cb->user_data);
	}
}

static void avdtp_state_callback(struct audio_device *dev,
					struct avdtp *session,
					avdtp_session_state_t old_state,
					avdtp_session_state_t new_state,
					void *user_data)
{
	struct sink *sink = dev->sink;

	if (sink == NULL)
		return;

	switch (new_state) {
	case AVDTP_SESSION_STATE_DISCONNECTED:
		if (sink->state != SINK_STATE_CONNECTING) {
			gboolean value = FALSE;
			g_dbus_emit_signal(dev->conn, dev->path,
					AUDIO_SINK_INTERFACE, "Disconnected",
					DBUS_TYPE_INVALID);
			emit_property_changed(dev->conn, dev->path,
					AUDIO_SINK_INTERFACE, "Connected",
					DBUS_TYPE_BOOLEAN, &value);
			if (sink->dc_id) {
				device_remove_disconnect_watch(dev->btd_dev,
								sink->dc_id);
				sink->dc_id = 0;
			}
		}
		sink_set_state(dev, SINK_STATE_DISCONNECTED);
		break;
	case AVDTP_SESSION_STATE_CONNECTING:
		sink_set_state(dev, SINK_STATE_CONNECTING);
		break;
	case AVDTP_SESSION_STATE_CONNECTED:
		break;
	}

	sink->session_state = new_state;
}

static void pending_request_free(struct audio_device *dev,
					struct pending_request *pending)
{
	if (pending->conn)
		dbus_connection_unref(pending->conn);
	if (pending->msg)
		dbus_message_unref(pending->msg);
	if (pending->id)
		a2dp_cancel(dev, pending->id);

	g_free(pending);
}

static void disconnect_cb(struct btd_device *btd_dev, gboolean removal,
				void *user_data)
{
	struct audio_device *device = user_data;
	struct sink *sink = device->sink;

	debug("Sink: disconnect %s", device->path);

	avdtp_close(sink->session, sink->stream);
}

static void stream_state_changed(struct avdtp_stream *stream,
					avdtp_state_t old_state,
					avdtp_state_t new_state,
					struct avdtp_error *err,
					void *user_data)
{
	struct audio_device *dev = user_data;
	struct sink *sink = dev->sink;
	gboolean value;

	if (err)
		return;

	switch (new_state) {
	case AVDTP_STATE_IDLE:
		if (sink->disconnect) {
			DBusMessage *reply;
			struct pending_request *p;

			p = sink->disconnect;
			sink->disconnect = NULL;

			reply = dbus_message_new_method_return(p->msg);
			g_dbus_send_message(p->conn, reply);
			pending_request_free(dev, p);
		}

		if (sink->dc_id) {
			device_remove_disconnect_watch(dev->btd_dev,
							sink->dc_id);
			sink->dc_id = 0;
		}

		if (sink->session) {
			avdtp_unref(sink->session);
			sink->session = NULL;
		}
		sink->stream = NULL;
		sink->cb_id = 0;
		break;
	case AVDTP_STATE_OPEN:
		if (old_state == AVDTP_STATE_CONFIGURED &&
				sink->state == SINK_STATE_CONNECTING) {
			value = TRUE;
			g_dbus_emit_signal(dev->conn, dev->path,
						AUDIO_SINK_INTERFACE,
						"Connected",
						DBUS_TYPE_INVALID);
			emit_property_changed(dev->conn, dev->path,
						AUDIO_SINK_INTERFACE,
						"Connected",
						DBUS_TYPE_BOOLEAN, &value);
			sink->dc_id = device_add_disconnect_watch(dev->btd_dev,
								disconnect_cb,
								dev, NULL);
		} else if (old_state == AVDTP_STATE_STREAMING) {
			value = FALSE;
			g_dbus_emit_signal(dev->conn, dev->path,
						AUDIO_SINK_INTERFACE,
						"Stopped",
						DBUS_TYPE_INVALID);
			emit_property_changed(dev->conn, dev->path,
						AUDIO_SINK_INTERFACE,
						"Playing",
						DBUS_TYPE_BOOLEAN, &value);
		}
		sink_set_state(dev, SINK_STATE_CONNECTED);
		break;
	case AVDTP_STATE_STREAMING:
		value = TRUE;
		g_dbus_emit_signal(dev->conn, dev->path, AUDIO_SINK_INTERFACE,
					"Playing", DBUS_TYPE_INVALID);
		emit_property_changed(dev->conn, dev->path,
					AUDIO_SINK_INTERFACE, "Playing",
					DBUS_TYPE_BOOLEAN, &value);
		sink_set_state(dev, SINK_STATE_PLAYING);
		break;
	case AVDTP_STATE_CONFIGURED:
	case AVDTP_STATE_CLOSING:
	case AVDTP_STATE_ABORTING:
	default:
		break;
	}

	sink->stream_state = new_state;
}

static DBusHandlerResult error_failed(DBusConnection *conn,
					DBusMessage *msg, const char * desc)
{
	return error_common_reply(conn, msg, ERROR_INTERFACE ".Failed", desc);
}

static gboolean stream_setup_retry(gpointer user_data)
{
	struct sink *sink = user_data;
	struct pending_request *pending = sink->connect;

	sink->retry_id = 0;

	if (sink->stream_state >= AVDTP_STATE_OPEN) {
		debug("Stream successfully created, after XCASE connect:connect");
		if (pending->msg) {
			DBusMessage *reply;
			reply = dbus_message_new_method_return(pending->msg);
			g_dbus_send_message(pending->conn, reply);
		}
	} else {
		debug("Stream setup failed, after XCASE connect:connect");
		if (pending->msg)
			error_failed(pending->conn, pending->msg, "Stream setup failed");
	}

	sink->connect = NULL;
	pending_request_free(sink->dev, pending);

	return FALSE;
}

static void stream_setup_complete(struct avdtp *session, struct a2dp_sep *sep,
					struct avdtp_stream *stream,
					struct avdtp_error *err, void *user_data)
{
	struct sink *sink = user_data;
	struct pending_request *pending;

	pending = sink->connect;

	pending->id = 0;

	if (stream) {
		debug("Stream successfully created");

		if (pending->msg) {
			DBusMessage *reply;
			reply = dbus_message_new_method_return(pending->msg);
			g_dbus_send_message(pending->conn, reply);
		}

		sink->connect = NULL;
		pending_request_free(sink->dev, pending);

		return;
	}

	avdtp_unref(sink->session);
	sink->session = NULL;
	if (avdtp_error_type(err) == AVDTP_ERROR_ERRNO
			&& avdtp_error_posix_errno(err) != EHOSTDOWN) {
		debug("connect:connect XCASE detected");
		sink->retry_id = g_timeout_add_seconds(STREAM_SETUP_RETRY_TIMER,
							stream_setup_retry,
							sink);
	} else {
		if (pending->msg)
			error_failed(pending->conn, pending->msg, "Stream setup failed");
		sink->connect = NULL;
		pending_request_free(sink->dev, pending);
		debug("Stream setup failed : %s", avdtp_strerror(err));
	}
}

static uint8_t default_bitpool(uint8_t freq, uint8_t mode)
{
	switch (freq) {
	case SBC_SAMPLING_FREQ_16000:
	case SBC_SAMPLING_FREQ_32000:
		return 53;
	case SBC_SAMPLING_FREQ_44100:
		switch (mode) {
		case SBC_CHANNEL_MODE_MONO:
		case SBC_CHANNEL_MODE_DUAL_CHANNEL:
			return 31;
		case SBC_CHANNEL_MODE_STEREO:
		case SBC_CHANNEL_MODE_JOINT_STEREO:
			return 53;
		default:
			error("Invalid channel mode %u", mode);
			return 53;
		}
	case SBC_SAMPLING_FREQ_48000:
		switch (mode) {
		case SBC_CHANNEL_MODE_MONO:
		case SBC_CHANNEL_MODE_DUAL_CHANNEL:
			return 29;
		case SBC_CHANNEL_MODE_STEREO:
		case SBC_CHANNEL_MODE_JOINT_STEREO:
			return 51;
		default:
			error("Invalid channel mode %u", mode);
			return 51;
		}
	default:
		error("Invalid sampling freq %u", freq);
		return 53;
	}
}

static gboolean select_sbc_params(struct sbc_codec_cap *cap,
					struct sbc_codec_cap *supported)
{
	unsigned int max_bitpool, min_bitpool;

	memset(cap, 0, sizeof(struct sbc_codec_cap));

	cap->cap.media_type = AVDTP_MEDIA_TYPE_AUDIO;
	cap->cap.media_codec_type = A2DP_CODEC_SBC;

	if (supported->frequency & SBC_SAMPLING_FREQ_44100)
		cap->frequency = SBC_SAMPLING_FREQ_44100;
	else if (supported->frequency & SBC_SAMPLING_FREQ_48000)
		cap->frequency = SBC_SAMPLING_FREQ_48000;
	else if (supported->frequency & SBC_SAMPLING_FREQ_32000)
		cap->frequency = SBC_SAMPLING_FREQ_32000;
	else if (supported->frequency & SBC_SAMPLING_FREQ_16000)
		cap->frequency = SBC_SAMPLING_FREQ_16000;
	else {
		error("No supported frequencies");
		return FALSE;
	}

	if (supported->channel_mode & SBC_CHANNEL_MODE_JOINT_STEREO)
		cap->channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
	else if (supported->channel_mode & SBC_CHANNEL_MODE_STEREO)
		cap->channel_mode = SBC_CHANNEL_MODE_STEREO;
	else if (supported->channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
		cap->channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
	else if (supported->channel_mode & SBC_CHANNEL_MODE_MONO)
		cap->channel_mode = SBC_CHANNEL_MODE_MONO;
	else {
		error("No supported channel modes");
		return FALSE;
	}

	if (supported->block_length & SBC_BLOCK_LENGTH_16)
		cap->block_length = SBC_BLOCK_LENGTH_16;
	else if (supported->block_length & SBC_BLOCK_LENGTH_12)
		cap->block_length = SBC_BLOCK_LENGTH_12;
	else if (supported->block_length & SBC_BLOCK_LENGTH_8)
		cap->block_length = SBC_BLOCK_LENGTH_8;
	else if (supported->block_length & SBC_BLOCK_LENGTH_4)
		cap->block_length = SBC_BLOCK_LENGTH_4;
	else {
		error("No supported block lengths");
		return FALSE;
	}

	if (supported->subbands & SBC_SUBBANDS_8)
		cap->subbands = SBC_SUBBANDS_8;
	else if (supported->subbands & SBC_SUBBANDS_4)
		cap->subbands = SBC_SUBBANDS_4;
	else {
		error("No supported subbands");
		return FALSE;
	}

	if (supported->allocation_method & SBC_ALLOCATION_LOUDNESS)
		cap->allocation_method = SBC_ALLOCATION_LOUDNESS;
	else if (supported->allocation_method & SBC_ALLOCATION_SNR)
		cap->allocation_method = SBC_ALLOCATION_SNR;

	min_bitpool = MAX(MIN_BITPOOL, supported->min_bitpool);
	max_bitpool = MIN(default_bitpool(cap->frequency, cap->channel_mode),
							supported->max_bitpool);

	cap->min_bitpool = min_bitpool;
	cap->max_bitpool = max_bitpool;

	return TRUE;
}

static gboolean select_capabilities(struct avdtp *session,
					struct avdtp_remote_sep *rsep,
					GSList **caps)
{
	struct avdtp_service_capability *media_transport, *media_codec;
	struct sbc_codec_cap sbc_cap;

	media_codec = avdtp_get_codec(rsep);
	if (!media_codec)
		return FALSE;

	select_sbc_params(&sbc_cap, (struct sbc_codec_cap *) media_codec->data);

	media_transport = avdtp_service_cap_new(AVDTP_MEDIA_TRANSPORT,
						NULL, 0);

	*caps = g_slist_append(*caps, media_transport);

	media_codec = avdtp_service_cap_new(AVDTP_MEDIA_CODEC, &sbc_cap,
						sizeof(sbc_cap));

	*caps = g_slist_append(*caps, media_codec);

	if (avdtp_get_delay_reporting(rsep)) {
		struct avdtp_service_capability *delay_reporting;
		delay_reporting = avdtp_service_cap_new(AVDTP_DELAY_REPORTING,
								NULL, 0);
		*caps = g_slist_append(*caps, delay_reporting);
	}

	return TRUE;
}

static void discovery_complete(struct avdtp *session, GSList *seps, struct avdtp_error *err,
				void *user_data)
{
	struct sink *sink = user_data;
	struct pending_request *pending;
	struct avdtp_local_sep *lsep;
	struct avdtp_remote_sep *rsep;
	struct a2dp_sep *sep;
	GSList *caps = NULL;
	int id;

	pending = sink->connect;

	if (err) {
		avdtp_unref(sink->session);
		sink->session = NULL;
		if (avdtp_error_type(err) == AVDTP_ERROR_ERRNO
				&& avdtp_error_posix_errno(err) != EHOSTDOWN) {
			debug("connect:connect XCASE detected");
			sink->retry_id =
				g_timeout_add_seconds(STREAM_SETUP_RETRY_TIMER,
							stream_setup_retry,
							sink);
		} else
			goto failed;
		return;
	}

	debug("Discovery complete");

	if (avdtp_get_seps(session, AVDTP_SEP_TYPE_SINK, AVDTP_MEDIA_TYPE_AUDIO,
				A2DP_CODEC_SBC, &lsep, &rsep) < 0) {
		error("No matching ACP and INT SEPs found");
		goto failed;
	}

	if (!select_capabilities(session, rsep, &caps)) {
		error("Unable to select remote SEP capabilities");
		goto failed;
	}

	sep = a2dp_get(session, rsep);
	if (!sep) {
		error("Unable to get a local source SEP");
		goto failed;
	}

	id = a2dp_config(sink->session, sep, stream_setup_complete, caps, sink);
	if (id == 0)
		goto failed;

	pending->id = id;
	return;

failed:
	if (pending->msg)
		error_failed(pending->conn, pending->msg, "Stream setup failed");
	pending_request_free(sink->dev, pending);
	sink->connect = NULL;
	avdtp_unref(sink->session);
	sink->session = NULL;
}

gboolean sink_setup_stream(struct sink *sink, struct avdtp *session)
{
	if (sink->connect || sink->disconnect)
		return FALSE;

	if (session && !sink->session)
		sink->session = avdtp_ref(session);

	if (!sink->session)
		return FALSE;

	avdtp_set_auto_disconnect(sink->session, FALSE);

	if (avdtp_discover(sink->session, discovery_complete, sink) < 0)
		return FALSE;

	sink->connect = g_new0(struct pending_request, 1);

	return TRUE;
}

static DBusMessage *sink_connect(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct audio_device *dev = data;
	struct sink *sink = dev->sink;
	struct pending_request *pending;

	if (!sink->session)
		sink->session = avdtp_get(&dev->src, &dev->dst);

	if (!sink->session)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
						"Unable to get a session");

	if (sink->connect || sink->disconnect)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
						"%s", strerror(EBUSY));

	if (sink->stream_state >= AVDTP_STATE_OPEN)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".AlreadyConnected",
						"Device Already Connected");

	if (!sink_setup_stream(sink, NULL))
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
						"Failed to create a stream");

	dev->auto_connect = FALSE;

	pending = sink->connect;

	pending->conn = dbus_connection_ref(conn);
	pending->msg = dbus_message_ref(msg);

	debug("stream creation in progress");

	return NULL;
}

static DBusMessage *sink_disconnect(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct audio_device *device = data;
	struct sink *sink = device->sink;
	struct pending_request *pending;
	int err;

	if (!sink->session)
		return g_dbus_create_error(msg, ERROR_INTERFACE
						".NotConnected",
						"Device not Connected");

	if (sink->connect || sink->disconnect)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
						"%s", strerror(EBUSY));

	if (sink->stream_state < AVDTP_STATE_OPEN) {
		DBusMessage *reply = dbus_message_new_method_return(msg);
		if (!reply)
			return NULL;
		avdtp_unref(sink->session);
		sink->session = NULL;
		return reply;
	}

	err = avdtp_close(sink->session, sink->stream);
	if (err < 0)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
						"%s", strerror(-err));

	pending = g_new0(struct pending_request, 1);
	pending->conn = dbus_connection_ref(conn);
	pending->msg = dbus_message_ref(msg);
	sink->disconnect = pending;

	return NULL;
}

static DBusMessage *sink_is_connected(DBusConnection *conn,
					DBusMessage *msg,
					void *data)
{
	struct audio_device *device = data;
	struct sink *sink = device->sink;
	DBusMessage *reply;
	dbus_bool_t connected;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	connected = (sink->stream_state >= AVDTP_STATE_CONFIGURED);

	dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &connected,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage *sink_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct audio_device *device = data;
	struct sink *sink = device->sink;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *state;
	gboolean value;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	/* Playing */
	value = (sink->stream_state == AVDTP_STATE_STREAMING);
	dict_append_entry(&dict, "Playing", DBUS_TYPE_BOOLEAN, &value);

	/* Connected */
	value = (sink->stream_state >= AVDTP_STATE_CONFIGURED);
	dict_append_entry(&dict, "Connected", DBUS_TYPE_BOOLEAN, &value);

	/* State */
	state = state2str(sink->state);
	if (state)
		dict_append_entry(&dict, "State", DBUS_TYPE_STRING, &state);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static GDBusMethodTable sink_methods[] = {
	{ "Connect",		"",	"",	sink_connect,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "Disconnect",		"",	"",	sink_disconnect,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "IsConnected",	"",	"b",	sink_is_connected,
						G_DBUS_METHOD_FLAG_DEPRECATED },
	{ "GetProperties",	"",	"a{sv}",sink_get_properties },
	{ NULL, NULL, NULL, NULL }
};

static GDBusSignalTable sink_signals[] = {
	{ "Connected",			"",	G_DBUS_SIGNAL_FLAG_DEPRECATED },
	{ "Disconnected",		"",	G_DBUS_SIGNAL_FLAG_DEPRECATED },
	{ "Playing",			"",	G_DBUS_SIGNAL_FLAG_DEPRECATED },
	{ "Stopped",			"",	G_DBUS_SIGNAL_FLAG_DEPRECATED },
	{ "PropertyChanged",		"sv"	},
	{ NULL, NULL }
};

static void sink_free(struct audio_device *dev)
{
	struct sink *sink = dev->sink;

	if (sink->cb_id)
		avdtp_stream_remove_cb(sink->session, sink->stream,
					sink->cb_id);

	if (sink->dc_id)
		device_remove_disconnect_watch(dev->btd_dev, sink->dc_id);

	if (sink->session)
		avdtp_unref(sink->session);

	if (sink->connect)
		pending_request_free(dev, sink->connect);

	if (sink->disconnect)
		pending_request_free(dev, sink->disconnect);

	if (sink->retry_id)
		g_source_remove(sink->retry_id);

	g_free(sink);
	dev->sink = NULL;
}

static void path_unregister(void *data)
{
	struct audio_device *dev = data;

	debug("Unregistered interface %s on path %s",
		AUDIO_SINK_INTERFACE, dev->path);

	sink_free(dev);
}

void sink_unregister(struct audio_device *dev)
{
	g_dbus_unregister_interface(dev->conn, dev->path,
		AUDIO_SINK_INTERFACE);
}

struct sink *sink_init(struct audio_device *dev)
{
	struct sink *sink;

	if (!g_dbus_register_interface(dev->conn, dev->path,
					AUDIO_SINK_INTERFACE,
					sink_methods, sink_signals, NULL,
					dev, path_unregister))
		return NULL;

	debug("Registered interface %s on path %s",
		AUDIO_SINK_INTERFACE, dev->path);

	if (avdtp_callback_id == 0)
		avdtp_callback_id = avdtp_add_state_cb(avdtp_state_callback,
									NULL);

	sink = g_new0(struct sink, 1);

	sink->dev = dev;

	return sink;
}

gboolean sink_is_active(struct audio_device *dev)
{
	struct sink *sink = dev->sink;

	if (sink->session)
		return TRUE;

	return FALSE;
}

avdtp_state_t sink_get_state(struct audio_device *dev)
{
	struct sink *sink = dev->sink;

	return sink->stream_state;
}

gboolean sink_new_stream(struct audio_device *dev, struct avdtp *session,
				struct avdtp_stream *stream)
{
	struct sink *sink = dev->sink;

	if (sink->stream)
		return FALSE;

	if (!sink->session)
		sink->session = avdtp_ref(session);

	sink->stream = stream;

	sink->cb_id = avdtp_stream_add_cb(session, stream,
						stream_state_changed, dev);

	return TRUE;
}

gboolean sink_shutdown(struct sink *sink)
{
	if (!sink->stream)
		return FALSE;

	if (avdtp_close(sink->session, sink->stream) < 0)
		return FALSE;

	return TRUE;
}

unsigned int sink_add_state_cb(sink_state_cb cb, void *user_data)
{
	struct sink_state_callback *state_cb;
	static unsigned int id = 0;

	state_cb = g_new(struct sink_state_callback, 1);
	state_cb->cb = cb;
	state_cb->user_data = user_data;
	state_cb->id = ++id;

	sink_callbacks = g_slist_append(sink_callbacks, state_cb);

	return state_cb->id;
}

gboolean sink_remove_state_cb(unsigned int id)
{
	GSList *l;

	for (l = sink_callbacks; l != NULL; l = l->next) {
		struct sink_state_callback *cb = l->data;
		if (cb && cb->id == id) {
			sink_callbacks = g_slist_remove(sink_callbacks, cb);
			g_free(cb);
			return TRUE;
		}
	}

	return FALSE;
}
