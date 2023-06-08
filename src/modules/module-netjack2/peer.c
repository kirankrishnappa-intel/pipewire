
struct volume {
	bool mute;
	uint32_t n_volumes;
	float volumes[SPA_AUDIO_MAX_CHANNELS];
};

static inline void do_volume(float *dst, const float *src, struct volume *vol, uint32_t ch, uint32_t n_samples)
{
	float v = vol->mute ? 0.0f : vol->volumes[ch];

	if (v == 0.0f || src == NULL)
		memset(dst, 0, n_samples * sizeof(float));
	else if (v == 1.0f)
		memcpy(dst, src, n_samples * sizeof(float));
	else {
		uint32_t i;
		for (i = 0; i < n_samples; i++)
			dst[i] = src[i] * v;
	}
}

struct netjack2_peer {
	int fd;

	uint32_t our_stream;
	uint32_t other_stream;
	struct nj2_session_params params;
	struct nj2_packet_header sync;
	uint32_t cycle;

	struct volume *send_volume;
	struct volume *recv_volume;

	void *buffer;
	uint32_t buffer_size;

	unsigned fix_midi:1;
};

struct data_info {
	uint32_t id;
	void *data;
	bool filled;
};

static inline void fix_midi_event(uint8_t *data, size_t size)
{
	/* fixup NoteOn with vel 0 */
	if (size > 2 && (data[0] & 0xF0) == 0x90 && data[2] == 0x00) {
		data[0] = 0x80 + (data[0] & 0x0F);
		data[2] = 0x40;
	}
}

static void midi_to_netjack2(struct netjack2_peer *peer,
		struct nj2_midi_buffer *buf, float *src, uint32_t n_samples)
{
	struct spa_pod *pod;
	struct spa_pod_sequence *seq;
	struct spa_pod_control *c;
	struct nj2_midi_event *ev;
	uint32_t free_size;

	buf->magic = MIDI_BUFFER_MAGIC;
	buf->buffer_size = 8192 * sizeof(float);
	buf->nframes = n_samples;
	buf->write_pos = 0;
	buf->event_count = 0;
	buf->lost_events = 0;

	if (src == NULL)
		return;

	if ((pod = spa_pod_from_data(src, n_samples * sizeof(float),
					0, n_samples * sizeof(float))) == NULL)
		return;
	if (!spa_pod_is_sequence(pod))
		return;

	seq = (struct spa_pod_sequence*)pod;

	free_size = buf->buffer_size - sizeof(*buf);

	SPA_POD_SEQUENCE_FOREACH(seq, c) {
		switch(c->type) {
		case SPA_CONTROL_Midi:
		{
			uint8_t *data = SPA_POD_BODY(&c->value);
			size_t size = SPA_POD_BODY_SIZE(&c->value);
			void *ptr;

			if (c->offset >= n_samples ||
			    size >= free_size) {
				buf->lost_events++;
				continue;
			}
			if (peer->fix_midi)
				fix_midi_event(data, size);

			ev = &buf->event[buf->event_count];
			ev->time = c->offset;
			ev->size = size;
			if (size <= MIDI_INLINE_MAX) {
				ptr = ev->buffer;
			} else {
				buf->write_pos += size;
				ev->offset = buf->buffer_size - 1 - buf->write_pos;
				free_size -= size;
				ptr = SPA_PTROFF(buf, ev->offset, void);
			}
			memcpy(ptr, data, size);
			buf->event_count++;
			free_size -= sizeof(*ev);
			break;
		}
		default:
			break;
		}
	}
	if (buf->write_pos > 0)
		memmove(SPA_PTROFF(buf, sizeof(*buf) + buf->event_count * sizeof(struct nj2_midi_event), void),
			SPA_PTROFF(buf, buf->buffer_size - buf->write_pos, void),
			buf->write_pos);
}

static inline void netjack2_to_midi(float *dst, uint32_t size, struct nj2_midi_buffer *buf)
{
	struct spa_pod_builder b = { 0, };
	uint32_t i;
	struct spa_pod_frame f;

	spa_pod_builder_init(&b, dst, size);
	spa_pod_builder_push_sequence(&b, &f, 0);
	for (i = 0; buf != NULL && i < buf->event_count; i++) {
		struct nj2_midi_event *ev = &buf->event[i];
		void *data;

		if (ev->size <= MIDI_INLINE_MAX)
			data = ev->buffer;
		else if (ev->offset > buf->write_pos)
			data = SPA_PTROFF(buf, ev->offset - buf->write_pos, void);
		else
			continue;

		spa_pod_builder_control(&b, ev->time, SPA_CONTROL_Midi);
		spa_pod_builder_bytes(&b, data, ev->size);
	}
	spa_pod_builder_pop(&b, &f);
}

