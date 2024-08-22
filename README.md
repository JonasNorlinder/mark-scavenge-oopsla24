# Mark-Scavenge: Waiting for Trash to Take Itself Out

This repository contains MS-ZGC, an implementation of mark-scavenge, presented in the paper [Mark-Scavenge: Waiting for Trash to Take Itself Out](https://doi.org/10.1145/3689791) during OOPSLA'24.

## Comparing patches

`mark-scavenge` contains the main contribution.

* [Mark-scavenge patch](https://github.com/JonasNorlinder/mark-scavenge-oopsla24/compare/jdk...mark-scavenge) ...
or `git diff jdk mark-scavenge`

* [Livemap fix for mark-scavenge patch](https://github.com/JonasNorlinder/mark-scavenge-oopsla24/compare/mark-scavenge...mark-scavenge-livemap-fix) ...
or `git diff mark-scavenge mark-scavenge-livemap-fix`

## Notes on building

The binaries used in the paper was built with the following configuration
`bash configure --with-boot-jdk=/home/user/jdk-21 --with-native-debug-symbols=internal --disable-warnings-as-errors --with-extra-cxxflags=-std=gnu++17`. Note that only Linux/x64 has been tested, so your milage may vary on other platforms.

JDK 21 (for boot image) can be downloaded from [https://jdk.java.net/archive/](https://jdk.java.net/archive/).

## Notes on running

To run MS-ZGC, build the source from branch `mark-scavenge` and use `-XX:+UseZGC -XX:+ZGenerational`.

Other GCs that is not MS-ZGC should be run using a build from the branch `jdk`. In the paper we have only evaluated ZGC generational (default mode is non-generational ZGC). To run generational ZGC use `-XX:+UseZGC -XX:+ZGenerational`.
