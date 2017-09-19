#define _GNU_SOURCE
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#define OPTPARSE_IMPLEMENTATION
#define OPTPARSE_API static
#include "optparse.h"
#define OPTPARSE_HELP_IMPLEMENTATION
#define OPTPARSE_HELP_API static
#include "optparse-help.h"

#define BOXED_DIR ".boxed"
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
	unsigned int num_mounts;
	uid_t uid;
	gid_t gid;
	unsigned long pdeath_sig;
	const char* work_dir;
	char** command;
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
drop_privileges(uid_t uid, gid_t gid)
{

	if(gid != (gid_t)-1 && setgid(gid) == -1)
	{
		perror("setgid() failed");
		return false;
	}

	if(uid != (uid_t)-1 && setuid(uid) == -1)
	{
		perror("setuid() failed");
		return false;
	}

	if((uid != (uid_t)-1 || gid != (gid_t)-1) && prctl(PR_SET_NO_NEW_PRIVS, 1) == -1)
	{
		perror("prctl(PR_SET_NO_NEW_PRIVS, 1) failed");
		return false;
	}

	return true;
}

static int
child_fn(void* arg)
{
	int exit_code = EXIT_SUCCESS;

	const struct sandbox_cfg_s* sandbox_cfg = arg;
	char* old_root_path = NULL;

	if(prctl(PR_SET_PDEATHSIG, sandbox_cfg->pdeath_sig) == -1)
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

	if(asprintf(
		&old_root_path, "%s/" BOXED_DIR, sandbox_cfg->sandbox_dir
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

	for(unsigned int i = 0; i < sandbox_cfg->num_mounts; ++i)
	{
		struct bindmnt_s* mount_cfg = &sandbox_cfg->mounts[i];
		char* new_host_path = NULL;

		if(asprintf(
			&new_host_path, BOXED_DIR "/%s", mount_cfg->host_path
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

		if(mount_cfg->readonly && mount(
			NULL, mount_cfg->sandbox_path, NULL,
			MS_REMOUNT | MS_BIND | MS_RDONLY, NULL
		) == -1)
		{
			fprintf(
				stderr, "Could not make %s read-only: %s\n",
				mount_cfg->sandbox_path, strerror(errno)
			);
			quit(EXIT_FAILURE);
		}
	}

	if(umount2(BOXED_DIR, MNT_DETACH) == -1)
	{
		perror("Could not unmount old root");
		quit(EXIT_FAILURE);
	}

	if(!drop_privileges(sandbox_cfg->uid, sandbox_cfg->gid))
	{
		quit(EXIT_FAILURE);
	}

	if(sandbox_cfg->work_dir != NULL && chdir(sandbox_cfg->work_dir) == -1)
	{
		perror("chdir() failed");
		quit(EXIT_FAILURE);
	}

	if(execvp(sandbox_cfg->command[0], sandbox_cfg->command) == -1)
	{
		perror("execvp() failed");
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
		{"read-only", 'r', OPTPARSE_OPTIONAL},
		{"user", 'u', OPTPARSE_REQUIRED},
		{"group", 'g', OPTPARSE_REQUIRED},
		{"chdir", 'c', OPTPARSE_REQUIRED},
		{"pid-file", 'p', OPTPARSE_REQUIRED},
		{"kill-signal", 's', OPTPARSE_REQUIRED},
		{0}
	};

	const char* help[] = {
		NULL, "Print this message",
		"HOST:SANDBOX[:ro/rw]", "Bind mount a file to sandbox",
		"true/false", "Make sandbox filesystem read-only",
		"USER", "Change to this user",
		"GROUP", "Change to this group after creating sandbox",
		"DIR", "Change to this directory inside sandbox",
		"FILE", "Write pid of sandbox to this file",
		"SIGNAL", "Signal to kill sandbox (default: SIGKILL)",
	};

	const char* usage = "Usage: boxed-run [options] <target> [--] [command] [args]";

	int option;
	char* child_stack = NULL;
	struct optparse options;
	struct sandbox_cfg_s sandbox_cfg = {
		.command = malloc(argc * sizeof(const char*)),
		.mounts = malloc(argc / 2 * sizeof(struct bindmnt_s)),
		.pdeath_sig = SIGKILL,
		.num_mounts = 0,
		.uid = (uid_t)-1,
		.gid = (uid_t)-1,
	};
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
					options.optarg, &sandbox_cfg.mounts[sandbox_cfg.num_mounts++]
				))
				{
					fprintf(stderr, "boxed-run: invalid mount: %s\n", options.optarg);
					quit(EXIT_FAILURE);
				}
				break;
			case 'c':
				sandbox_cfg.work_dir = options.optarg;
				break;
			case '?':
				fprintf(stderr, "boxed-run: %s\n", options.errmsg);
				quit(EXIT_FAILURE);
				break;
			default:
				fprintf(stderr, "Unimplemented option\n");
				quit(EXIT_FAILURE);
				break;
		}
	}

	const char* arg;
	int command_argc = 0;
    while((arg = optparse_arg(&options)))
	{
		if(sandbox_cfg.sandbox_dir == NULL)
		{
			sandbox_cfg.sandbox_dir = arg;
		}
		else
		{
			sandbox_cfg.command[command_argc++] = (char*)arg;
		}
	}
	sandbox_cfg.command[command_argc] = NULL;

	if(sandbox_cfg.sandbox_dir == NULL)
	{
		fprintf(stderr, "boxed-run: must provide sandbox dir\n");
		quit(EXIT_FAILURE);
	}

	if(command_argc == 0)
	{
		sandbox_cfg.command[0] = "/bin/sh";
		sandbox_cfg.command[1] = NULL;
	}

	// Create a child process in a new namespace
	long stack_size = sysconf(_SC_PAGESIZE) * 8;
	child_stack = malloc(stack_size);
	int clone_flags = 0
		| SIGCHLD
		| CLONE_VFORK // wait until child execs away
		| CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWUTS
		| CLONE_NEWNET;
	pid_t child_pid = clone(
		child_fn, child_stack + stack_size, clone_flags, &sandbox_cfg
	);
	if(child_pid == -1)
	{
		perror("clone() failed");
		quit(EXIT_FAILURE);
	}

	if(!drop_privileges(sandbox_cfg.uid, sandbox_cfg.gid))
	{
		quit(EXIT_FAILURE);
	}

	int child_ret;
	errno = 0;
	while(waitpid(child_pid, &child_ret, 0) != child_pid && errno == EINTR) { }

	quit(WIFEXITED(child_ret) ? WEXITSTATUS(child_ret) : EXIT_FAILURE);

quit:
	for(size_t i = 0; i < sandbox_cfg.num_mounts; ++i)
	{
		free(sandbox_cfg.mounts[i].host_path);
		free(sandbox_cfg.mounts[i].sandbox_path);
	}
	free(sandbox_cfg.mounts);
	free(sandbox_cfg.command);
	free(child_stack);

	return exit_code;
}
