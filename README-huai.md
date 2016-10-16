# About rsync-huai

## What is rsync-huai

It is a modified version of rsync which can reduce the IO of a rsync server.

## How does it work

It can store the attributes, modes and so on of the entire file tree in memory, avoiding recursively generating a list of the file tree from the block device.

# Detailed Introduction

As we all know, rsync is a good tool for mirroring things. Various features of it are relied by mirror administrators. Most importantly, it is needed to find out what get changed and only transfer them. Besides, we need `rsync` to keep the `modtime`, `mode`, `owner` the same. Also, some may add a `exclude-from` list to mirror files only needed or `delete-after` to delete files deleted by the upstream after fetching new ones. 

However, providing rsync service for other mirror sites can be a desaster. When a client begins to `rsync` file from a server, the first thing the server do is recursively generating a list of the file tree including all the names and other information of the files and directories. Obviously, the process involves repeatedly seeking the disks, which leads to bad performance of disk io. 

Take the centos repo for example. The command `rsync --dry-run` is used to make the rsync server generate the list and exit, without doing any real data transfer. Cgroup is also used to count the bytes read and IO operations. And a wrapper script is written to put the server process inside the cgroup. 

```
% cd /sys/fs/cgroup/blkio
% mkdir rsyncd
% cat /tmp/wrap.sh
#!/bin/bash
shift
exec cgexec -g blkio:rsyncd "$@"
% echo 3 > /proc/sys/vm/drop_caches 
% echo 1 > blkio.reset_stats
% rsync -an :/path/to/local/centos_repo/ /path/to/new/repo/ --rsh="/tmp/wrap.sh"
% grep . blkio.throttle.io* 

# Here are the counter values
```

Here is the result. For comparison, `find .`, `du -shxc .` and `cat centos/7.2.1511/isos/x86_64/CentOS-7-x86_64-DVD-1511.iso > /dev/null` are also executed and measured.

Command  | Read IO Ops | Bytes Read | Bytes per IO Op
---------|-------------|------------|-----------------
`rsync`  | 10538       | 43298816   | 4108.83
`find ./`| 3871        | 16449536   | 4249.43
`du -shxc` | 10448     | 42905600   | 4106.58
`cat`    | 16525       | 4329652224 | 262006.19

It's no doubt that, traversing the directory tree generates large amount of fragmented IO operatons.

I once came up with an idea that maintaining the meta data inside a git repo. After a successful update, the server calculates and stores the meta into and generates a commit. The client, simply doing `git pull` and `git diff`, can easily know the difference and can only request the files changed with a `--files-from` option. However, this approach needs modification on the client side, which cannot be easily adapted.

Considering the compatibility of standard rsync clients, generating the list is not avoidable. Noticing it is only file attributes that are needed to do that, such information can be stored in a faster storage and hard disks are visited only when the real content of some file is requested. 

First, a new option `--only-send-attrs` is added to the rsync command. When enabled, it will work as if the content of each regular file from the source is replaced by its size. As a result, we can get a directory tree with the same `modtime`, `mode`, `owner` information with the source, except the size. The size information, however, is stored in the content. This tree can be stored on things like `tmpfs`. We call this "attr tree" for short.

Secondly, a new config item `real file prefix` is add to `rsyncd.conf`. A rsync daemon provides service with the "attr tree", and thus can correctly generate the list. When requested for a certain file, the rsync daemon prepend the `real file prefix` before the path, and can find the correct path to the content of the file.

The code of rsync is so ill-organized that I did some ugly modifications to the code. Here is a brief how-to:

Suppose originally the repos on the server are stored like this:

```
/storage
 |
 +-foo/
 +-bar/
 +-barz/
```

You need create a new directory, and move them inside.

```
/storage
 |
 +-mirrors
   |
   +-foo/
   +-bar/
   +-barz/
```

Now, you can generate the "attr tree"

```
cd /storage
mkdir mirror-attrs
mount -t tmpfs none mirror-attrs
for i in foo bar; do
  /path/to/my/rsync -avPH --delete-after --only-send-attrs mirrors/$i/ mirror-attrs/$i/
done 
```

The result is:

```
/storage
 |
 +- mirrors
 |  |
 |  +- foo/
 |  +- bar/
 |  +- barz/
 +- mirror-attrs
    |
    +- foo/
    +- bar/
```

And this is the corresponding rsyncd.conf

