# Build rsync-huai for Debian

This build process is tested in Debian 11 and 12 container.

1. Start container and install build dependencies

Debian 11:

```bash
docker run -it -v $(pwd):/workspace --rm debian:11 bash
> # add deb-src
> sed -i '/^deb / {p;s/^deb /deb-src /}' /etc/apt/sources.list
> apt update && apt upgrade -y && apt build-dep -y rsync && apt install -y git python3 python3-cmarkgfm devscripts
```

Debian 12:

```bash
docker run -it -v $(pwd):/workspace --rm debian:12 bash
> # add deb-src (DEB822)
> sed -i 's/Types: deb/Types: deb deb-src/g' /etc/apt/sources.list.d/debian.sources
> apt update && apt upgrade -y && apt build-dep -y rsync && apt install -y git devscripts
```

(Create a new user with same UID and GID as your host user if you want to avoid permission issues.)

2. Start build

In `/workspace`:

```bash
distro=$(grep -Po 'VERSION="[0-9]+ \(\K[^)]+' /etc/os-release)
export EMAIL="Your Name <email@example.com>"
dch -b --newversion "$(dpkg-parsechangelog --show-field Version)~${distro}1" "Build for ${distro}."
dpkg-buildpackage -b -rfakeroot -us -uc
```

Built packages are in `/`.

3. Cleanup

```bash
dpkg-buildpackage -rfakeroot -Tclean
# you may need to add path to safe directory by `git config --global --add safe.directory /workspace`
git checkout .
```
