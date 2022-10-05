/* PipeWire
 *
 * Copyright © 2022 Wim Taymans <wim.taymans@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ctype.h>

#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/json.h>
#include <spa/debug/pod.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>

#include <module-rtp/sap.h>
#include <module-rtp/rtp.h>


/** \page page_module_rtp_sink PipeWire Module: RTP sink
 *
 * The `rtp-sink` module creates a PipeWire sink that sends audio
 * RTP packets.
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `stream.props = {}`: properties to be passed to the stream
 * - `sap.ip = <str>`: IP address of the SAP messages
 * - `sap.port = <str>`: port of the SAP messages
 * - `local.ifname = <str>`: interface name to use
 * - `sess.latency.msec = <str>`: target network latency in milliseconds
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_MEDIA_NAME
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.modules = [
 *  {   name = libpipewire-module-rtp-sink
 *      args = {
 *          #sap.ip = 224.0.0.56
 *          #sap.port = 9875
 *          #local.ifname = eth0
 *          sess.latency.msec = 200
 *          stream.props = {
 *             node.name = "rtp-sink"
 *          }
 *      }
 *  }
 *]
 *\endcode
 *
 */

#define NAME "rtp-sink"

static const struct spa_dict_item module_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "RTP Source" },
	{ PW_KEY_MODULE_USAGE,	"sap.ip=<SAP IP address to listen on> "
				"sap.port=<SAP port to listen on> "
				"source.ip=<source IP address> "
				"destination.ip=<destination IP address> "
				"local.ifname=<local interface name to use> "
				"sess.latency.msec=<target network latency in milliseconds> "
				"stream.props= { key=value ... }" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};


PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define SAP_INTERVAL_SEC	5

#define BUFFER_SIZE		(1u<<16)
#define BUFFER_MASK		(BUFFER_SIZE-1)

#define DEFAULT_SAP_IP		"224.0.0.56"
#define DEFAULT_SAP_PORT	9875

#define DEFAULT_FORMAT		"S16BE"
#define DEFAULT_RATE		44100
#define DEFAULT_CHANNELS	2
#define DEFAULT_POSITION	"[ FL FR ]"
#define DEFAULT_PORT		46000
#define DEFAULT_SOURCE_IP	"0.0.0.0"
#define DEFAULT_DESTINATION_IP	"224.0.0.56"
#define DEFAULT_TTL		1
#define DEFAULT_MTU		1280

struct impl {
	struct pw_impl_module *module;
	struct spa_hook module_listener;
	struct pw_properties *props;
	struct pw_context *module_context;

	struct pw_loop *loop;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;

	struct spa_source *timer;

	struct pw_properties *stream_props;
	struct pw_stream *stream;
	struct spa_hook stream_listener;

	unsigned int do_disconnect:1;

	char *ifname;
	char *session_name;
	int sess_latency_msec;
	bool mcast_loop;
	bool ttl;
	int mtu;

	struct sockaddr_storage src_addr;
	socklen_t src_len;

	uint16_t port;
	struct sockaddr_storage dst_addr;
	socklen_t dst_len;

	uint16_t sap_port;
	struct sockaddr_storage sap_addr;
	socklen_t sap_len;

	struct spa_audio_info_raw info;
	uint32_t frame_size;
	int payload;
	uint16_t seq;
	uint32_t timestamp;
	uint32_t ssrc;

	struct spa_ringbuffer ring;
	uint8_t buffer[BUFFER_SIZE];

	int rtp_fd;
};

static void stream_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->stream_listener);
	impl->stream = NULL;
}

static inline void
set_iovec(struct spa_ringbuffer *rbuf, void *buffer, uint32_t size,
		uint32_t offset, struct iovec *iov, uint32_t len)
{
	iov[0].iov_len = SPA_MIN(len, size - offset);
	iov[0].iov_base = SPA_PTROFF(buffer, offset, void);
	iov[1].iov_len = len - iov[0].iov_len;
	iov[1].iov_base = buffer;
}

