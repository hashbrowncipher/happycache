happycache: a utility for loading pages into cache
==================================================

Databases such as PostgreSQL and Cassandra make extensive use of the OS page
cache to serve queries. Maintenance events like reboots result in the database
software operating against an empty or almost empty page cache, hampering
performance until the cache can fill.

happycache is a utility to quicky re-fill ths OS page cache using the pages
that were present previously. A happy cache makes for a happy database!

Command-line usage
------------------

Usage
~~~~~

::

  $ happycache
  Usage: happycache (dump|load) args...
    dump [directory]
      print out a map of pages that are currently in the page cache by recursively
      walking a directory.
        directory: the directory to walk
                     (default=current working directory)

    load [filename] [threads]
      load pages into the cache using a happycache dump file
        filename: the happycache dump file to read (default=stdin)
        threads: the number of threads to read with (default=32)

Examples
~~~~~~~~

happycache dump files contain the addresses (not the data) of the pages in the
cache. They are designed to be highly compressible, so we'll use gzip to cut
the map file down to size. In this example, we'll dump the page cache of a
Cassandra data directory.

::

  cassandra@box:/var/lib/cassandra$ happycache dump | gzip -9 > .happycache.gz
  root@box:/var/lib/cassandra# reboot
  cassandra@box:/var/lib/cassandra# zcat .happycache.gz | happycache load

We can use happycache load across servers. In the example below, we rsync files
from the `/var/lib/cassandra` directory on box1 to the
`/var/lib/cassandra/subdir` directory on box2. Because the happycache dump file
encodes relative paths, it has no problem working under the modified path.

::

  cassandra@box1:/var/lib/cassandra$ happycache dump | gzip -9 > .happycache.gz
  root@box2:/var/lib/cassandra/subdir# rsync -a box1:/var/lib/cassandra/ .
  cassandra@box2:/var/lib/cassandra/subdir# zcat .happycache.gz | happycache load

FAQ
---

Can I use the same happycache map on two different servers?
  Yep! If the two servers contain identical or similar files, then a happycache
  map from one server can be used to "copy" the page cache onto a second
  server. This is useful for Postgres replicas, where the files are identical
  or very similar.

Does a happycache dump file include the contents of pages?
  No. The happycache dump includes only enough information to *locate* pages.
  The dump does not contain any actual data from within the pages. As a result,
  happycache dumps are small, and do not leak any confidential data that may
  be in the page cache.

Does happycache loading have a performance impact?
  The purpose of happycache is to populate a page cache as quickly as the
  underlying hardware will allow. In cases where happycache is running beside
  another application (e.g. a database), happycache's reads will compete with
  I/O from that application. To mitigate this, happycache places itself in
  ``CLASS_IDLE`` I/O priority, meaning that the Linux CFQ disk scheduler will
  prioritize all other I/O above happycache's. This only works with the CFQ
  I/O scheduler, and NOT the noop or deadline schedulers.

Can happycache cause kernel panics (or plague, or lice)?
  The interfaces happycache uses to dump and fill page caches are quite
  boring and entirely non-invasive. The worst case scenario would be
  accidentally filling your page cache up with data that nobody has use for.

Should I run this after my nightly backups to repopulate the page-cache?
  Instead of using happycache to repopulate a busted page cache, I would
  consider using a backup tool which does not thrash the page cache in the
  first place. ``tar``, for example, works with the operating system to ensure
  that the files it reads do not remain in the page cache.

How can I use cgroups to preserve the page cache?
-------------------------------------------------

The need for happycache can be avoided in many cases by using cgroups to limit
the amount of page cache an application can consume. For instance, one can run
``mycommand`` in a memory cgroup with 50MB of memory like so::

  $ sudo cgcreate -g memory:backups -a $USER -t $USER
  $ cgset -r memory.limit_in_bytes=52428800 backups
  $ cgexec -g memory:backups mycommand