static int netjack2_send_sync(struct netjack2_peer *peer, uint32_t nframes)
{
	struct nj2_packet_header header;
	uint8_t buffer[peer->params.mtu];
	uint32_t i, packet_size, active_ports, is_last;
	int32_t *p;

	/* we always listen on all ports */
	active_ports = peer->params.recv_audio_channels;
	packet_size = sizeof(header) + active_ports * sizeof(int32_t);
	is_last = peer->params.send_midi_channels == 0 &&
                        peer->params.send_audio_channels == 0 ? 1 : 0;

	strcpy(header.type, "header");
	header.data_type = htonl('s');
	header.data_stream = htonl(peer->our_stream);
	header.id = htonl(peer->params.id);
	header.num_packets = 0;
	header.packet_size = htonl(packet_size);
	header.active_ports = htonl(active_ports);
	header.cycle = htonl(peer->cycle);
	header.sub_cycle = 0;
	header.frames = htonl(nframes);
	header.is_last = htonl(is_last);

	memcpy(buffer, &header, sizeof(header));
	p = SPA_PTROFF(buffer, sizeof(header), int32_t);
	for (i = 0; i < active_ports; i++)
		p[i] = htonl(i);
	send(peer->fd, buffer, packet_size, 0);
	return 0;
}

static int netjack2_send_midi(struct netjack2_peer *peer, uint32_t nframes,
		struct data_info *info, uint32_t n_info)
{
	struct nj2_packet_header header;
	uint8_t buffer[peer->params.mtu];
	uint32_t i, num_packets, active_ports, data_size, res1, res2;
	uint32_t max_size;

	active_ports = peer->params.send_midi_channels;
	if (active_ports <= 0)
		return 0;

	data_size = 0;
	for (i = 0; i < active_ports; i++) {
		struct nj2_midi_buffer *mbuf;
		void *data = (i < n_info && info) ? info[i].data : NULL;

		mbuf = SPA_PTROFF(peer->buffer, data_size, struct nj2_midi_buffer);
		midi_to_netjack2(peer, mbuf, data, nframes);

		data_size += sizeof(*mbuf)
			+ mbuf->event_count * sizeof(struct nj2_midi_event)
			+ mbuf->write_pos;

		nj2_midi_buffer_hton(mbuf, mbuf);
	}

	/* Note: jack2 calculates the packet max_size and num packets with
	 * different values... */
	max_size = peer->params.mtu - sizeof(header);

	res1 = data_size % max_size;
        res2 = data_size / max_size;
        num_packets = (res1) ? res2 + 1 : res2;

	strcpy(header.type, "header");
	header.data_type = htonl('m');
	header.data_stream = htonl(peer->our_stream);
	header.id = htonl(peer->params.id);
	header.cycle = htonl(peer->cycle);
	header.active_ports = htonl(active_ports);
	header.num_packets = htonl(num_packets);
	header.frames = htonl(nframes);

	for (i = 0; i < num_packets; i++) {
		uint32_t is_last = ((i == num_packets - 1) && peer->params.send_audio_channels == 0) ? 1 : 0;
		uint32_t size = data_size - i * max_size;
		uint32_t copy_size = SPA_MIN(size, max_size);
		uint32_t packet_size = sizeof(header) + copy_size;

		header.sub_cycle = htonl(i);
		header.is_last = htonl(is_last);
                header.packet_size = htonl(packet_size);
		memcpy(buffer, &header, sizeof(header));
		memcpy(SPA_PTROFF(buffer, sizeof(header), void),
			SPA_PTROFF(peer->buffer, i * max_size, void),
			copy_size);
		send(peer->fd, buffer, packet_size, 0);
		//nj2_dump_packet_header(&header);
	}
	return 0;
}

static int netjack2_send_audio(struct netjack2_peer *peer, uint32_t nframes,
		struct data_info *info, uint32_t n_info)
{
	struct nj2_packet_header header;
	uint8_t buffer[peer->params.mtu];
	uint32_t i, j, active_ports, num_packets;
	uint32_t sub_period_size, sub_period_bytes;

	if (peer->params.send_audio_channels <= 0)
		return 0;

	active_ports = n_info;

	if (active_ports == 0) {
		sub_period_size = nframes;
	} else {
		uint32_t max_size = PACKET_AVAILABLE_SIZE(peer->params.mtu);
		uint32_t period = (uint32_t) powf(2.f, (uint32_t) (logf((float)max_size /
				(active_ports * sizeof(float))) / logf(2.f)));
		sub_period_size = SPA_MIN(period, nframes);
	}
	sub_period_bytes = sub_period_size * sizeof(float) + sizeof(int32_t);
	num_packets = nframes / sub_period_size;

