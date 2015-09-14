// evecho
// 
// Written in 2015  by Pat Gavlin <pgavlin at gmail.com>
// 
// To the extent possible under law, the author(s) have dedicated all copyright
// and related and neighboring rights to this software to the public domain
// worldwide. This software is distributed without any warranty.  You should have
// received a copy of the CC0 Public Domain Dedication along with this software.
// If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.

#define _GNU_SOURCE

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>

typedef struct {
	unsigned pos;
	unsigned len;
	unsigned cap;
	uint8_t bytes[1]; // Actually variable-length
} buffer_t;

typedef struct {
	int fd;
	int done;
	buffer_t* buffer;
} client_t;

int createAndBind(char* host, char* port)
{
	assert(port != NULL);

	struct addrinfo hints = {
		.ai_flags = AI_PASSIVE,
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = 0,
	};

	struct addrinfo* res = NULL;
	int err = getaddrinfo(host, port, &hints, &res);
	if (err != 0) {
		return err;
	}

	int sfd = -1;
	for (struct addrinfo* ai = res; ai != NULL; ai = ai->ai_next) {
		if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
			continue;
		}

		sfd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
		if (sfd == -1) {
			continue;
		}

		err = bind(sfd, ai->ai_addr, ai->ai_addrlen);
		if (err == 0) {
			break;
		}

		close(sfd);
	}

	return sfd;
}

void acceptConnections(int sfd, int epfd)
{
	for (;;) {
		int cfd = accept4(sfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (cfd == -1) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				fprintf(stderr, "accept4: %d\n", errno);
			}
			return;
		}

		client_t* client = malloc(sizeof(client_t));
		client->fd = cfd;
		client->done = 0;

		buffer_t* buffer = (buffer_t*)malloc(sizeof(buffer_t) + 32);
		buffer->pos = 0;
		buffer->len = 0;
		buffer->cap = 32;
		client->buffer = buffer;

		struct epoll_event event = {
			.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP,
			.data = { .ptr = (void*)client },
		};
		int err = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &event);
		if (err != 0) {
			fprintf(stderr, "epoll_ctl: %d\n", errno);
			close(cfd);
			free(client->buffer);
			free(client);
			return;
		}

		printf("client attached\n");
	}
}

int readFrom(client_t* client, int epfd)
{
	buffer_t* buffer = client->buffer;
	unsigned endPos = (buffer->pos + buffer->len) % buffer->cap;
	
	int available = 0;
	int err = ioctl(client->fd, FIONREAD, &available);
	if (err != 0) {
		return -1;
	}

	assert(available >= 0);
	if (available == 0)
	{
		// Always attempt to read at least one byte
		available = 1;
	}

	unsigned space = buffer->cap - buffer->len;
	if (space < available) {
		unsigned required = buffer->len + available;

		// Make room!
		buffer_t* b = (buffer_t*)malloc(sizeof(buffer_t) + required);
		if (buffer->pos < endPos) {
			memcpy(&b->bytes[0], &buffer->bytes[buffer->pos], buffer->len);
		}
		else {
			unsigned end1 = buffer->cap - buffer->pos;
			memcpy(&b->bytes[0], &buffer->bytes[buffer->pos], end1);
			memcpy(&b->bytes[end1], &buffer->bytes[0], endPos);
		}
		b->pos = 0;
		b->len = buffer->len;
		b->cap = required;
		endPos = b->len;

		client->buffer = b;
		free(buffer);
		buffer = b;
	}

	int r;
	unsigned startPos = endPos;
	endPos = (startPos + available) % buffer->cap;
	if (startPos < endPos) {
		r = read(client->fd, &buffer->bytes[startPos], available);
	}
	else {
		struct iovec iov[2] = {
			{ .iov_base = &buffer->bytes[startPos], .iov_len = buffer->cap - startPos },
			{ .iov_base = &buffer->bytes[0], .iov_len = endPos }
		};
		r = readv(client->fd, iov, 2);
	}

	if (r == -1) {
		return errno != EAGAIN && errno != EWOULDBLOCK ? -1 : 0;
	}

	buffer->len += r;

	struct epoll_event event = {
		.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLHUP,
		.data = { .ptr = (void*)client },
	};
	epoll_ctl(epfd, EPOLL_CTL_MOD, client->fd, &event);
	return 0;
}

