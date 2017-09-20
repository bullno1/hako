# hako - A minimal sandboxing tool

[![License](https://img.shields.io/badge/license-BSD-blue.svg)](LICENSE)

`hako` = chroot + Linux namespace.

It is created out of a need for a simple tool like `chroot` but with extra isolation.

## What it does

- It generally works like `chroot` with the added benefit of isolation using Linux namespace.
- It can bind mount files from outside the sandbox and clean up on termination.
- It can run on a read-only filesystem.
- Some rudimentary form of privilege dropping through setuid, setgid and [`PR_SET_NO_NEW_PRIVS`](https://www.kernel.org/doc/Documentation/prctl/no_new_privs.txt).

## What it does not do

- Networking: use docker/runc instead.
  Alternatively, Unix socket works for sandboxes in the same host too.
- Seccomp: I might start a new project for this if needed.
  Something like `seccomp-exec <rule-file> <command> [args]` would be nice.

## Build requirements

- A C99 compiler (gcc/clang)
- Recent Linux headers
- make

## Usage

### Creating a sandbox

```sh
mkdir sandbox
mkdir sandbox/.hako # Must be present for hako-run
mkdir sandbox/bin
touch sandbox/bin/busybox
ln -s busybox sandbox/bin/sh
```

Run it with:

```sh
hako-run --mount $(which busybox):/bin/busybox:ro sandbox /bin/sh
```

General syntax is: `hako-run [options] <target> [--] [command] [args]`.

If `command` is not given, it will default to `/sbin/init`.

Run `hako-run --help` for more info.

### Entering an existing sandbox

Given:

```sh
hako-run --pid-file sandbox.pid sandbox
```

One can enter the sandbox with:

```sh
hako-enter --fork $(cat sandbox.pid) /bin/sh
```

General syntax is: `hako-enter [options] <pid> [--] [command] [args]`.

If `command` is not given, it will default to `/bin/sh`.

Run `hako-enter --help` for more info.

## FAQ

### Why not docker?

Docker does too many things.
It also requires a daemon running.
While it's possible to use it without building image, it's just annoying in general.

### Why not runc (aka: Docker, the good part)?

`runc` looks good but I only need something a little more than `chroot` that runs only on Linux.
I rather like the idea of simple Unix tools and [Bernstein chaining](http://www.catb.org/~esr/writings/taoup/html/ch06s06.html).
If I need features like seccomp, I'd probably write a separate chain wrapper for it.

### Why not systemd-nspawn?

1. It requires glibc, according to buildroot. `hako` can be built with musl.
2. While I'm sure it can be used standalone, it comes with a bunch of dependencies from the systemd project.
3. It's systemd (jk).

### Why must the sandbox contains an empty .hako directory?

[`pivot_root`](https://linux.die.net/man/8/pivot_root) requires it.
It also provides access to the old root filesystem while creating the sandbox.
`runc` relies on an [undocumented trick](https://github.com/opencontainers/runc/blob/593914b8bd5448a93f7c3e4902a03408b6d5c0ce/libcontainer/rootfs_linux.go#L635) but I'd rather not.

### How to build with musl?

`CC='musl-gcc -static' make`
