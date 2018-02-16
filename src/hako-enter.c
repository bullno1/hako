#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#define OPTPARSE_IMPLEMENTATION
#define OPTPARSE_API static __attribute__((unused))
#include "optparse.h"
#define OPTPARSE_HELP_IMPLEMENTATION
#define OPTPARSE_HELP_API static
#include "optparse-help.h"
#include "hako-common.h"

#define PROG_NAME "hako-enter"
#define quit(code) exit_code = code; goto quit;

static bool
enter_sandbox(const char* pid)
{
	bool exit_code = true;
	DIR* dir = NULL;
	char ns_dir[256]; // long enough to hold path to namespace

	if(snprintf(ns_dir, sizeof(ns_dir), "/proc/%s/ns", pid) > (int)sizeof(ns_dir))
	{
		fprintf(stderr, "Invalid pid\n");
		quit(false);
	}

	if(chdir(ns_dir) == -1)
	{
		fprintf(stderr, "chdir(\"%s\") failed: %s\n", ns_dir, strerror(errno));
		quit(false);
	}

	dir = opendir(".");
	if(dir == NULL)
	{
		perror("Could not examine sandbox");
		quit(false);
	}

	struct dirent* dirent;
	while((dirent = readdir(dir)) != NULL)
	{
		if(dirent->d_type != DT_LNK) { continue; }

		int ns = open(dirent->d_name, O_RDONLY);
		if(ns < 0)
		{
			if(errno == ENOENT) // no such namespace
			{
				continue;
			}
			else
			{
				fprintf(
					stderr, "Could not open %s: %s\n",
					dirent->d_name, strerror(errno)
				);
				quit(false);
			}
		}
		else
		{
			int setns_result = setns(ns, 0);
			int setns_error = errno;
			close(ns);
			if(setns_result == -1)
			{
				fprintf(
					stderr, "Could not setns %s: %s\n",
					dirent->d_name, strerror(setns_error)
				);

				if(!(strcmp(dirent->d_name, "user") == 0
					|| strcmp(dirent->d_name, "net") == 0))
				{
					quit(false);
				}
			}
		}
	}

quit:
	if(dir != NULL) { closedir(dir); }

	return exit_code;
}

int
main(int argc, char* argv[])
{
	int exit_code = EXIT_SUCCESS;

	struct optparse_long opts[] = {
		{"help", 'h', OPTPARSE_NONE},
		{"fork", 'f', OPTPARSE_NONE},
		RUN_CTX_OPTS,
		{0}
	};

	const char* help[] = {
		NULL, "Print this message",
		NULL, "Fork a new process inside sandbox",
		RUN_CTX_HELP,
	};

	const char* usage = "Usage: " PROG_NAME " [options] <pid> [command] [args]";

	int option;
	bool fork_before_exec = false;
	struct optparse options;
	struct run_ctx_s run_ctx;

	init_run_ctx(&run_ctx, argc);
	optparse_init(&options, argv);
	options.permute = 0;

	while((option = optparse_long(&options, opts, NULL)) != -1)
	{
		switch(option)
		{
			case 'h':
				optparse_help(usage, opts, help);
				quit(EXIT_SUCCESS);
				break;
			case 'f':
				fork_before_exec = true;
				break;
			CASE_RUN_OPT:
				if(!parse_run_option(&run_ctx, PROG_NAME, option, options.optarg))
				{
					quit(EXIT_FAILURE);
				}
				break;
			case '?':
				fprintf(stderr, PROG_NAME ": %s\n", options.errmsg);
				quit(EXIT_FAILURE);
				break;
			default:
				fprintf(stderr, "Unimplemented option\n");
				quit(EXIT_FAILURE);
				break;
		}
	}

	const char* pid = parse_run_command(&run_ctx, &options);

	if(pid == NULL)
	{
		fprintf(stderr, PROG_NAME ": must provide sandbox PID\n");
		quit(EXIT_FAILURE);
	}

	if(!enter_sandbox(pid)) { quit(EXIT_FAILURE); }

	if(fork_before_exec)
	{
		pid_t child = vfork();
		if(child == -1)
		{
			perror("vfork() failed");
			quit(EXIT_FAILURE);
		}
		else if(child == 0) // child
		{
			if(prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) == -1)
			{
				perror("Could not set parent death signal");
				exit(EXIT_FAILURE);
			}

			if(!execute_run_ctx(&run_ctx)) { exit(EXIT_FAILURE); }

			exit(EXIT_SUCCESS);// unreachable
		}
		else // parent
		{
			if(!drop_privileges(&run_ctx)) { quit(EXIT_FAILURE); }

			int status;
			errno = 0;
			while(waitpid(child, &status, 0) != child && errno == EINTR) {}

			quit(
				WIFEXITED(status) ?
				WEXITSTATUS(status) : (128 + WTERMSIG(status))
			);
		}
	}
	else
	{
		if(!execute_run_ctx(&run_ctx)) { quit(EXIT_FAILURE); }
	}
quit:
	cleanup_run_ctx(&run_ctx);

	return exit_code;
}