int writeTo(client_t* client, int epfd)
{
	buffer_t* buffer = client->buffer;
	unsigned endPos = (buffer->pos + buffer->len) % buffer->cap;

	int w;
	if (buffer->pos < endPos) {
		w = write(client->fd, &buffer->bytes[buffer->pos], buffer->len);
	}
	else {
		struct iovec iov[2] = {
			{ .iov_base = &buffer->bytes[buffer->pos], .iov_len = buffer->cap - buffer->pos },
			{ .iov_base = &buffer->bytes[0], .iov_len = endPos }
		};
		w = writev(client->fd, iov, 2);
	}

	if (w == -1) {
		return errno != EAGAIN && errno != EWOULDBLOCK ? -1 : 0;
	}
	else if (w == buffer->len) {
		struct epoll_event event = {
			.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP,
			.data = { .ptr = (void*)client },
		};
		epoll_ctl(epfd, EPOLL_CTL_MOD, client->fd, &event);
	}

	buffer->pos = (buffer->pos + w) % buffer->cap;
	buffer->len -= w;
	return 0;
}

void echo(client_t* client, int epfd, uint32_t events)
{
	int canRead = (events & EPOLLIN) == EPOLLIN;
	int canWrite = (events & EPOLLOUT) == EPOLLOUT;
	int noMoreData = (events & EPOLLRDHUP) == EPOLLRDHUP;
	int disconnected = (events & EPOLLHUP) == EPOLLHUP;
	int hasError = (events & EPOLLERR) == EPOLLERR;

	if (disconnected || hasError) {
		printf("client disconnected\n");
		goto err;
	}

	if (canWrite) {
		int e = writeTo(client, epfd);
		if (e != 0) {
			fprintf(stderr, "writeTo: %d\n", errno);
			goto err;
		}
	}
	if (canRead) {
		int len = client->buffer->len;
		int e = readFrom(client, epfd);
		if (e != 0) {
			fprintf(stderr, "readFrom: %d\n", errno);
			goto err;
		}
		if (client->buffer->len == len) {
			noMoreData = 1;
		}
	}
	if (noMoreData) {
		client->done = 1;
	}
	if (client->done && client->buffer->len == 0) {
		printf("disconnecting client\n");
		goto err;
	}

	return;

err:
	close(client->fd);
	epoll_ctl(epfd, client->fd, EPOLL_CTL_DEL, NULL);
	free(client->buffer);
	free(client);
	return;
}

int main(int argc, char* argv[])
{
	char* host = NULL;
	char* port = NULL;
	switch (argc) {
	case 2:
		port = argv[1];
		break;

	case 3:
		host = argv[1];
		port = argv[2];
		break;

	default:
		fprintf(stderr, "usage: %s [address-or-hostname] port-number\n", argv[0]);
		return -1;
	}

	int sfd = createAndBind(host, port);
	if (sfd == -1) {
		fprintf(stderr, "failed to open socket\n");
		return -1;
	}

	int err = listen(sfd, SOMAXCONN);
	if (err != 0) {
		fprintf(stderr, "listen: %d\n", errno);
		return -1;
	}

	int epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd == -1) {
		fprintf(stderr, "epoll_create1: %d\n", errno);
		return -1;
	}

	
	struct epoll_event event = {
		.events = EPOLLIN,
		.data = { .ptr = NULL },
	};
	err = epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &event);
	if (err != 0) {
		fprintf(stderr, "epoll_ctl: %d\n", errno);
		return -1;
	}

	struct epoll_event events[64];
	for (;;) {
		int nfds = epoll_wait(epfd, events, 64, -1);
		if (nfds == -1 && errno != EINTR) {
			fprintf(stderr, "epoll_wait: %d\n", errno);
			return -1;
		}
		else if (nfds == 0) {
			continue;
		}

		for (int i = 0; i < nfds; i++) {
			client_t* ptr = (client_t*)events[i].data.ptr;
			if (ptr == NULL) {
				acceptConnections(sfd, epfd);
			}
			else {
				echo((client_t*)events[i].data.ptr, epfd, events[i].events);
			}
		}
	}
}
