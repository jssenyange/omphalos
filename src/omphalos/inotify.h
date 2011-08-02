#ifndef OMPHALOS_INOTIFY
#define OMPHALOS_INOTIFY

#ifdef __cplusplus
extern "C" {
#endif

struct omphalos_iface;

typedef void(*watchcbfxn)(const struct omphalos_iface *);

int watch_init(const struct omphalos_iface *);
int watch_file(const struct omphalos_iface *,const char *,watchcbfxn);
int handle_watch_event(const struct omphalos_iface *,int);
int watch_stop(const struct omphalos_iface *);

#ifdef __cplusplus
}
#endif

#endif