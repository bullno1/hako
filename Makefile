CFLAGS += -Wall -Wextra -pedantic -Wno-missing-field-initializers -Werror -std=c99 -O3 -g

all: hako-run hako-enter

hako-%: src/hako-%.c
	$(CC) $(CFLAGS) -o $@ $<
