/*
 * Copyright 2016 IBM
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>

#include <systemd/sd-bus.h>

#include "mbox_dbus.h"

#define USAGE \
"Usage: %s <command> [args]\n\n" \
"\tCommands:\n" \
"\t\t--ping\t\t\t- ping the daemon (args: 0)\n" \
"\t\t--status\t\t- check status of the daemon (args: 0)\n" \
"\t\t--reset\t\t\t- hard reset the daemon state (args: 0)\n" \
"\t\t--point-to-flash\t- point the lpc mapping back to flash (args: 0)\n" \
"\t\t--suspend\t\t- suspend the daemon to inhibit flash accesses (args: 0)\n" \
"\t\t--resume\t\t- resume the daemon (args: 1)\n" \
"\t\t\targ[0]: whether flash was modified (0 - no | 1 - yes)\n" \
"\t\t--flash-modified\t- tell the daemon to discard its cache (args: 0)\n"

#define NAME		"MBOX Control"
#define VERSION		1
#define SUBVERSION	0

struct mboxctl_context {
	sd_bus *bus;
};

static void usage(char *name)
{
	printf(USAGE, name);
	exit(0);
}

static inline const char *parse_error(int error_val)
{
	switch (error_val) {
	case DBUS_SUCCESS:
		return "Success";
	case E_DBUS_INTERNAL:
		return "Failed - Internal Error";
	case E_DBUS_INVAL:
		return "Failed - Invalid Command or Request";
	case E_DBUS_REJECTED:
		return "Failed - Request Rejected by Daemon";
	case E_DBUS_HARDWARE:
		return "Failed - BMC Hardware Error";
	default:
		return "Failed - Unknown Error";
	}
}

static int init_dbus_dev(struct mboxctl_context *context)
{
	int rc;

	rc = sd_bus_default_system(&context->bus);
	if (rc < 0) {
		fprintf(stderr, "Failed to connect to the system bus: %s\n",
			strerror(-rc));
	}

	return rc;
}

static int send_dbus_msg(struct mboxctl_context *context,
			 struct mbox_dbus_msg *msg,
			 struct mbox_dbus_msg *resp)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL, *n = NULL;
	uint8_t *buf;
	size_t sz;
	int rc;

	/* Generate the bus message */
	rc = sd_bus_message_new_method_call(context->bus, &m, DBUS_NAME,
					    DOBJ_NAME, DBUS_NAME, "cmd");
	if (rc < 0) {
		fprintf(stderr, "Failed to init method call: %s\n",
			strerror(-rc));
		goto out;
	}

	/* Add the command */
	rc = sd_bus_message_append(m, "y", msg->cmd);
	if (rc < 0) {
		fprintf(stderr, "Failed to add cmd to message: %s\n",
			strerror(-rc));
		goto out;
	}

	/* Add the args */
	rc = sd_bus_message_append_array(m, 'y', msg->args, msg->num_args);
	if (rc < 0) {
		fprintf(stderr, "Failed to add args to message: %s\n",
			strerror(-rc));
		goto out;
	}

	/* Send the message */
	rc = sd_bus_call(context->bus, m, 0, &error, &n);
	if (rc < 0) {
		fprintf(stderr, "Failed to post message: %s\n", strerror(-rc));
		goto out;
	}

	/* Read response code */
	rc = sd_bus_message_read(n, "y", &resp->cmd);
	if (rc < 0) {
		fprintf(stderr, "Failed to read response code: %s\n",
			strerror(-rc));
		goto out;
	}

	/* Read response args */
	rc = sd_bus_message_read_array(n, 'y', (const void **) &buf, &sz);
	if (rc < 0) {
		fprintf(stderr, "Failed to read response args: %s\n",
			strerror(-rc));
		goto out;
	}

	if (sz < resp->num_args) {
		fprintf(stderr, "Command returned insufficient response args\n");
		rc = -E_DBUS_INTERNAL;
		goto out;
	}

	memcpy(resp->args, buf, resp->num_args);
	rc = 0;

out:
	sd_bus_error_free(&error);
	sd_bus_message_unref(m);
	sd_bus_message_unref(n);

	return rc;
}

static int handle_cmd_ping(struct mboxctl_context *context)
{
	struct mbox_dbus_msg msg = { 0 }, resp = { 0 };
	int rc;

	msg.cmd = DBUS_C_PING;

	rc = send_dbus_msg(context, &msg, &resp);
	if (rc < 0) {
		fprintf(stderr, "Failed to send ping command\n");
		goto out;
	}

	rc = -resp.cmd;
	printf("Ping: %s\n", parse_error(resp.cmd));

out:
	return rc;
}

