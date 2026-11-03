// SPDX-License-Identifier: GPL-2.0
/*
 * afhs - userspace controller for Nitrux Aesthetic FHS.
 */

#define _GNU_SOURCE

#include "afhs.h"

#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define AFHS_CONFIG_PATH "/etc/aesthetic-fhs.conf"
#define AFHS_BUFFER_SIZE 8192

struct afhs_client {
	int fd;
	unsigned int sequence;
	unsigned short family_id;
};

static size_t afhs_align(size_t length)
{
	return (length + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1);
}

static void *afhs_attr_data(struct nlattr *attr)
{
	return (char *)attr + NLA_HDRLEN;
}

static int afhs_add_attribute(struct nlmsghdr *message, size_t capacity,
				      unsigned short type, const void *data,
				      size_t length)
{
	struct nlattr *attr;
	size_t attribute_length = NLA_HDRLEN + length;
	size_t aligned_length = afhs_align(attribute_length);

	if (message->nlmsg_len + aligned_length > capacity)
		return -1;

	attr = (struct nlattr *)((char *)message + message->nlmsg_len);
	attr->nla_type = type;
	attr->nla_len = attribute_length;
	memcpy(afhs_attr_data(attr), data, length);
	if (aligned_length > attribute_length)
		memset((char *)attr + attribute_length, 0,
		       aligned_length - attribute_length);
	message->nlmsg_len += aligned_length;
	return 0;
}

static struct nlattr *afhs_next_attribute(struct nlattr *attr, int *remaining)
{
	size_t aligned_length;

	if (*remaining < NLA_HDRLEN || attr->nla_len < NLA_HDRLEN ||
	    attr->nla_len > *remaining)
		return NULL;

	aligned_length = afhs_align(attr->nla_len);
	if (aligned_length > (size_t)*remaining)
		return NULL;

	*remaining -= aligned_length;
	return (struct nlattr *)((char *)attr + aligned_length);
}

static int afhs_open(struct afhs_client *client)
{
	struct sockaddr_nl address = {
		.nl_family = AF_NETLINK,
	};

	client->fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (client->fd < 0) {
		fprintf(stderr, "afhs: cannot open Generic Netlink socket: %s\n",
			strerror(errno));
		return -1;
	}

	if (bind(client->fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		fprintf(stderr, "afhs: cannot bind Generic Netlink socket: %s\n",
			strerror(errno));
		close(client->fd);
		client->fd = -1;
		return -1;
	}

	client->sequence = 0;
	client->family_id = 0;
	return 0;
}

static int afhs_send(struct afhs_client *client, struct nlmsghdr *message)
{
	struct sockaddr_nl address = {
		.nl_family = AF_NETLINK,
	};
	struct iovec vector = {
		.iov_base = message,
		.iov_len = message->nlmsg_len,
	};
	struct msghdr header = {
		.msg_name = &address,
		.msg_namelen = sizeof(address),
		.msg_iov = &vector,
		.msg_iovlen = 1,
	};

	if (sendmsg(client->fd, &header, 0) < 0) {
		fprintf(stderr, "afhs: cannot send Generic Netlink request: %s\n",
			strerror(errno));
		return -1;
	}

	return 0;
}

static int afhs_receive(struct afhs_client *client, char *buffer,
			 size_t capacity, ssize_t *length)
{
	struct sockaddr_nl address;
	struct iovec vector = {
		.iov_base = buffer,
		.iov_len = capacity,
	};
	struct msghdr header = {
		.msg_name = &address,
		.msg_namelen = sizeof(address),
		.msg_iov = &vector,
		.msg_iovlen = 1,
	};

	*length = recvmsg(client->fd, &header, 0);
	if (*length < 0) {
		fprintf(stderr, "afhs: cannot receive Generic Netlink response: %s\n",
			strerror(errno));
		return -1;
	}

	if (header.msg_flags & MSG_TRUNC) {
		fprintf(stderr, "afhs: Generic Netlink response was truncated\n");
		return -1;
	}

	return 0;
}

static int afhs_error(const struct nlmsghdr *message)
{
	const struct nlmsgerr *error;

	if (message->nlmsg_len < NLMSG_LENGTH(sizeof(*error)))
		return -EPROTO;

	error = (const struct nlmsgerr *)NLMSG_DATA(message);
	if (error->error == 0)
		return 0;

	return error->error;
}

static int afhs_resolve_family(struct afhs_client *client)
{
	char buffer[AFHS_BUFFER_SIZE];
	struct nlmsghdr *message = (struct nlmsghdr *)buffer;
	struct genlmsghdr *generic;
	ssize_t length;

	memset(buffer, 0, sizeof(buffer));
	message->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	message->nlmsg_type = GENL_ID_CTRL;
	message->nlmsg_flags = NLM_F_REQUEST;
	message->nlmsg_seq = ++client->sequence;
	generic = (struct genlmsghdr *)NLMSG_DATA(message);
	generic->cmd = CTRL_CMD_GETFAMILY;
	generic->version = 1;

	if (afhs_add_attribute(message, sizeof(buffer), CTRL_ATTR_FAMILY_NAME,
			       AFHS_GENL_NAME, sizeof(AFHS_GENL_NAME)) < 0) {
		fprintf(stderr, "afhs: family-name request is too large\n");
		return -1;
	}

	if (afhs_send(client, message) < 0 ||
	    afhs_receive(client, buffer, sizeof(buffer), &length) < 0)
		return -1;

	for (message = (struct nlmsghdr *)buffer;
	     NLMSG_OK(message, length);
	     message = NLMSG_NEXT(message, length)) {
		struct nlattr *attribute;
		int remaining;

		if (message->nlmsg_seq != client->sequence)
			continue;
		if (message->nlmsg_type == NLMSG_ERROR) {
			int error = afhs_error(message);

			if (error < 0)
				fprintf(stderr, "afhs: cannot resolve kernel family: %s\n",
					strerror(-error));
			return error < 0 ? -1 : 0;
		}
		if (message->nlmsg_type != GENL_ID_CTRL ||
		    message->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN))
			continue;

		remaining = message->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
		attribute = (struct nlattr *)((char *)NLMSG_DATA(message) +
					      GENL_HDRLEN);
		while (remaining >= NLA_HDRLEN) {
			struct nlattr *next = afhs_next_attribute(attribute, &remaining);

			if (!next)
				break;
			if (attribute->nla_type == CTRL_ATTR_FAMILY_ID &&
			    attribute->nla_len >= NLA_HDRLEN + sizeof(unsigned short)) {
				memcpy(&client->family_id, afhs_attr_data(attribute),
				       sizeof(client->family_id));
				return 0;
			}
			attribute = next;
		}
	}

	fprintf(stderr, "afhs: kernel family %s was not found\n", AFHS_GENL_NAME);
	return -1;
}

