CFLAGS += -Wall -Wextra -pedantic -Wno-missing-field-initializers -Werror -std=c99 -O3 -g

all: hako-run

hako-run: hako-run.c
	$(CC) $(CFLAGS) -o hako-run hako-run.c