```
ignore nonreadable = yes
uid = nobody
gid = nobody
use chroot = yes
dont compress = *.tar *.ova *.dmg *.pkg *.gz *.tgz *.zip *.z *.xz *.rpm *.deb *.bz2 *.tbz *.lzma *.7z *.rar *.iso
refuse options = checksum
read only = true
reverse lookup = no

[foo]
path = /storage/./mirror-attrs/foo
real file prefix = /mirrors/foo

[bar]
path = /storage/./mirror-attrs/bar
real file prefix = /mirrors/bar

[barz]
path = /storage/mirrors/barz
```

I've tested the code with severial cases and the centos repo and it seems to work. However, there is a lot to do to improve. I need your advice and test!

## A Test With CentOS Repo

To analyze how many IO ops and read bytes are reduced by `rsync-huai`, a test has been carried out with CentOS repo. Here are the process and the result.

Two copies of the whole CentOS repo were made, one of which was older and the other was newer. The newer one was put in `centos/` and the older was in `centos-old/`. Then `centos-old/` was copied twice, into `centos-old1/` and `centos-old2/`, after which, standard `rsync` and `rsync-huai` were used to sync the two old directories with the new one. Cgroup counters were used to account the IO ops and read bytes of the sender side, which in this case was the server. Before each test, `echo 3 > /proc/sys/vm/drop_caches` was used to drop all the cache.

### Timestamps

```
% cat centos/timestamp.txt
Fri Oct 14 11:36:01 UTC 2016
% cat centos-old/timestamp.txt
Sat Oct  1 06:36:01 UTC 2016
```

### Standard `rsync`

```
% cat /tmp/wrap.sh
#!/bin/bash
shift
exec cgexec -g blkio:rsync "$@"
% echo 3 > /proc/sys/vm/drop_caches
% echo 1 > /sys/fs/cgroup/blkio/rsync/blkio.reset_stats
% rsync -avP --delete :./centos/ ./centos-old1/ --rsh="/tmp/wrap.sh"
sent 7,029,376 bytes  received 4,784,663,386 bytes  12,829,164.02 bytes/sec
total size is 153,833,232,729  speedup is 32.10
``` 

### `rsync-huai`

```
% /path/to/rsync-huai -avP --only-send-attrs ./centos/ /dev/shm/centos-attrs/
% cat /path/to/rsyncd.conf
syslog facility = local1
max verbosity = yes
transfer logging = yes
ignore nonreadable = yes
uid = nobody
gid = nogroup
use chroot = no
dont compress = *.tar *.ova *.dmg *.pkg *.gz *.tgz *.zip *.z *.xz *.rpm *.deb *.bz2 *.tbz *.lzma *.7z *.rar *.iso
refuse options = checksum
read only = true
reverse lookup = no

log format = %o [%a] %m %b %f %l

[centos]
path = /dev/shm/centos-attrs
real file prefix = /path/to/centos
% cgexec -g blkio:rsync /path/to/rsync-huai --daemon --config /path/to/rsyncd.conf --port 12345
% echo 3 > /proc/sys/vm/drop_caches
% echo 1 > /sys/fs/cgroup/blkio/rsync/blkio.reset_stats
% rsync -avP --delete rsync://localhost:12345/centos/ ./centos-old2/
sent 7,029,440 bytes  received 4,783,453,228 bytes  14,027,767.70 bytes/sec
total size is 153,833,232,729  speedup is 32.11
```

To verify if `centos-old2/` was exactly the same with `centos-old1/`, the following check was performed.

```
% rsync -avP --delete --dry-run  ./centos-old1/ ./centos-old2/
sending incremental file list

sent 6,419,084 bytes  received 1,333 bytes  856,055.60 bytes/sec
total size is 153,833,232,729  speedup is 23,960.01 (DRY RUN)
```
This indicated that there was no difference between them.

### Result

              |IO ops  | Read Bytes | Bytes/IO op
 -------------|--------|------------|------------
`rsync`       | 88846  | 6345285632 | 71418.92
`rsync-huai`  | 83510  | 6323490816 | 75721.36
 Difference   | -6.01% | -0.34%     | 6.02%
 
### Test #2

After this test, I've carried another one. The result is as below.

```
% cat centos/timestamp.txt
Sat Oct  1 06:36:01 UTC 2016
% cat centos-old/timestamp.txt
Sun Oct 16 11:48:01 UTC 2016
```

              |IO ops  | Read Bytes | Bytes/IO op
 -------------|--------|------------|------------
