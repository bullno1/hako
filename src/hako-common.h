#ifndef HAKO_COMMON_H
#define HAKO_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include "optparse.h"

#define CASE_RUN_OPT case 'L': case 'u': case 'g': case 'e': case 'c'
#define RUN_CTX_OPTS \
	{"env", 'e', OPTPARSE_REQUIRED}, \
	{"user", 'u', OPTPARSE_REQUIRED}, \
	{"group", 'g', OPTPARSE_REQUIRED}, \
	{"lock-privs", 'L', OPTPARSE_NONE}, \
	{"chdir", 'c', OPTPARSE_REQUIRED}

#define RUN_CTX_HELP \
	"NAME=VALUE", "Set environment variable inside sandbox", \
	"USER", "Run as this user", \
	"GROUP", "Run as this group", \
	NULL, "Prevent further privilege escalation", \
	"DIR", "Change to this directory inside sandbox"

struct run_ctx_s
{
	uid_t uid;
	gid_t gid;
	bool lock_privs;
	const char* work_dir;
	unsigned int env_len;
	char** env;
	char** command;
};

static bool
strtonum(const char* str, long* number)
{
	char* end;
	*number = strtol(str, &end, 10);
	return *end == '\0';
}

static void
init_run_ctx(struct run_ctx_s* run_ctx, int argc)
{
	*run_ctx = (struct run_ctx_s){
		.uid = (uid_t)-1,
		.gid = (gid_t)-1,
		.env = calloc(argc / 2, sizeof(char*)),
		.command = calloc(argc, sizeof(char*))
	};
}

static void
cleanup_run_ctx(struct run_ctx_s* run_ctx)
{
	free(run_ctx->command);
	free(run_ctx->env);
}

static bool
parse_run_option(
	struct run_ctx_s* run_ctx,
	const char* prog_name,
	char option,
	char* optarg
)
{
	long num;
	switch(option)
	{
		case 'L':
			run_ctx->lock_privs = true;
			return true;
		case 'u':
			if(strtonum(optarg, &num) && num >= 0)
			{
				run_ctx->uid = (uid_t)num;
			}
			else
			{
				struct passwd* pwd = getpwnam(optarg);
				if(pwd != NULL)
				{
					run_ctx->uid = pwd->pw_uid;
				}
				else
				{
					fprintf(stderr, "%s: invalid user: %s\n", prog_name, optarg);
					return false;
				}
			}
			return true;
		case 'g':
			if(strtonum(optarg, &num) && num >= 0)
			{
				run_ctx->gid = (gid_t)num;
			}
			else
			{
				struct group* grp = getgrnam(optarg);
				if(grp != NULL)
				{
					run_ctx->gid = grp->gr_gid;
				}
				else
				{
					fprintf(stderr, "%s: invalid group: %s\n", prog_name, optarg);
					return false;
				}
			}
			return true;
		case 'e':
			run_ctx->env[run_ctx->env_len++] = optarg;
			return true;
		case 'c':
			run_ctx->work_dir = optarg;
			return true;
		default:
			fprintf(stderr, "%s: invalid option: %c\n", prog_name, option);
			return false;
	}
}

static bool
drop_privileges(const struct run_ctx_s* run_ctx)
{
	uid_t uid = run_ctx->uid;
	uid_t gid = run_ctx->gid;

	if((uid != (uid_t)-1 || gid != (gid_t)-1) && setgroups(0, NULL) == -1)
	{
		perror("setgroups(0, NULL) failed");
		return false;
	}

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

	return true;
}

static const char*
parse_run_command(
	struct run_ctx_s* run_ctx, struct optparse* options, char* default_cmd
)
{
	int command_argc = 0;
	const char* target = NULL;
	const char* arg;
    while((arg = optparse_arg(options)))
	{
		if(target == NULL)
		{
			target = arg;
		}
		else
		{
			run_ctx->command[command_argc++] = (char*)arg;
		}
	}

	if(command_argc == 0)
	{
		run_ctx->command[0] = default_cmd;
	}

	return target;
}

static bool
execute_run_ctx(const struct run_ctx_s* run_ctx)
{
	if(!drop_privileges(run_ctx)) { return false; }

	if(run_ctx->lock_privs && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1)
	{
		perror("Could not lock privileges");
		return false;
	}

	if(run_ctx->work_dir != NULL && chdir(run_ctx->work_dir) == -1)
	{
		perror("chdir() failed");
		return false;
	}

	if(execve(run_ctx->command[0], run_ctx->command, run_ctx->env) == -1)
	{
		perror("execve() failed");
		return false;
	}

	return true;
}

#endif