	strcpy(header.type, "header");
	header.data_type = htonl('a');
	header.data_stream = htonl(peer->our_stream);
	header.id = htonl(peer->params.id);
	header.cycle = htonl(peer->cycle);
	header.active_ports = htonl(active_ports);
	header.num_packets = htonl(num_packets);
	header.frames = htonl(nframes);

	for (i = 0; i < num_packets; i++) {
		uint32_t is_last = (i == num_packets - 1) ? 1 : 0;
		uint32_t packet_size = sizeof(header) + active_ports * sub_period_bytes;
		int32_t *ap = SPA_PTROFF(buffer, sizeof(header), int32_t);
		float *src;

		for (j = 0; j < active_ports; j++) {
			ap[0] = htonl(info[j].id);

			src = SPA_PTROFF(info[j].data, i * sub_period_size * sizeof(float), float);
			do_volume((float*)&ap[1], src, peer->send_volume, info[j].id, sub_period_size);

			ap = SPA_PTROFF(ap, sub_period_bytes, int32_t);
		}
		header.sub_cycle = htonl(i);
		header.is_last = htonl(is_last);
		header.packet_size = htonl(packet_size);
		memcpy(buffer, &header, sizeof(header));
		send(peer->fd, buffer, packet_size, 0);
		//nj2_dump_packet_header(&header);
	}
	return 0;
}

static int netjack2_send_data(struct netjack2_peer *peer, uint32_t nframes,
		struct data_info *midi, uint32_t n_midi,
		struct data_info *audio, uint32_t n_audio)
{
	netjack2_send_sync(peer, nframes);
	netjack2_send_midi(peer, nframes, midi, n_midi);
	netjack2_send_audio(peer, nframes, audio, n_audio);
	return 0;
}

static inline int32_t netjack2_driver_sync_wait(struct netjack2_peer *peer)
{
	struct nj2_packet_header sync;
	ssize_t len;

	while (true) {
		if ((len = recv(peer->fd, &sync, sizeof(sync), 0)) < 0)
			goto receive_error;

		if (len >= (ssize_t)sizeof(sync)) {
			//nj2_dump_packet_header(&sync);

			if (strcmp(sync.type, "header") == 0 &&
			    ntohl(sync.data_type) == 's' &&
			    ntohl(sync.data_stream) == peer->other_stream &&
			    ntohl(sync.id) == peer->params.id)
				break;
		}
	}
	peer->sync.is_last = ntohl(sync.is_last);
	peer->sync.frames = ntohl(sync.frames);
	if (peer->sync.frames == -1)
		peer->sync.frames = peer->params.period_size;

	return peer->sync.frames;

receive_error:
	pw_log_warn("recv error: %m");
	return 0;
}

static inline int32_t netjack2_manager_sync_wait(struct netjack2_peer *peer)
{
	struct nj2_packet_header sync;
	ssize_t len;
	int32_t offset;

	while (true) {
		if ((len = recv(peer->fd, &sync, sizeof(sync), MSG_PEEK)) < 0)
			goto receive_error;

		if (len >= (ssize_t)sizeof(sync)) {
			//nj2_dump_packet_header(sync);

			if (strcmp(sync.type, "header") == 0 &&
			    ntohl(sync.data_type) == 's' &&
			    ntohl(sync.data_stream) == peer->other_stream &&
			    ntohl(sync.id) == peer->params.id)
				break;
		}
		if ((len = recv(peer->fd, &sync, sizeof(sync), 0)) < 0)
			goto receive_error;
	}
	peer->sync.cycle = ntohl(sync.cycle);
	peer->sync.is_last = ntohl(sync.is_last);
	peer->sync.frames = ntohl(sync.frames);
	if (peer->sync.frames == -1)
		peer->sync.frames = peer->params.period_size;

	offset = peer->cycle - peer->sync.cycle;
	if (offset < (int32_t)peer->params.network_latency) {
		pw_log_info("sync offset %d %d %d", peer->cycle, peer->sync.cycle, offset);
		peer->sync.is_last = true;
		return 0;
	} else {
		if ((len = recv(peer->fd, &sync, sizeof(sync), 0)) < 0)
			goto receive_error;
	}
	return peer->sync.frames;

receive_error:
	pw_log_warn("recv error: %m");
	return 0;
}

static int netjack2_recv_midi(struct netjack2_peer *peer, struct nj2_packet_header *header, uint32_t *count,
		struct data_info *info, uint32_t n_info)
{
	ssize_t len;
	uint32_t i, active_ports, sub_cycle, max_size, offset;
	uint32_t packet_size = SPA_MIN(ntohl(header->packet_size), peer->params.mtu);
	uint8_t buffer[packet_size], *p = buffer;

	//nj2_dump_packet_header(header);

	if ((len = recv(peer->fd, buffer, packet_size, 0)) < 0)
		return -errno;