static int afhs_request(struct afhs_client *client, unsigned char command,
			const char *path)
{
	char buffer[AFHS_BUFFER_SIZE];
	struct nlmsghdr *message = (struct nlmsghdr *)buffer;
	struct genlmsghdr *generic;
	ssize_t length;

	memset(buffer, 0, sizeof(buffer));
	message->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	message->nlmsg_type = client->family_id;
	message->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	message->nlmsg_seq = ++client->sequence;
	generic = (struct genlmsghdr *)NLMSG_DATA(message);
	generic->cmd = command;
	generic->version = AFHS_GENL_VERSION;

	if (path && afhs_add_attribute(message, sizeof(buffer), AFHS_ATTR_PATH,
				       path, strlen(path) + 1) < 0) {
		fprintf(stderr, "afhs: path is too large\n");
		return -1;
	}

	if (afhs_send(client, message) < 0)
		return -1;

	for (;;) {
		struct nlmsghdr *response;
		unsigned int response_length;

		if (afhs_receive(client, buffer, sizeof(buffer), &length) < 0)
			return -1;
		response_length = (unsigned int)length;
		for (response = (struct nlmsghdr *)buffer;
		     NLMSG_OK(response, response_length);
		     response = NLMSG_NEXT(response, response_length)) {
			int error;

			if (response->nlmsg_seq != client->sequence)
				continue;
			if (response->nlmsg_type != NLMSG_ERROR)
				continue;
			error = afhs_error(response);
			if (error < 0)
				fprintf(stderr, "afhs: kernel rejected request: %s\n",
					strerror(-error));
			return error < 0 ? -1 : 0;
		}
	}
}

