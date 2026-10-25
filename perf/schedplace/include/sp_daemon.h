/* schedplace — event loop: timerfd scan + signalfd + inotify wakeups. */
#ifndef SP_DAEMON_H
#define SP_DAEMON_H

#include "sp_conf.h"
#include "sp_topology.h"

int sp_daemon_run(struct sp_config *cfg, struct sp_topology *topo);
int sp_daemon_once(struct sp_config *cfg, struct sp_topology *topo);

#endif
