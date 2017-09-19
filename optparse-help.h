#ifndef OPTPARSE_HELP_H
#define OPTPARSE_HELP_H

#include "optparse.h"

#ifndef OPTPARSE_HELP_API
#	define OPTPARSE_HELP_API
#endif

OPTPARSE_HELP_API void
optparse_help(
	const char* title, const struct optparse_long* opts, const char* help[]
);

#ifdef OPTPARSE_HELP_IMPLEMENTATION

#include <string.h>
#include <stdio.h>

static inline unsigned int
optparse_help_opt_width_(const struct optparse_long opt, const char* param)
{
    return 0
        + 2 // "-o"
        + (opt.longname ? 3 + strlen(opt.longname) : 0) // ",--option",
        + (param ? (opt.argtype == OPTPARSE_REQUIRED ? 1 : 3) + strlen(param) : 0); //"[=param]" or " param"
}

void
optparse_help(
	const char* title, const struct optparse_long* opts, const char* help[]
)
{
	printf("%s\n\nAvailable options:\n\n", title);

    unsigned int max_width = 0;
    for(unsigned int i = 0;; ++i)
    {
        struct optparse_long opt = opts[i];
        if(!opt.shortname) { break; }

        unsigned int opt_width = optparse_help_opt_width_(opt, help[i * 2]);
        max_width = opt_width > max_width ? opt_width : max_width;
    }

    for(unsigned int i = 0;; ++i)
    {
        struct optparse_long opt = opts[i];
        if(!opt.shortname) { break; }

        printf("  -%c", opt.shortname);
        if(opt.longname)
        {
            printf(",--%s", opt.longname);
        }

        const char* param = help[i * 2];
        const char* description = help[i * 2 + 1];

        switch(opt.argtype)
        {
            case OPTPARSE_NONE:
                break;
            case OPTPARSE_OPTIONAL:
                printf("[=%s]", param);
                break;
            case OPTPARSE_REQUIRED:
                printf(" %s", param);
                break;
        }
		printf("%*s", max_width + 4 - optparse_help_opt_width_(opt, param), "");
        printf("%s\n", description);
    }
}

#endif

#endif
