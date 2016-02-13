# YaRF

RioFS is a userspace filesystem that allows administrators to mount Amazon S3 buckets on \*nix boxes as local paths using the FUSE driver. YaRF is nothing more than *Y*et *A*nother *R*ioFS *F*ork.

## Differences

There's no point in forking a project that already does what you want, so here are a few of the reasons that led me to fork YaRF:

1. Support for mounting prefixes within buckets  
   ie: `riofs bucket-name/arbitrary/prefix/here/ localdir` (note trailing slash)


2. Better MIME typing on created objects by using `/etc/mime.types` instead of `libmagic`, which is useful when using S3's static web server

3. Optional setting of a `Cache-Control` header which helps when using S3's static web server

### Known Issues

1. Appending data to an existing file is not supported (this is an S3 limitation)

2. Folder renaming is not supported (inherited limitation)

3. YaRF/RioFS is a leaky abstraction. There are several POSIXy things that aren't (and probably will never) be supported.

4. YaRF seems to dislike 0 byte files when mounting a bucket without a prefix. It's on my list.

5. Sometimes killing the riofs won't remove the mount from the kernel's mount table requiring a manual `umount`. Seems to be more an issue with CentOS' antiquated FUSE than Rio.

6. With neither `libmagic` nor `/etc/mime.types` everything is probably uploaded as `text/plain`.

### Dependencies

As YaRF is basically a fork of RioFS, it has the same dependencies as the upstream project:

* glib >= 2.22
* fuse >= 2.73
* libevent >= 2.0
* libxml >= 2.6
* libcrypto >= 0.9
* libmagic (optional, with --with-libmagic=*PATH*)

### Tested Platforms

I've managed to build and use both YaRF and RioFS without any major issues on the following platorms:

* CentOS 6.6 and 6.7
* Ubuntu 14.04 and 15.04
* Amazon Linux v1.4.3 and v2.0.6

## Building YaRF

Building YaRF differs from building regular RioFS only when you want to use `/etc/mime.types` instead of `libmagic`. The rest of the modifications (`Cache-Control` and bucket prefix support) are always available.

### Configure Options

**--enable-debug**  
Creates a debug build of RioFS/YaRF.

**--with-libmagic**  
Use `libmagic` for guessing MIME types of created objects.

**--with-mimetypes**  
Use the `/etc/mime.types` to guess the MIME types of created objects. Using this implies `--with-libmagic=no`

### Examples

#### Vanilla RioFS

```
./autogen.sh
./configure
make
sudo make install
```

#### With `libmagic` MIME types

```
./autogen.sh
./configure --with-libmagic
make
sudo make install
```

#### With `/etc/mime.types` MIME types

```
./autogen.sh
./configure --with-mimetypes
make
sudo make install
```

## Usage

The most basic way of using YaRF is:

```
export AWS_ACCESS_KEY_ID="your access key"  
export AWS_SECRET_ACCESS_KEY="your secret"
riofs my-bucket[/optional/path/] localdir
```

### Options

**-v** Verbose output  
**-f** Run in foreground  
**-c [file]** Use configuration file  
**-o "opt[,opt...]"** FUSE options  
**-l [path]** Log file to use  
**--uid [uid]** UID of filesystem owner  
**--gid [gid]** GID of filesystem owner  
**--fmode [octal]** Mode for files  
**--dmode [octal]** Mode for directories


### Statistics Server

Haven't touched this, never used it, but all you'd ever want to know is in `riofs.xml.conf`.

### Signals

**SIGUSR1** Re-read the configuration file  
**SIGUSR2** Re-open log file  
**SIGTERM** Unmount filesystem and terminate YaRF