static int afhs_list(struct afhs_client *client)
{
	char buffer[AFHS_BUFFER_SIZE];
	struct nlmsghdr *message = (struct nlmsghdr *)buffer;
	struct genlmsghdr *generic;
	ssize_t length;

	memset(buffer, 0, sizeof(buffer));
	message->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	message->nlmsg_type = client->family_id;
	message->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	message->nlmsg_seq = ++client->sequence;
	generic = (struct genlmsghdr *)NLMSG_DATA(message);
	generic->cmd = AFHS_CMD_LIST;
	generic->version = AFHS_GENL_VERSION;

	if (afhs_send(client, message) < 0)
		return -1;

	for (;;) {
		unsigned int response_length;

		if (afhs_receive(client, buffer, sizeof(buffer), &length) < 0)
			return -1;
		response_length = (unsigned int)length;
		for (message = (struct nlmsghdr *)buffer;
		     NLMSG_OK(message, response_length);
		     message = NLMSG_NEXT(message, response_length)) {
			struct nlattr *attribute;
			int remaining;

			if (message->nlmsg_seq != client->sequence)
				continue;
			if (message->nlmsg_type == NLMSG_DONE)
				return 0;
			if (message->nlmsg_type == NLMSG_ERROR) {
				int error = afhs_error(message);

				if (error < 0)
					fprintf(stderr, "afhs: cannot list hidden paths: %s\n",
						strerror(-error));
				return error < 0 ? -1 : 0;
			}
			if (message->nlmsg_type != client->family_id ||
			    message->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN))
				continue;

			remaining = message->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
			attribute = (struct nlattr *)((char *)NLMSG_DATA(message) +
					      GENL_HDRLEN);
			while (remaining >= NLA_HDRLEN) {
				struct nlattr *next = afhs_next_attribute(attribute,
								      &remaining);

				if (!next)
					break;
				if (attribute->nla_type == AFHS_ATTR_PATH) {
					int path_length = attribute->nla_len - NLA_HDRLEN;

					if (path_length > 0)
						printf("%.*s\n", path_length,
						       (char *)afhs_attr_data(attribute));
				}
				attribute = next;
			}
		}
	}
}

static char *afhs_normalize_path(const char *line)
{
	char *path;

	if (line[0] == '/')
		return strdup(line);

	if (asprintf(&path, "/%s", line) < 0)
		return NULL;
	return path;
}

static char *afhs_trim(char *line)
{
	char *end;

	while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n')
		line++;
	end = line + strlen(line);
	while (end > line && (end[-1] == ' ' || end[-1] == '\t' ||
				      end[-1] == '\r' || end[-1] == '\n'))
		*--end = '\0';
	return line;
}

static int afhs_load(struct afhs_client *client, const char *filename)
{
	FILE *file;
	char *line = NULL;
	size_t capacity = 0;
	unsigned long line_number = 0;
	int result = 0;

	file = fopen(filename, "re");
	if (!file) {
		fprintf(stderr, "afhs: cannot open %s: %s\n", filename,
			strerror(errno));
		return -1;
	}

	if (afhs_request(client, AFHS_CMD_FLUSH, NULL) < 0) {
		fclose(file);
		return -1;
	}

	while (getline(&line, &capacity, file) >= 0) {
		char *entry;
		char *path;

		line_number++;
		entry = afhs_trim(line);
		if (!*entry || *entry == '#')
			continue;
		path = afhs_normalize_path(entry);
		if (!path) {
			fprintf(stderr, "afhs: cannot allocate path for %s:%lu\n",
				filename, line_number);
			result = -1;
			break;
		}
		{
			struct stat status;

			if (lstat(path, &status) < 0) {
				if (errno == ENOENT || errno == ENOTDIR) {
					free(path);
					continue;
				}
				fprintf(stderr, "afhs: cannot stat %s:%lu: %s\n",
					filename, line_number, strerror(errno));
				result = -1;
				free(path);
				break;
			}
		}
		if (afhs_request(client, AFHS_CMD_HIDE, path) < 0) {
			fprintf(stderr, "afhs: cannot hide %s:%lu\n", filename,
				line_number);
			result = -1;
			free(path);
			break;
		}
		free(path);
	}

	if (ferror(file)) {
		fprintf(stderr, "afhs: cannot read %s: %s\n", filename,
			strerror(errno));
		result = -1;
	}
	free(line);
	fclose(file);
	return result;
}

static void afhs_usage(FILE *stream)
{
	fputs("Usage: afhs <hide|unhide|flush|list|load> [PATH|FILE]\n",
	      stream);
}

int main(int argc, char **argv)
{
	struct afhs_client client = {
		.fd = -1,
	};
	int result;

	if (argc < 2 || argc > 3) {
		afhs_usage(stderr);
		return 64;
	}

	if (afhs_open(&client) < 0)
		return 1;
	if (afhs_resolve_family(&client) < 0) {
		close(client.fd);
		return 1;
	}

	if (!strcmp(argv[1], "hide") && argc == 3)
		result = afhs_request(&client, AFHS_CMD_HIDE, argv[2]);
	else if (!strcmp(argv[1], "unhide") && argc == 3)
		result = afhs_request(&client, AFHS_CMD_UNHIDE, argv[2]);
	else if (!strcmp(argv[1], "flush") && argc == 2)
		result = afhs_request(&client, AFHS_CMD_FLUSH, NULL);
	else if (!strcmp(argv[1], "list") && argc == 2)
		result = afhs_list(&client);
	else if (!strcmp(argv[1], "load"))
		result = afhs_load(&client, argc == 3 ? argv[2] : AFHS_CONFIG_PATH);
	else {
		afhs_usage(stderr);
		result = -1;
	}

	close(client.fd);
	return result < 0 ? 1 : 0;
}
