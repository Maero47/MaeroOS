This directory exists so /disk/ffout is present and writable by the desktop
user: /disk/ff copies Firefox's MOZ_LOG files here (parent and children) when an
attempt stalls, so the host can read them out of disk-ff.img afterwards with
  debugfs -R 'dump /ffout/<name> <name>' disk-ff.img
instead of paying serial-console bandwidth for a 75 KB log.