`rsync`       |  10658 |   45445120 |  4263.94
`rsync-huai`  |     51 |    1998848 | 39193.10
 Difference   |-99.52% |    -95.60% |  819.17%

# Conversation in TUNA About It

We have internally talked about it, and I think it necessary to let you all know our ideas about it.

> ```
> From: Shanker Wang 
> Subject: [RFC] Optimized Rsync with cached attributes in memory 
> To: All Staff
> 
> Hi, TUNAers
> 
> As we all know, rsync is a good tool for mirroring things. Various 
> features of it are relied by mirror administrators. Most importantly, it 
> is needed to find out what get changed and only transfer them. Besides, 
> we need rsync to keep the modtime, mode, owner the same. Also, some may 
> add a exclude-from list to mirror files only needed or delete-after to 
> delete files deleted by the upstream after fetching new ones. 
> 
> However, providing rsync service for other mirror sites can be a 
> desaster. When a client begins to rsync file from a server, the first 
> thing the server do is recursively generating a list of the file tree 
> including all the names and other information of the files and 
> directories. Obviously, the process involves repeatedly seeking the 
> disks, which leads to bad performance of disk io. 
> 
> Take the centos repo for example. The command rsync --dry-run is used to 
> make the rsync server generate the list and exits, without doing any 
> real data transfer. Cgroup is also used to count the bytes read and IO 
> operations. And a wrapper script is written to put the server process 
> inside the cgroup. 
> 
> > cd /sys/fs/cgroup/blkio
> > mkdir rsyncd
> > cat /tmp/wrap.sh
> #!/bin/bash
> shift
> exec cgexec -g blkio:rsyncd "$@"
> > echo 3 > /proc/sys/vm/drop_caches 
> > echo 1 > blkio.reset_stats
> > rsync -an :/path/to/local/centos_repo/ /path/to/new/repo/ 
> --rsh="/tmp/wrap.sh"
> > grep . blkio.throttle.io* 
> 
> # Here are the counter values
> Here is the result. For comparison, find ., du -shxc . and cat 
> centos/7.2.1511/isos/x86_64/CentOS-7-x86_64-DVD-1511.iso > /dev/null are 
> also executed and measured.
> 
> Command	Read IO Ops	Bytes Read	Bytes per IO Op
> rsync	10538	43298816	4108.83
> find ./	3871	16449536	4249.43
> du -shxc	10448	42905600	4106.58
> cat	16525	4329652224	262006.19
> It's no doubt that, traversing the directory tree generates large amount 
> of fragmented IO operatons.
> 
> I once came up with an idea that maintaining the meta data inside a git 
> repo. After a successful update, the server calculates and stores the 
> meta into and generates a commit. The client, simply doing git pull and 
> git diff, can easily know the difference and can only request the files 
> changed with a --files-fromoption. However, this approach needs 
> modification on the client side, which cannot be easily adapted.
> 
> Considering the compatibility of standard rsync clients, generating the 
> list is not avoidable. Noticing it is only file attributes that are 
> needed to do that, such information can be stored in a faster storage 
> and hard disks are visited only when the real content of some file is 
> requested. 
> 
> First, a new option --only-send-attrs is add to the rsync command. When 
> used, it will work as if the content of each regular file from the 
> source is replaced by its size. As a result, we can get a directory tree 
> with the same modtime, mode, owner information with the source, except 
> the size. The size information, however, is stored in the content. This 
> tree can be stored on things like tmpfs. We call this attr tree for 
> short.
> 
> Secondly, a new config item real file prefix is add to rsyncd.conf. A 
> rsync daemon provides service with the attr tree, and thus can correctly 
> generate the list. When requested for a certain file, the rsync daemon 
> prepend the real file prefix before the path, and can find the correct 
> path to the content of the file.
> 
> You may say, talk is cheap, show me the f2k code. Here it is: 
> https://github.com/shankerwangmiao/rsync 
> <https://github.com/shankerwangmiao/rsync>. The code of rsync is so 
> ill-organized that I did some ugly modifications to the code. Here is a 
> brief how-to:
> 
> Suppose originally the repos on the server are stored like this:
> 
> /storage
>  |
>  +-foo/
>  +-bar/
>  +-barz/
> You need create a new directory, and move them inside.
> 
> /storage
>  |
>  +-mirrors
>    |
>    +-foo/
>    +-bar/
>    +-barz/
> Now, you can generate the attr tree
> 
> cd /storage/mirrors
> for i in foo bar; do
>   mkdir -p ../$i;
>   mount -t tmpfs tmpfs ../$i;
>   /path/to/my/rsync -avP --delete-after --only-send-attrs $i/ ../$i/
> done 
> Here is the result:
> 
> /storage
>  |
>  +-mirrors
>  | |
>  | +-foo/
>  | +-bar/
>  | +-barz/
>  +-foo/    <---- these two are "attr trees"
>  +-bar/ 
> And here is the rsyncd.conf
> 
> ignore nonreadable = yes
> uid = nobody
> gid = nobody
> use chroot = yes
> dont compress = *.tar *.ova *.dmg *.pkg *.gz *.tgz *.zip *.z *.xz 
> *.rpm *.deb *.bz2 *.tbz *.lzma *.7z *.rar *.iso
> refuse options = checksum
> read only = true
> reverse lookup = no
> 
> [foo]
> path = /storage/./foo
> real file prefix = /mirrors
> 
> [bar]
> path = /storage/./bar
> real file prefix = /mirrors
> 
> [barz]
> path = /storage/mirrors/barz
> I've tested the code with severial cases and the centos repo and it 
> seems to work. However, there is a lot to do to improve, such as the 
> method to look up the real content of a file.
> 
> I need your advice and test!
> 
> -------------
> Please verify the digital signature attached with the e-mail.
> Miao Wang
> Department of Computer Science and Technology, Tsinghua University
> Add.: Zijing Apartment, Tsinghua University, Peking. P.R.C. 100084
> 
> ```