static int handle_cmd_status(struct mboxctl_context *context)
{
	struct mbox_dbus_msg msg = { 0 }, resp = { 0 };
	int rc;

	msg.cmd = DBUS_C_STATUS;
	resp.num_args = 1;
	resp.args = calloc(resp.num_args, sizeof(*resp.args));

	rc = send_dbus_msg(context, &msg, &resp);
	if (rc < 0) {
		fprintf(stderr, "Failed to send status command\n");
		goto out;
	}

	rc = -resp.cmd;
	if (resp.cmd != DBUS_SUCCESS) {
		fprintf(stderr, "Status command failed\n");
		goto out;
	}

	printf("Daemon Status: %s\n", resp.args[0] == STATUS_ACTIVE ? "Active" :
				      "Suspended");

out:
	free(resp.args);
	return rc;
}

static int handle_cmd_reset(struct mboxctl_context *context)
{
	struct mbox_dbus_msg msg = { 0 }, resp = { 0 };
	int rc;

	msg.cmd = DBUS_C_RESET;

	rc = send_dbus_msg(context, &msg, &resp);
	if (rc < 0) {
		fprintf(stderr, "Failed to send reset command\n");
		goto out;
	}

	rc = -resp.cmd;
	printf("Reset: %s\n", parse_error(resp.cmd));

out:
	return rc;
}

static int handle_cmd_suspend(struct mboxctl_context *context)
{
	struct mbox_dbus_msg msg = { 0 }, resp = { 0 };
	int rc;

	msg.cmd = DBUS_C_SUSPEND;

	rc = send_dbus_msg(context, &msg, &resp);
	if (rc < 0) {
		fprintf(stderr, "Failed to send suspend command\n");
		goto out;
	}

	rc = -resp.cmd;
	printf("Suspend: %s\n", parse_error(resp.cmd));

out:
	return rc;
}

static int handle_cmd_resume(struct mboxctl_context *context, char *arg)
{
	struct mbox_dbus_msg msg = { 0 }, resp = { 0 };
	int rc;

	if (!arg || *arg != '0' || *arg != '1') {
		rc = -E_DBUS_INVAL;
		goto out;
	}

	msg.cmd = DBUS_C_RESUME;
	msg.num_args = 1;
	msg.args = calloc(msg.num_args, sizeof(*msg.args));
	msg.args[0] = *arg == '1' ? RESUME_FLASH_MODIFIED :
				    RESUME_NOT_MODIFIED;

	rc = send_dbus_msg(context, &msg, &resp);
	if (rc < 0) {
		fprintf(stderr, "Failed to send resume command\n");
		goto out;
	}

	rc = -resp.cmd;
	printf("Resume: %s\n", parse_error(resp.cmd));

out:
	return rc;
}

static int handle_cmd_modified(struct mboxctl_context *context)
{
	struct mbox_dbus_msg msg = { 0 }, resp = { 0 };
	int rc;

	msg.cmd = DBUS_C_MODIFIED;

	rc = send_dbus_msg(context, &msg, &resp);
	if (rc < 0) {
		fprintf(stderr, "Failed to send flash modified command\n");
		goto out;
	}

	rc = -resp.cmd;
	printf("Flash Modified: %s\n", parse_error(resp.cmd));

out:
	free(msg.args);
	return rc;
}

static int parse_cmdline(struct mboxctl_context *context, int argc, char **argv)
{
	int opt, rc = -1;

	static const struct option long_options[] = {
		{ "ping",		no_argument,		0, 'p' },
		{ "status",		no_argument,		0, 's' },
		{ "reset",		no_argument,		0, 'r' },
		{ "point-to-flash",	no_argument,		0, 'f' },
		{ "suspend",		no_argument,		0, 'u' },
		{ "resume",		required_argument,	0, 'e' },
		{ "flash_modified",	no_argument,		0, 'm' },
		{ "version",		no_argument,		0, 'v' },
		{ "help",		no_argument,		0, 'h' },
		{ 0,			0,			0, 0   }
	};

	while ((opt = getopt_long(argc, argv, "psrfs:umvh", long_options, NULL))
			!= -1)
	{
		switch (opt) {
		case 'p':
			rc = handle_cmd_ping(context);
			break;
		case 's':
			rc = handle_cmd_status(context);
			break;
		case 'r': /* These are the same for now (reset may change) */
		case 'f':
			rc = handle_cmd_reset(context);
			break;
		case 'u':
			rc = handle_cmd_suspend(context);
			break;
		case 'e':
			rc = handle_cmd_resume(context, optarg);
			break;
		case 'm':
			rc = handle_cmd_modified(context);
			break;
		case 'v':
			printf("%s V%d.%.2d\n", NAME, VERSION, SUBVERSION);
			rc = 0;
			break;
		case 'h':
			usage(argv[0]);
			rc = 0;
			break;
		default:
			usage(argv[0]);
			rc = -E_DBUS_INVAL;
			break;
		}
	}

	return rc;
}

int main(int argc, char **argv)
{
	struct mboxctl_context context;
	int rc;
	
	rc = init_dbus_dev(&context);
	if (rc < 0) {
		fprintf(stderr, "Failed to init dbus\n");
		goto out;
	}

	rc = parse_cmdline(&context, argc, argv);

out:
	return rc;
}