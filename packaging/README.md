# Debian package build

This directory contains a minimal local Debian package builder for Terminal2.

For the broadest Ubuntu compatibility, build the package on the oldest target
release, currently Ubuntu 22.04. A package built on Ubuntu 22.04 should also be
usable on Ubuntu 24.04 when the listed runtime dependencies are available.

Install build dependencies:

```sh
sudo apt update
sudo apt install build-essential qt5-qmake qtbase5-dev qtserialport5-dev \
  libqt5svg5-dev libqt5opengl5-dev libqwt-qt5-dev
```

Build the package:

```sh
./packaging/build-deb.sh
```

Install the generated package:

```sh
sudo apt install ./dist/terminal2_*_amd64.deb
```