> ```
> From: Aron Xu
> To: Shanker Wang
> CC: Justin Wong, All Staff
> Subject: Re: [RFC] Optimized Rsync with cached attributes in memory
> 
> Since you are duplicating the directory metadata, which implies
> updating your attr tree whenever updates happen on-disk, why not save
> the metadata to a signle file and load directly when requests arrive?

> Dangling tree consisting thousands of files is vulnerable. By saving
> the metadata to one file you are able to add timestamp and checksum
> easily, not mentioning the huge benefit of saving memory and elimating
> redundant I/Os. Also, it could be semi-transparent to users when
> metadata file is created on persistant storage.
> 
> 
> Regards,
> Aron
> ```

> ```
> From: Shanker Wang
> To: Aron Xu
> CC: Justin Wong, All Staff
> Subject: Re: [RFC] Optimized Rsync with cached attributes in memory 
> 
> I think filesystem itself is a better place to store information of filesystem.
> And doing so doesn’t require much modification to rsync itself.
> 
> ```

> ```
> From: Justin Wong
> To: Shanker Wang
> CC: Aron Xu, All Staff
> Subject: Re: [RFC] Optimized Rsync with cached attributes in memory 
> 
> Can we implement the tree layout as
> 
> +- storage
> +- mirrors
>    - foo
>    - bar
> +- mirrors_meta
>    - foo
>    - bar
> 
> --
> Yuzhi Wang
> ```

> ```
> From: Shanker Wang
> To: Justin Wong
> CC: Aron Xu, All Staff
> Subject: Re: [RFC] Optimized Rsync with cached attributes in memory 
> 
> I’ll try to do this. 
> 
> ```

> ```
> From: Shanker Wang
> To: Justin Wong
> CC: Aron Xu, All Staff
> Subject: Re: [RFC] Optimized Rsync with cached attributes in memory 
> 
> Done.
> 
> Example tree layout is:
> 
> /path/to/storage
> |
> +- mirror-files
> |  |
> |  +- foo/
> |  +- bar/
> |  +- barz/
> +- mirror-attrs
>    |
>    +- foo/
>    +- bar/
> 
> Example configuration is:
> 
> ignore nonreadable = yes
> uid = nobody
> gid = nobody
> use chroot = yes
> dont compress = *.tar *.ova *.dmg *.pkg *.gz *.tgz *.zip *.z *.xz *.rpm *.deb *.bz2 *.tbz *.lzma *.7z *.rar *.iso
> refuse options = checksum
> read only = true
> reverse lookup = no
> 
> [foo]
> path = /path/to/storage/./mirror-attrs/foo
> real file prefix = /mirror-files/foo
> 
> [bar]
> path = /path/to/storage/./mirror-attrs/bar
> real file prefix = /mirror-files/bar
> 
> [barz]
> path = /path/to/storage/mirror-files/barz
> 
> ```
