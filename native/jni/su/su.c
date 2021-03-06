/*
 * Copyright 2017, John Wu (@topjohnwu)
 * Copyright 2015, Pierre-Hugues Husson <phh@phh.me>
 * Copyright 2010, Adam Shanks (@ChainsDD)
 * Copyright 2008, Zinx Verituse (@zinxv)
 */

/* su.c - The main function running in the daemon
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <pwd.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "magisk.h"
#include "daemon.h"
#include "utils.h"
#include "su.h"
#include "pts.h"
#include "flags.h"

#define quit_signals ((int []) { SIGALRM, SIGABRT, SIGHUP, SIGPIPE, SIGQUIT, SIGTERM, SIGINT, 0 })

static void usage(int status) {
	FILE *stream = (status == EXIT_SUCCESS) ? stdout : stderr;

	fprintf(stream,
	"MagiskSU v" xstr(MAGISK_VERSION) "(" xstr(MAGISK_VER_CODE) ")\n\n"
	"Usage: su [options] [-] [user [argument...]]\n\n"
	"Options:\n"
	"  -c, --command COMMAND         pass COMMAND to the invoked shell\n"
	"  -h, --help                    display this help message and exit\n"
	"  -, -l, --login                pretend the shell to be a login shell\n"
	"  -m, -p,\n"
	"  --preserve-environment        preserve the entire environment\n"
	"  -s, --shell SHELL             use SHELL instead of the default " DEFAULT_SHELL "\n"
	"  -v, --version                 display version number and exit\n"
	"  -V                            display version code and exit\n"
	"  -mm, -M,\n"
	"  --mount-master                force run in the global mount namespace\n");
	exit(status);
}

static char *concat_commands(int argc, char *argv[]) {
	char command[ARG_MAX];
	command[0] = '\0';
	for (int i = optind - 1; i < argc; ++i) {
		if (command[0])
			sprintf(command, "%s %s", command, argv[i]);
		else
			strcpy(command, argv[i]);
	}
	return strdup(command);
}

static void sighandler(int sig) {
	restore_stdin();

	// Assume we'll only be called before death
	// See note before sigaction() in set_stdin_raw()
	//
	// Now, close all standard I/O to cause the pumps
	// to exit so we can continue and retrieve the exit
	// code
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	// Put back all the default handlers
	struct sigaction act;

	memset(&act, 0, sizeof(act));
	act.sa_handler = SIG_DFL;
	for (int i = 0; quit_signals[i]; ++i) {
		sigaction(quit_signals[i], &act, NULL);
	}
}

static void setup_sighandlers(void (*handler)(int)) {
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = handler;
	for (int i = 0; quit_signals[i]; ++i) {
		sigaction(quit_signals[i], &act, NULL);
	}
}

/*
 * Connect daemon, send argc, argv, cwd, pts slave
 */
int su_client_main(int argc, char *argv[]) {
	int c;
	struct option long_opts[] = {
			{ "command",                required_argument,  NULL, 'c' },
			{ "help",                   no_argument,        NULL, 'h' },
			{ "login",                  no_argument,        NULL, 'l' },
			{ "preserve-environment",   no_argument,        NULL, 'p' },
			{ "shell",                  required_argument,  NULL, 's' },
			{ "version",                no_argument,        NULL, 'v' },
			{ "context",                required_argument,  NULL, 'z' },
			{ "mount-master",           no_argument,        NULL, 'M' },
			{ NULL, 0, NULL, 0 },
	};

	struct su_request su_req = {
		.uid = UID_ROOT,
		.login = 0,
		.keepenv = 0,
		.mount_master = 0,
		.shell = DEFAULT_SHELL,
		.command = "",
	};


	for (int i = 0; i < argc; i++) {
		// Replace -cn with -z, -mm with -M for supporting getopt_long
		if (strcmp(argv[i], "-cn") == 0)
			strcpy(argv[i], "-z");
		else if (strcmp(argv[i], "-mm") == 0)
			strcpy(argv[i], "-M");
	}

	while ((c = getopt_long(argc, argv, "c:hlmps:Vvuz:M", long_opts, NULL)) != -1) {
		switch (c) {
			case 'c':
				su_req.command = concat_commands(argc, argv);
				optind = argc;
				break;
			case 'h':
				usage(EXIT_SUCCESS);
				break;
			case 'l':
				su_req.login = 1;
				break;
			case 'm':
			case 'p':
				su_req.keepenv = 1;
				break;
			case 's':
				su_req.shell = optarg;
				break;
			case 'V':
				printf("%d\n", MAGISK_VER_CODE);
				exit(EXIT_SUCCESS);
			case 'v':
				printf("%s\n", xstr(MAGISK_VERSION) ":MAGISKSU (topjohnwu)");
				exit(EXIT_SUCCESS);
			case 'z':
				// Do nothing, placed here for legacy support :)
				break;
			case 'M':
				su_req.mount_master = 1;
				break;
			default:
				/* Bionic getopt_long doesn't terminate its error output by newline */
				fprintf(stderr, "\n");
				usage(2);
		}
	}

	if (optind < argc && strcmp(argv[optind], "-") == 0) {
		su_req.login = 1;
		optind++;
	}
	/* username or uid */
	if (optind < argc) {
		struct passwd *pw;
		pw = getpwnam(argv[optind]);
		if (pw)
			su_req.uid = pw->pw_uid;
		else
			su_req.uid = atoi(argv[optind]);
		optind++;
	}

	char pts_slave[PATH_MAX];
	int ptmx, fd;

	// Connect to client
	fd = connect_daemon();

	// Tell the daemon we are su
	write_int(fd, SUPERUSER);

	// Send su_request
	xwrite(fd, &su_req, 4 * sizeof(unsigned));
	write_string(fd, su_req.shell);
	write_string(fd, su_req.command);

	// Determine which one of our streams are attached to a TTY
	int atty = 0;
	if (isatty(STDIN_FILENO))  atty |= ATTY_IN;
	if (isatty(STDOUT_FILENO)) atty |= ATTY_OUT;
	if (isatty(STDERR_FILENO)) atty |= ATTY_ERR;

	if (atty) {
		// We need a PTY. Get one.
		ptmx = pts_open(pts_slave, sizeof(pts_slave));
	} else {
		pts_slave[0] = '\0';
	}

	// Send pts_slave
	write_string(fd, pts_slave);

	// Send stdin
	send_fd(fd, (atty & ATTY_IN) ? -1 : STDIN_FILENO);
	// Send stdout
	send_fd(fd, (atty & ATTY_OUT) ? -1 : STDOUT_FILENO);
	// Send stderr
	send_fd(fd, (atty & ATTY_ERR) ? -1 : STDERR_FILENO);

	// Wait for ack from daemon
	if (read_int(fd)) {
		// Fast fail
		fprintf(stderr, "%s\n", strerror(EACCES));
		return DENY;
	}

	if (atty & ATTY_IN) {
		setup_sighandlers(sighandler);
		pump_stdin_async(ptmx);
	}
	if (atty & ATTY_OUT) {
		// Forward SIGWINCH
		watch_sigwinch_async(STDOUT_FILENO, ptmx);
		pump_stdout_blocking(ptmx);
	}

	// Get the exit code
	int code = read_int(fd);
	close(fd);

	return code;
}