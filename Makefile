CFLAGS += -Wall -Wextra -pedantic -Wno-missing-field-initializers -Werror -std=c99 -O3 -g

all: boxed-run

boxed-run: boxed-run.c
	$(CC) $(CFLAGS) -o boxed-run boxed-run.c
