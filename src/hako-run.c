#define _GNU_SOURCE
#include <inttypes.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>
#include <alloca.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#define OPTPARSE_IMPLEMENTATION
#define OPTPARSE_API static
#include "optparse.h"
#define OPTPARSE_HELP_IMPLEMENTATION
#define OPTPARSE_HELP_API static
#include "optparse-help.h"
#include "hako-common.h"

#define HAKO_DIR ".hako"
#define PROG_NAME "hako-run"
#define quit(code) exit_code = code; goto quit;

struct sandbox_cfg_s
{
	const char* sandbox_dir;
	struct bindmnt_s* mounts;
	bool readonly;
	struct run_ctx_s run_ctx;
};

static int
sandbox_entry(void* arg)
{
	int exit_code = EXIT_SUCCESS;

	const struct sandbox_cfg_s* sandbox_cfg = arg;

	if(prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) == -1)
	{
		perror("Could not set parent death signal");
		quit(EXIT_FAILURE);
	}

	if(mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) == -1)
	{
		perror("Could not make root mount private");
		quit(EXIT_FAILURE);
	}

	if(mount(
		sandbox_cfg->sandbox_dir, sandbox_cfg->sandbox_dir,
		NULL, MS_BIND | MS_REC, NULL
	) == -1)
	{
		perror("Could not turn sandbox into a mountpoint");
		quit(EXIT_FAILURE);
	}

	if(chdir(sandbox_cfg->sandbox_dir) == -1)
	{
		perror("Could not chdir into sandbox");
		quit(EXIT_FAILURE);
	}

	pid_t init_pid = vfork();
	if(init_pid < 0)
	{
		perror("vfork() failed");
		quit(EXIT_FAILURE);
	}
	else if(init_pid == 0) // child
	{
		if(prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) == -1)
		{
			perror("Could not set parent death signal");
			quit(EXIT_FAILURE);
		}

		char* init_cmd[] = { HAKO_DIR "/init", NULL };
		if(execv(init_cmd[0], init_cmd) == -1)
		{
			perror("Could not execute " HAKO_DIR "/init");
			quit(EXIT_FAILURE);
		}
	}
	else // parent
	{
		int init_status;
		errno = 0;
		while(waitpid(init_pid, &init_status, 0) != init_pid && errno == EINTR)
		{ }

		if(!WIFEXITED(init_status) || WEXITSTATUS(init_status) != 0)
		{
			fprintf(
				stderr, HAKO_DIR "/init failed with %s: %d\n",
				WIFEXITED(init_status) ? "status" : "signal",
				WIFEXITED(init_status) ? WEXITSTATUS(init_status) : WTERMSIG(init_status)
			);
			quit(EXIT_FAILURE);
		}
	}

	if(sandbox_cfg->readonly
		&& mount(NULL, ".", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) == -1)
	{
		perror("Could not make sandbox read-only");
		quit(EXIT_FAILURE);
	}

	if(syscall(__NR_pivot_root, ".", HAKO_DIR) == -1)
	{
		perror("Could not pivot root");
		quit(EXIT_FAILURE);
	}

	if(chdir("/") == -1)
	{
		perror("Could not chdir into new root");
		quit(EXIT_FAILURE);
	}

	if(umount2(HAKO_DIR, MNT_DETACH) == -1)
	{
		perror("Could not unmount old root");
		quit(EXIT_FAILURE);
	}

	if(!execute_run_ctx(&sandbox_cfg->run_ctx))
	{
		quit(EXIT_FAILURE);
	}

quit:
	return exit_code;
}

int
main(int argc, char* argv[])
{
	(void)argc;

	int exit_code = EXIT_SUCCESS;

	struct optparse_long opts[] = {
		{"help", 'h', OPTPARSE_NONE},
		{"read-only", 'R', OPTPARSE_NONE},
		{"pid-file", 'p', OPTPARSE_REQUIRED},
		RUN_CTX_OPTS,
		{0}
	};

	const char* help[] = {
		NULL, "Print this message",
		NULL, "Make sandbox filesystem read-only",
		"FILE", "Write pid of sandbox to this file",
		RUN_CTX_HELP,
	};

	const char* usage = "Usage: " PROG_NAME " [options] <target> [--] [command] [args]";

	int option;
	const char* pid_file = NULL;
	struct optparse options;
	struct sandbox_cfg_s sandbox_cfg = { 0 };
	init_run_ctx(&sandbox_cfg.run_ctx, argc);
	optparse_init(&options, argv);

	while((option = optparse_long(&options, opts, NULL)) != -1)
	{
		switch(option)
		{
			case 'h':
				optparse_help(usage, opts, help);
				quit(EXIT_SUCCESS);
				break;
			case 'R':
				sandbox_cfg.readonly = true;
				break;
			case 'p':
				pid_file = options.optarg;
				break;
			CASE_RUN_OPT:
				if(!parse_run_option(
					&sandbox_cfg.run_ctx, PROG_NAME, option, options.optarg
				))
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

	sandbox_cfg.sandbox_dir = parse_run_command(
		&sandbox_cfg.run_ctx, &options, "/sbin/init"
	);

	if(sandbox_cfg.sandbox_dir == NULL)
	{
		fprintf(stderr, PROG_NAME ": must provide sandbox dir\n");
		quit(EXIT_FAILURE);
	}

	// Create a child process in a new namespace
	long stack_size = sysconf(_SC_PAGESIZE);
	char* child_stack = alloca(stack_size);
	int clone_flags = 0
		| SIGCHLD
		| CLONE_VFORK // wait until child execs away
		| CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWUTS
		| CLONE_NEWNET;
	pid_t child_pid = clone(
		sandbox_entry, child_stack + stack_size, clone_flags, &sandbox_cfg
	);
	if(child_pid == -1)
	{
		perror("clone() failed");
		quit(EXIT_FAILURE);
	}

	if(!drop_privileges(&sandbox_cfg.run_ctx)) { quit(EXIT_FAILURE); }

	if(pid_file != NULL)
	{
		FILE* file = fopen(pid_file, "w");
		if(file == NULL)
		{
			perror("Could not open pid file for writing");
			quit(EXIT_FAILURE);
		}

		bool written = fprintf(file, "%" PRIdMAX, (intmax_t)child_pid) > 0;
		bool closed = fclose(file) == 0;
		if(!(written && closed))
		{
			fprintf(stderr, "Could not write pid to file\n");
			quit(EXIT_FAILURE);
		}
	}

	sigset_t set;
	sigfillset(&set);
	sigprocmask(SIG_BLOCK, &set, NULL);
	for(;;)
	{
		int sig, status;
		sigwait(&set, &sig);
		switch(sig)
		{
			case SIGINT:
			case SIGTERM:
			case SIGHUP:
			case SIGQUIT:
				// Kill child manualy because SIGKILL from PR_SET_PDEATHSIG can
				// be handled (and ignored) by child.
				kill(child_pid, SIGKILL);
				quit(128 + sig);
				break;
			case SIGCHLD:
				if(waitpid(child_pid, &status, WNOHANG) > 0)
				{
					quit(
						WIFEXITED(status) ?
						WEXITSTATUS(status) : (128 + WTERMSIG(status))
					);
				}
				break;
		}
	}

quit:
	cleanup_run_ctx(&sandbox_cfg.run_ctx);

	return exit_code;
}
