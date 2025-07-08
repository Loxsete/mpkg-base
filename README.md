# mypkg — a minimal package manager for Linux

## Features
- Installs .tar.xz packages from a remote repository (eg, https://loxsete.github.io/mpkg-server)
- Checks dependencies
- Tracks installed packages
- Config system

## Requirements
- libarchive

## Build
```
make
```

## Usage

- `cd /`
- `mypkg install <package>`
- `mypkg remove <package>`
- `mypkg list`               
- `mypkg info <package>`

## Config
Configure settings in **/etc/mpkg.conf** with these parameters:

**`PKG_DB_PATH`** -    Package database directory

**`PKG_CACHE_PATH`** - Package cache directory

**`PKG_REPO_URL`** -   Package repository URL

## Example config
```
PKG_DB_PATH=/var/db/mpkg
PKG_CACHE_PATH=/var/cache/mpkg
PKG_REPO_URL=https://loxsete.github.io/mpkg-server/
```