static void flush_packets(struct impl *impl)
{
	int32_t avail;
	uint32_t index;
	struct iovec iov[3];
	struct msghdr msg;
	ssize_t n;
	struct rtp_header header;

	spa_zero(header);
	header.v = 2;
	header.pt = impl->payload;
	header.ssrc = htonl(impl->ssrc);

	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = iov;
	msg.msg_iovlen = 3;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;

	avail = spa_ringbuffer_get_read_index(&impl->ring, &index);

	while (avail >= impl->mtu) {
		header.sequence_number = htons(impl->seq);
		header.timestamp = htonl(impl->timestamp);

		set_iovec(&impl->ring,
			impl->buffer, BUFFER_SIZE,
			index % BUFFER_MASK,
			&iov[1], impl->mtu);

		n = sendmsg(impl->rtp_fd, &msg, MSG_NOSIGNAL);
		if (n < 0)
			pw_log_warn("sendmsg() failed: %m");

		impl->seq++;
		impl->timestamp += impl->mtu / impl->frame_size;

		index += impl->mtu;
		avail -= impl->mtu;
	}
	spa_ringbuffer_read_update(&impl->ring, index);
}

static void stream_process(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t index;
        int32_t filled, wanted;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	wanted = d[0].chunk->size;

	filled = spa_ringbuffer_get_write_index(&impl->ring, &index);

	if (filled > (int32_t)BUFFER_SIZE) {
		pw_log_warn("overrun %u > %u", filled, BUFFER_SIZE);
	} else {
		spa_ringbuffer_write_data(&impl->ring,
				impl->buffer,
				BUFFER_SIZE,
                                index & BUFFER_MASK,
                                d[0].data, wanted);

                index += wanted;
                spa_ringbuffer_write_update(&impl->ring, index);
        }
	pw_stream_queue_buffer(impl->stream, buf);

	flush_packets(impl);
}

static void on_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = d;

	switch (state) {
	case PW_STREAM_STATE_UNCONNECTED:
		pw_log_info("stream disconnected, unloading");
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_ERROR:
		pw_log_error("stream error: %s", error);
		break;
	default:
		break;
	}
}

static const struct pw_stream_events in_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = on_stream_state_changed,
	.process = stream_process
};

static int parse_address(const char *address, uint16_t port,
		struct sockaddr_storage *addr, socklen_t *len)
{
	struct sockaddr_in *sa4 = (struct sockaddr_in*)addr;
	struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)addr;

	if (inet_pton(AF_INET, address, &sa4->sin_addr) > 0) {
		sa4->sin_family = AF_INET;
		sa4->sin_port = htons(port);
		*len = sizeof(*sa4);
	} else if (inet_pton(AF_INET6, address, &sa6->sin6_addr) > 0) {
		sa6->sin6_family = AF_INET6;
		sa6->sin6_port = htons(port);
		*len = sizeof(*sa6);
	} else
		return -EINVAL;

	return 0;
}

static int make_multicast_socket(struct sockaddr_storage *src, socklen_t src_len,
		struct sockaddr_storage *dst, socklen_t dst_len,
		bool loop, int ttl)
{
	int af, fd, val, res;

	af = src->ss_family;
	if ((fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		pw_log_error("socket failed: %m");
		return -errno;
	}
	if (bind(fd, (struct sockaddr*)src, src_len) < 0) {
		res = -errno;
		pw_log_error("bind() failed: %m");
		goto error;
	}
	if (connect(fd, (struct sockaddr*)dst, dst_len) < 0) {
		res = -errno;
		pw_log_error("connect() failed: %m");
		goto error;
	}
	val = loop;
	if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof(val)) < 0)
		pw_log_warn("setsockopt(IP_MULTICAST_LOOP) failed: %m");

	val = ttl;
	if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &val, sizeof(val)) < 0)
		pw_log_warn("setsockopt(IP_MULTICAST_TTL) failed: %m");

	return fd;
error:
	close(fd);
	return res;
}