	active_ports = peer->params.send_midi_channels;
	sub_cycle = ntohl(header->sub_cycle);
	peer->sync.num_packets = ntohl(header->num_packets);
	max_size = peer->params.mtu - sizeof(*header);
	offset = max_size * sub_cycle;

	p += sizeof(*header);
	len -= sizeof(*header);

	if (offset + len < peer->buffer_size)
		memcpy(SPA_PTROFF(peer->buffer, offset, void), p, len);

	if (++(*count) < peer->sync.num_packets)
		return 0;

	p = (uint8_t*)peer->buffer;
	for (i = 0; i < active_ports; i++) {
		struct nj2_midi_buffer *mbuf = (struct nj2_midi_buffer *)p;
		nj2_midi_buffer_ntoh(mbuf, mbuf);

		if (i < n_info && info[i].data != NULL) {
			netjack2_to_midi(info[i].data, peer->params.period_size * sizeof(float), mbuf);
			info[i].filled = true;
		}
		p += sizeof(*mbuf)
			+ mbuf->event_count * sizeof(struct nj2_midi_event)
			+ mbuf->write_pos;
	}
	return 0;
}

static int netjack2_recv_audio(struct netjack2_peer *peer, struct nj2_packet_header *header, uint32_t *count,
		struct data_info *info, uint32_t n_info)
{
	ssize_t len;
	uint32_t i, sub_cycle, sub_period_size, sub_period_bytes, active_ports;
	uint32_t packet_size = SPA_MIN(ntohl(header->packet_size), peer->params.mtu);
	uint8_t buffer[packet_size];

	if ((len = recv(peer->fd, buffer, packet_size, 0)) < 0)
		return -errno;

	sub_cycle = ntohl(header->sub_cycle);
	active_ports = ntohl(header->active_ports);

	if (active_ports == 0) {
		sub_period_size = peer->sync.frames;
	} else {
		uint32_t max_size = PACKET_AVAILABLE_SIZE(peer->params.mtu);
		uint32_t period = (uint32_t) powf(2.f, (uint32_t) (logf((float)max_size /
				(active_ports * sizeof(float))) / logf(2.f)));
		sub_period_size = SPA_MIN(period, (uint32_t)peer->sync.frames);
	}
	sub_period_bytes = sub_period_size * sizeof(float) + sizeof(int32_t);

	for (i = 0; i < active_ports; i++) {
		int32_t *ap = SPA_PTROFF(buffer, sizeof(*header) + i * sub_period_bytes, int32_t);
		uint32_t active_port = ntohl(ap[0]);
		void *data;

		pw_log_trace_fp("%u/%u %u %u", active_port, n_info,
				sub_cycle, sub_period_size);
		if (active_port >= n_info)
			continue;

		if ((data = info[active_port].data) != NULL) {
			float *dst = SPA_PTROFF(data,
					sub_cycle * sub_period_size * sizeof(float),
					float);
			do_volume(dst, (float*)&ap[1], peer->recv_volume, active_port, sub_period_size);
			info[active_port].filled = peer->sync.is_last;
		}
	}
	return 0;
}

static int netjack2_recv_data(struct netjack2_peer *peer,
		struct data_info *midi, uint32_t n_midi,
		struct data_info *audio, uint32_t n_audio)
{
	ssize_t len;
	uint32_t i, count = 0;
	struct nj2_packet_header header;

	while (!peer->sync.is_last) {
		if ((len = recv(peer->fd, &header, sizeof(header), MSG_PEEK)) < 0)
			goto receive_error;

		if (len < (ssize_t)sizeof(header))
			goto receive_error;

		//nj2_dump_packet_header(&header);

		if (ntohl(header.data_stream) != peer->other_stream ||
		    ntohl(header.id) != peer->params.id) {
			pw_log_debug("not our packet");
			continue;
		}

		peer->sync.is_last = ntohl(header.is_last);

		switch (ntohl(header.data_type)) {
		case 'm':
			netjack2_recv_midi(peer, &header, &count, midi, n_midi);
			break;
		case 'a':
			netjack2_recv_audio(peer, &header, &count, audio, n_audio);
			break;
		case 's':
			pw_log_info("missing last data packet");
			peer->sync.is_last = true;
			break;
		}
	}
	for (i = 0; i < n_audio; i++) {
		if (!audio[i].filled && audio[i].data != NULL)
			memset(audio[i].data, 0, peer->sync.frames * sizeof(float));
	}
	for (i = 0; i < n_midi; i++) {
		if (!midi[i].filled && midi[i].data != NULL)
			netjack2_to_midi(midi[i].data, peer->params.period_size * sizeof(float), NULL);
	}
	peer->sync.cycle = ntohl(header.cycle);
	return 0;

receive_error:
	pw_log_warn("recv error: %m");
	return -errno;
}
