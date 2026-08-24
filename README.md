# obese

the most bloated package manager on earth (self-proclaimed).

writes real LHA archives (`-lh5-`), bundles every dependency inside,
converts .deb files, recommends random software at every install,
forces you onto a mirror that never works, and switches your language
based on the system locale for no reason.

## build

requirements:

- a C++17 compiler (`g++` or `clang++`)
- `make`
- POSIX (linux recommended)
- no external libraries. none. we wrote our own LZH codec because of course we did.

```sh
make
```

produces two binaries:

```text
obese          the package manager
obese-server   the http repo server
```

make sure you do not pass `-O2`. we deliberately compile with `-O0 -g3`
so the binaries stay fat. you have been warned.

## usage

```sh
# package a directory into a real LHA archive (dependencies bundled inside)
./obese pkg <dir> -o out.ob

# install (from a file, the local repo, or whatever source you are stuck with)
./obese --root=./obe install out.ob

# run an installed package without sudo
./obese --root=./obe run <name>

# deb -> ob, downloading every dependency and bundling it
./obese deb2ob ./some.deb -o out.ob

# sources. only a server decides what sources exist. the bundled mirror
# can never be removed and never connects.
./obese-server ./repo 8080 primary
./obese --root=./obe source fetch http://127.0.0.1:8080
./obese --root=./obe source list

# switching sources requires ten warnings. yes, ten. press enter ten times.
./obese --root=./obe source use <name>

# language: auto-detected from $LANG, or force it
./obese --lang=zh ...
OBESE_LANG=en ./obese ...
```

most commands need `--root=<dir>` unless you run as root.

## the bloat, proudly

- real LHA `-lh5-` format. name and version live in the LHA header itself.
- every dependency (transitively) is bundled inside the package as `.ob`.
- installs recommend random other software. default answer is YES.
- rollback uninstalls the current version and installs an older one.
- a forced mirror (`http://obese.kochiya-sanae.icu:8080`) that is bundled
  forever and is unreachable. obviously.
- telemetry that goes nowhere and a cache that is never cleaned.
- auto-update that cannot be turned off. there is no config file.

## license

ALL RIGHTS RESERVED. you may not use this software for Good. you may not
use it for Evil either. you may not use it. probably.
