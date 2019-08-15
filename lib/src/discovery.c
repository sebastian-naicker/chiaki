/*
 * This file is part of Chiaki.
 *
 * Chiaki is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Chiaki is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Chiaki.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <chiaki/discovery.h>
#include <chiaki/http.h>
#include <chiaki/log.h>

#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <stdio.h>
#include <fcntl.h>
#include <arpa/inet.h>

const char *chiaki_discovery_host_state_string(ChiakiDiscoveryHostState state)
{
	switch(state)
	{
		case CHIAKI_DISCOVERY_HOST_STATE_READY:
			return "ready";
		case CHIAKI_DISCOVERY_HOST_STATE_STANDBY:
			return "standby";
		default:
			return "unknown";
	}
}

CHIAKI_EXPORT int chiaki_discovery_packet_fmt(char *buf, size_t buf_size, ChiakiDiscoveryPacket *packet)
{
	const char *version_str = packet->protocol_version ? packet->protocol_version : CHIAKI_DISCOVERY_PROTOCOL_VERSION;
	switch(packet->cmd)
	{
		case CHIAKI_DISCOVERY_CMD_SRCH:
			return snprintf(buf, buf_size, "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:%s\n",
							version_str);
		default:
			return -1;
	}
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_discovery_srch_response_parse(ChiakiDiscoveryHost *response, struct sockaddr *addr, char *addr_buf, size_t addr_buf_size, char *buf, size_t buf_size)
{
	ChiakiHttpResponse http_response;
	ChiakiErrorCode err = chiaki_http_response_parse(&http_response, buf, buf_size);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	memset(response, 0, sizeof(*response));

	void *addr_src;
	switch(addr->sa_family)
	{
		case AF_INET:
			addr_src = &((struct sockaddr_in *)addr)->sin_addr;
			break;
		case AF_INET6:
			addr_src = &((struct sockaddr_in6 *)addr)->sin6_addr;
			break;
		default:
			addr_src = NULL;
			break;
	}
	if(addr_src)
		response->host_addr = inet_ntop(addr->sa_family, addr_src, addr_buf, addr_buf_size);

	switch(http_response.code)
	{
		case 200:
			response->state = CHIAKI_DISCOVERY_HOST_STATE_READY;
			break;
		case 620:
			response->state = CHIAKI_DISCOVERY_HOST_STATE_STANDBY;
			break;
		default:
			response->state = CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN;
			break;
	}

	for(ChiakiHttpHeader *header = http_response.headers; header; header=header->next)
	{
		if(strcmp(header->key, "system-version") == 0)
			response->system_version = header->value;
		else if(strcmp(header->key, "device-discovery-protocol-version") == 0)
			response->device_discovery_protocol_version = header->value;
		else if(strcmp(header->key, "host-request-port") == 0)
			response->host_request_port = (uint16_t)strtoul(header->value, NULL, 0);
		else if(strcmp(header->key, "host-name") == 0)
			response->host_name = header->value;
		else if(strcmp(header->key, "host-type") == 0)
			response->host_type = header->value;
		else if(strcmp(header->key, "host-id") == 0)
			response->host_id = header->value;
		else if(strcmp(header->key, "running-app-titleid") == 0)
			response->running_app_titleid = header->value;
		else if(strcmp(header->key, "running-app-name") == 0)
			response->running_app_name = header->value;
		//else
		//	printf("unknown %s: %s\n", header->key, header->value);
	}

	chiaki_http_response_fini(&http_response);
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_discovery_init(ChiakiDiscovery *discovery, ChiakiLog *log, sa_family_t family)
{
	if(family != AF_INET && family != AF_INET6)
		return CHIAKI_ERR_INVALID_DATA;

	discovery->log = log;

	discovery->socket = socket(AF_INET, SOCK_DGRAM, 0);
	if(discovery->socket < 0)
	{
		CHIAKI_LOGE(discovery->log, "Discovery failed to create socket");
		return CHIAKI_ERR_NETWORK;
	}

	memset(&discovery->local_addr, 0, sizeof(discovery->local_addr));
	discovery->local_addr.sa_family = family;
	if(family == AF_INET6)
	{
		struct in6_addr anyaddr = IN6ADDR_ANY_INIT;
		struct sockaddr_in6 *addr = (struct sockaddr_in6 *)&discovery->local_addr;
		addr->sin6_addr = anyaddr;
		addr->sin6_port = htons(0);
	}
	else // AF_INET
	{
		struct sockaddr_in *addr = (struct sockaddr_in *)&discovery->local_addr;
		addr->sin_addr.s_addr = htonl(INADDR_ANY);
		addr->sin_port = htons(0);
	}

	int r = bind(discovery->socket, &discovery->local_addr, sizeof(discovery->local_addr));
	if(r < 0)
	{
		CHIAKI_LOGE(discovery->log, "Discovery failed to bind");
		close(discovery->socket);
		return CHIAKI_ERR_NETWORK;
	}

	const int broadcast = 1;
	r = setsockopt(discovery->socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
	if(r < 0)
		CHIAKI_LOGE(discovery->log, "Discovery failed to setsockopt SO_BROADCAST");

	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT void chiaki_discovery_fini(ChiakiDiscovery *discovery)
{
	close(discovery->socket);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_discovery_send(ChiakiDiscovery *discovery, ChiakiDiscoveryPacket *packet, struct sockaddr *addr, size_t addr_size)
{
	if(addr->sa_family != discovery->local_addr.sa_family)
		return CHIAKI_ERR_INVALID_DATA;

	char buf[512];
	int len = chiaki_discovery_packet_fmt(buf, sizeof(buf), packet);
	if(len < 0)
		return CHIAKI_ERR_UNKNOWN;
	if((size_t)len >= sizeof(buf))
		return CHIAKI_ERR_BUF_TOO_SMALL;

	ssize_t rc = sendto(discovery->socket, buf, (size_t)len + 1, 0, addr, addr_size);
	if(rc < 0)
		return CHIAKI_ERR_NETWORK;

	return CHIAKI_ERR_SUCCESS;
}

static void *discovery_thread_func(void *user);

CHIAKI_EXPORT ChiakiErrorCode chiaki_discovery_thread_start(ChiakiDiscoveryThread *thread, ChiakiDiscovery *discovery, ChiakiDiscoveryCb cb, void *cb_user)
{
	thread->discovery = discovery;
	thread->cb = cb;
	thread->cb_user = cb_user;

	ChiakiErrorCode err = chiaki_stop_pipe_init(&thread->stop_pipe);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(discovery->log, "Discovery (thread) failed to create pipe");
		return err;
	}

	err = chiaki_thread_create(&thread->thread, discovery_thread_func, thread);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		chiaki_stop_pipe_fini(&thread->stop_pipe);
		return err;
	}

	chiaki_thread_set_name(&thread->thread, "Chiaki Discovery");

	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_discovery_thread_stop(ChiakiDiscoveryThread *thread)
{
	chiaki_stop_pipe_stop(&thread->stop_pipe);
	ChiakiErrorCode err = chiaki_thread_join(&thread->thread, NULL);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	chiaki_stop_pipe_fini(&thread->stop_pipe);
	return CHIAKI_ERR_SUCCESS;
}

static void *discovery_thread_func(void *user)
{
	ChiakiDiscoveryThread *thread = user;
	ChiakiDiscovery *discovery = thread->discovery;

	while(1)
	{
		ChiakiErrorCode err = chiaki_stop_pipe_select_single(&thread->stop_pipe, discovery->socket, UINT64_MAX);
		if(err == CHIAKI_ERR_CANCELED)
			break;
		if(err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGE(discovery->log, "Discovery thread failed to select");
			break;
		}

		char buf[512];
		struct sockaddr client_addr;
		socklen_t client_addr_size = sizeof(client_addr);
		ssize_t n = recvfrom(discovery->socket, buf, sizeof(buf) - 1, 0, &client_addr, &client_addr_size);
		if(n < 0)
		{
			CHIAKI_LOGE(discovery->log, "Discovery thread failed to read from socket");
			break;
		}

		if(n == 0)
			continue;

		if(n > sizeof(buf) - 1)
			n = sizeof(buf) - 1;

		buf[n] = '\00';

		//CHIAKI_LOGV(discovery->log, "Discovery received:\n%s", buf);
		//chiaki_log_hexdump_raw(discovery->log, CHIAKI_LOG_VERBOSE, (const uint8_t *)buf, n);

		char addr_buf[64];
		ChiakiDiscoveryHost response;
		err = chiaki_discovery_srch_response_parse(&response, &client_addr, addr_buf, sizeof(addr_buf), buf, n);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGI(discovery->log, "Discovery Response invalid");
			continue;
		}

		if(thread->cb)
			thread->cb(&response, thread->cb_user);
	}

	return NULL;
}