static int setup_stream(struct impl *impl)
{
	const struct spa_pod *params[1];
	struct spa_pod_builder b;
	uint32_t n_params;
	uint8_t buffer[1024];
	struct pw_properties *props;
	int res, fd;

	props = pw_properties_copy(impl->stream_props);
	if (props == NULL)
		return -errno;

	pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", impl->info.rate);

	impl->stream = pw_stream_new(impl->core,
			"rtp-sink capture", props);
	if (impl->stream == NULL)
		return -errno;

	pw_stream_add_listener(impl->stream,
			&impl->stream_listener,
			&in_stream_events, impl);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&impl->info);

	if ((res = pw_stream_connect(impl->stream,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;


	if ((fd = make_multicast_socket(&impl->src_addr, impl->src_len,
					&impl->dst_addr, impl->dst_len,
					impl->mcast_loop, impl->ttl)) < 0)
		return fd;

	impl->rtp_fd = fd;

	return 0;
}

static int get_ip(const struct sockaddr_storage *sa, char *ip, size_t len)
{
	if (sa->ss_family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in*)sa;
		inet_ntop(sa->ss_family, &in->sin_addr, ip, len);
	} else if (sa->ss_family == AF_INET6) {
		struct sockaddr_in6 *in = (struct sockaddr_in6*)sa;
		inet_ntop(sa->ss_family, &in->sin6_addr, ip, len);
	} else
		return -EIO;
	return 0;
}
static void on_timer_event(void *data, uint64_t expirations)
{
	struct impl *impl = data;
	char buffer[1024], src_addr[64], dst_addr[64];
	const char *user_name = "-", *af, *fmt;
	uint32_t ntp;

	af = impl->src_addr.ss_family == AF_INET ? "IP4" : "IP6";
	ntp = (uint32_t) time(NULL) + 2208988800U;

	get_ip(&impl->src_addr, src_addr, sizeof(src_addr));
	get_ip(&impl->dst_addr, dst_addr, sizeof(dst_addr));

	fmt = "L16";

	snprintf(buffer, sizeof(buffer),
			"v=0\n"
			"o=%s %u 0 IN %s %s\n"
			"s=%s\n"
			"c=IN %s %s\n"
			"t=%u 0\n"
			"a=recvonly\n"
			"m=audio %u RTP/AVP %i\n"
			"a=rtpmap:%i %s/%u/%u\n"
			"a=type:broadcast\n",
			user_name, ntp, af, src_addr,
			impl->session_name,
			af, dst_addr,
			ntp,
			impl->port, impl->payload,
			impl->payload, fmt, impl->info.rate, impl->info.channels);

	pw_log_info("%s", buffer);
}

static int start_sap_announce(struct impl *impl)
{
	int fd, res;
	struct timespec value, interval;

	if ((fd = make_multicast_socket(&impl->src_addr, impl->src_len,
					&impl->sap_addr, impl->sap_len,
					impl->mcast_loop, impl->ttl)) < 0)
		return fd;

	pw_log_info("starting SAP timer");
	impl->timer = pw_loop_add_timer(impl->loop, on_timer_event, impl);
	if (impl->timer == NULL) {
		res = -errno;
		pw_log_error("can't create timer source: %m");
		goto error;
	}
	value.tv_sec = 0;
	value.tv_nsec = 1;
	interval.tv_sec = SAP_INTERVAL_SEC;
	interval.tv_nsec = 0;
	pw_loop_update_timer(impl->loop, impl->timer, &value, &interval, false);

	return 0;
error:
	close(fd);
	return res;

}

static void core_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->core_listener);
	impl->core = NULL;
	pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
	if (impl->stream)
		pw_stream_destroy(impl->stream);

	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	if (impl->timer)
		pw_loop_destroy_source(impl->loop, impl->timer);

	if (impl->rtp_fd != -1)
		close(impl->rtp_fd);

	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->props);

	free(impl->ifname);
	free(impl);
}

static void module_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void on_core_error(void *d, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = d;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static inline uint32_t format_from_name(const char *name, size_t len)
{
	int i;
	for (i = 0; spa_type_audio_format[i].name; i++) {
		if (strncmp(name, spa_debug_type_short_name(spa_type_audio_format[i].name), len) == 0)
			return spa_type_audio_format[i].type;
	}
	return SPA_AUDIO_FORMAT_UNKNOWN;
}

static uint32_t channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)))
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static void parse_position(struct spa_audio_info_raw *info, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	info->channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    info->channels < SPA_AUDIO_MAX_CHANNELS) {
		info->position[info->channels++] = channel_from_name(v);
	}
}

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	spa_zero(*info);
	if ((str = pw_properties_get(props, PW_KEY_AUDIO_FORMAT)) == NULL)
		str = DEFAULT_FORMAT;
	info->format = format_from_name(str, strlen(str));

	info->rate = pw_properties_get_uint32(props, PW_KEY_AUDIO_RATE, info->rate);
	if (info->rate == 0)
		info->rate = DEFAULT_RATE;

	info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, info->channels);
	info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
	if (info->channels == 0)
		parse_position(info, DEFAULT_POSITION, strlen(DEFAULT_POSITION));
}

