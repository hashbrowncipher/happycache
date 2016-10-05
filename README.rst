happycache: a utility for loading pages into cache
==================================================

Databases such as PostgreSQL and Cassandra make extensive use of the OS page
cache to serve queries. Maintenance events like reboots result in the database
software operating against an empty or almost empty page cache, hampering
performance until the cache can fill.

happycache is a utility designed to make it easy to quicky re-fill a page
cache using the pages that were present previously. A happy cache makes for a
happy database!

Command-line usage
------------------

Usage
~~~~~

::

  $ happycache
  Usage: happycache [-dl] [<name>]

  -d: dump mode (default)
  print out a map of pages that are currently in the page cache. happycache 
  recursively walks the files in the directory given by <name>. Only files 
  in that directory are mapped. If <name> is not specified, the current
  working directory is assumed.
 
  -l: load mode
  load pages into the cache using a happycache dump. If no <name> is specified,
  happycache reads from stdin.

Examples
~~~~~~~~

happycache dump files contain the addresses (not the data) of the pages in the
cache. They are designed to be highly compressible, so we'll use gzip to cut 
the map file down to size.

::

  # cd /var/lib/cassandra
  # happycache | gzip -9 > .happycache.gz
  # (reboot happens here)
  # gzip -cd .happycache.gz | happycache -l

FAQ
---

Can I use the same happycache map on two different servers?
  Yep! If the two servers contain identical files, then a happycache map from
  one server can be used to warm the page cache on a second server. This is 
  useful for Postgres replicas, where the files are always identical (or near 
  to it).

Does a happycache dump file include the contents of pages?
  No. The happycache dump includes only enough information to _locate_ pages.
  The dump does not contain any actual data from within the pages. As such,
  happycache dumps 

Does happycache have a performance impact?
  The purpose of happycache is to populate a page cache as quickly as the
  underlying hardware will allow. In cases where happycache is running beside
  another application (e.g. a database), happycache's reads will compete with
  I/O from that application. To mitigate this, happycache places itself in 
  ``CLASS_IDLE`` I/O priority, meaning that the Linux CFQ disk scheduler will
  prioritize all other I/O above happycache's. This only works with the CFQ
  I/O scheduler, and NOT the noop or deadline schedulers.

Can happycache cause kernel panics (or plague, or lice)?
  The interfaces happycache uses to dump and fill page caches are quite
  boring and entirely non-invasive. The worst case scenario for happycache
  would be accidentally filling your page cache with data that nobody has use
  for.

Should I run this after my nightly backups to repopulate the page-cache?
  Instead of using happycache to repopulate a busted page cache, I would
  consider using a backup tool which does not thrash the page cache in the
  first place.
