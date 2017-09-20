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

struct bindmnt_s
{
	char* host_path;
	char* sandbox_path;
	bool readonly;
};

struct sandbox_cfg_s
{
	const char* sandbox_dir;
	struct bindmnt_s* mounts;
	bool readonly;
	struct run_ctx_s run_ctx;
};

static bool
parse_mount(const char* mount_spec_, struct bindmnt_s* mount_cfg)
{
	bool exit_code = true;
	char* saveptr = NULL;
	char* mount_spec = strdup(mount_spec_);
	unsigned int i;
	char* token;

	memset(mount_cfg, 0, sizeof(*mount_cfg));
	for(
		i = 0, token = strtok_r(mount_spec, ":", &saveptr);
		token != NULL;
		token = strtok_r(NULL, ":", &saveptr), ++i
	)
	{
		switch(i)
		{
			case 0:
				mount_cfg->host_path = strdup(token);
				break;
			case 1:
				mount_cfg->sandbox_path = strdup(token);
				break;
			case 2:
				if(strcmp(token, "ro") == 0)
				{
					mount_cfg->readonly = true;
				}
				else if(strcmp(token, "rw") == 0)
				{
					mount_cfg->readonly = false;
				}
				else
				{
					quit(false);
				}
				break;
			default:
				quit(false);
				break;
		}
	}

	if(mount_cfg->host_path == NULL || mount_cfg->sandbox_path == NULL)
	{
		quit(false);
	}

quit:
	free(mount_spec);

	return exit_code;
}

static bool
make_mount_readonly(const char* path)
{
	return mount(NULL, path, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) == 0;
}

static int
sandbox_entry(void* arg)
{
	int exit_code = EXIT_SUCCESS;

	const struct sandbox_cfg_s* sandbox_cfg = arg;
	char* old_root_path = NULL;

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

	if(sandbox_cfg->readonly && !make_mount_readonly(sandbox_cfg->sandbox_dir))
	{
		perror("Could not make sandbox read-only");
		quit(EXIT_FAILURE);
	}

	if(asprintf(
		&old_root_path, "%s/" HAKO_DIR, sandbox_cfg->sandbox_dir
	) == -1)
	{
		perror(NULL);
		quit(EXIT_FAILURE);
	}

	if(syscall(__NR_pivot_root, sandbox_cfg->sandbox_dir, old_root_path) == -1)
	{
		perror("Could not pivot root");
		quit(EXIT_FAILURE);
	}

	if(chdir("/") == -1)
	{
		perror("Could not chdir into new root");
		quit(EXIT_FAILURE);
	}

	struct bindmnt_s* mount_cfg;
	unsigned int i;
	for(
		i = 0, mount_cfg = &sandbox_cfg->mounts[i];
		mount_cfg->sandbox_path != NULL;
		++i, mount_cfg = &sandbox_cfg->mounts[i])
	{
		char* new_host_path = NULL;

		if(asprintf(
			&new_host_path, HAKO_DIR "/%s", mount_cfg->host_path
		) == -1)
		{
			perror(NULL);
			quit(EXIT_FAILURE);
		}

		int mount_result = mount(
			new_host_path, mount_cfg->sandbox_path, NULL, MS_BIND | MS_REC, NULL
		);
		free(new_host_path);

		if(mount_result == -1)
		{
			fprintf(
				stderr, "Could not mount %s to %s: %s\n",
				mount_cfg->host_path, mount_cfg->sandbox_path, strerror(errno)
			);
			quit(EXIT_FAILURE);
		}

		if(mount_cfg->readonly && !make_mount_readonly(mount_cfg->sandbox_path))
		{
			fprintf(
				stderr, "Could not make %s read-only: %s\n",
				mount_cfg->sandbox_path, strerror(errno)
			);
			quit(EXIT_FAILURE);
		}
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
	free(old_root_path);

	return exit_code;
}

int
main(int argc, char* argv[])
{
	(void)argc;

	int exit_code = EXIT_SUCCESS;

	struct optparse_long opts[] = {
		{"help", 'h', OPTPARSE_NONE},
		{"mount", 'm', OPTPARSE_REQUIRED},
		{"read-only", 'R', OPTPARSE_NONE},
		{"pid-file", 'p', OPTPARSE_REQUIRED},
		RUN_CTX_OPTS,
		{0}
	};

	const char* help[] = {
		NULL, "Print this message",
		"HOST:SANDBOX[:ro/rw]", "Bind mount a file to sandbox",
		NULL, "Make sandbox filesystem read-only",
		"FILE", "Write pid of sandbox to this file",
		RUN_CTX_HELP,
	};

	const char* usage = "Usage: " PROG_NAME " [options] <target> [--] [command] [args]";

	int option;
	const char* pid_file = NULL;
	unsigned int num_mounts = 0;
	struct optparse options;
	struct sandbox_cfg_s sandbox_cfg = {
		.mounts = calloc(argc / 2, sizeof(struct bindmnt_s)),
	};
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
			case 'm':
				if(!parse_mount(
					options.optarg, &sandbox_cfg.mounts[num_mounts++]
				))
				{
					fprintf(stderr, PROG_NAME ": invalid mount: %s\n", options.optarg);
					quit(EXIT_FAILURE);
				}
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
	for(size_t i = 0; i < num_mounts; ++i)
	{
		free(sandbox_cfg.mounts[i].host_path);
		free(sandbox_cfg.mounts[i].sandbox_path);
	}
	free(sandbox_cfg.mounts);
	cleanup_run_ctx(&sandbox_cfg.run_ctx);

	return exit_code;
}