static int calc_frame_size(struct spa_audio_info_raw *info)
{
	int res = info->channels;
	switch (info->format) {
	case SPA_AUDIO_FORMAT_U8:
	case SPA_AUDIO_FORMAT_S8:
	case SPA_AUDIO_FORMAT_ALAW:
	case SPA_AUDIO_FORMAT_ULAW:
		return res;
	case SPA_AUDIO_FORMAT_S16:
	case SPA_AUDIO_FORMAT_S16_OE:
	case SPA_AUDIO_FORMAT_U16:
		return res * 2;
	case SPA_AUDIO_FORMAT_S24:
	case SPA_AUDIO_FORMAT_S24_OE:
	case SPA_AUDIO_FORMAT_U24:
		return res * 3;
	case SPA_AUDIO_FORMAT_S24_32:
	case SPA_AUDIO_FORMAT_S24_32_OE:
	case SPA_AUDIO_FORMAT_S32:
	case SPA_AUDIO_FORMAT_S32_OE:
	case SPA_AUDIO_FORMAT_U32:
	case SPA_AUDIO_FORMAT_U32_OE:
	case SPA_AUDIO_FORMAT_F32:
	case SPA_AUDIO_FORMAT_F32_OE:
		return res * 4;
	case SPA_AUDIO_FORMAT_F64:
	case SPA_AUDIO_FORMAT_F64_OE:
		return res * 8;
	default:
		return 0;
	}
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->stream_props, key) == NULL)
			pw_properties_set(impl->stream_props, key, str);
	}
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct pw_properties *props = NULL, *stream_props = NULL;
	uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
	uint32_t pid = getpid(), port;
	const char *str;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->rtp_fd = -1;

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}
	impl->props = props;

	stream_props = pw_properties_new(NULL, NULL);
	if (stream_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}
	impl->stream_props = stream_props;

	impl->module = module;
	impl->module_context = context;
	impl->loop = pw_context_get_main_loop(context);

	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(stream_props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(stream_props, PW_KEY_NODE_NETWORK, "true");

//	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
//		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "rtp-sink-%u-%u", pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(props, PW_KEY_NODE_DESCRIPTION,
				pw_properties_get(props, PW_KEY_NODE_NAME));

	if ((str = pw_properties_get(props, "stream.props")) != NULL)
		pw_properties_update_string(stream_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_FORMAT);
	copy_props(impl, props, PW_KEY_AUDIO_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_NAME);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_MEDIA_CLASS);

	parse_audio_info(impl->stream_props, &impl->info);

	impl->frame_size = calc_frame_size(&impl->info);
	if (impl->frame_size == 0) {
		pw_log_error("unsupported audio format:%d channels:%d",
				impl->info.format, impl->info.channels);
		res = -EINVAL;
		goto out;
	}
	impl->payload = 127;
	impl->mtu = DEFAULT_MTU;
	impl->ttl = DEFAULT_TTL;
	impl->ssrc = rand();
	impl->timestamp = rand();
	impl->seq = rand();

	str = pw_properties_get(props, "local.ifname");
	impl->ifname = str ? strdup(str) : NULL;

	if ((str = pw_properties_get(props, "sap.ip")) == NULL)
		str = DEFAULT_SAP_IP;
	port = pw_properties_get_uint32(props, "sap.port", DEFAULT_SAP_PORT);

	if ((res = parse_address(str, port, &impl->sap_addr, &impl->sap_len)) < 0) {
		pw_log_error("invalid sap.ip %s: %s", str, spa_strerror(res));
		goto out;
	}

	if ((str = pw_properties_get(props, "source.ip")) == NULL)
		str = DEFAULT_SOURCE_IP;
	if ((res = parse_address(str, 0, &impl->src_addr, &impl->src_len)) < 0) {
		pw_log_error("invalid source.ip %s: %s", str, spa_strerror(res));
		goto out;
	}

	impl->port = DEFAULT_PORT + ((uint32_t) (rand() % 512) << 1);
	impl->port = pw_properties_get_uint32(props, "destination.port", impl->port);
	if ((str = pw_properties_get(props, "destination.ip")) == NULL)
		str = DEFAULT_DESTINATION_IP;
	if ((res = parse_address(str, impl->port, &impl->dst_addr, &impl->dst_len)) < 0) {
		pw_log_error("invalid destination.ip %s: %s", str, spa_strerror(res));
		goto out;
	}

	impl->core = pw_context_get_object(impl->module_context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(impl->module_context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		impl->do_disconnect = true;
	}
	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto out;
	}

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	if ((res = setup_stream(impl)) < 0)
		goto out;

	if ((res = start_sap_announce(impl)) < 0)
		goto out;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_info));

	pw_log_info("Successfully loaded module-rtp-sink");

	return 0;
out:
	impl_destroy(impl);
	return res;
}