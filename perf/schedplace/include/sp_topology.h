/* schedplace — runtime CPU topology discovery (little / big / prime). */
#ifndef SP_TOPOLOGY_H
#define SP_TOPOLOGY_H

#include <sched.h>
#include <stdbool.h>

#include "sp_rules.h"
#include "sp_util.h"

struct sp_topology {
    cpu_set_t online;
    cpu_set_t cluster[3]; /* indexed by SP_CLUSTER_LITTLE/BIG/PRIME */
    unsigned long capacity[3];
    int levels; /* 1 = one homogeneous level, 2 = little+big, 3 = little+big+prime */
    bool degraded;
    char source[32];
    char warning[SP_TEXT_MAX];
    char sysfs_root[SP_PATH_MAX];
};

int sp_topo_discover(struct sp_topology *topo, const char *sysfs_root, char *err,
                     size_t errlen);
bool sp_topo_resolve(const struct sp_topology *topo, enum sp_cluster cluster,
                     enum sp_profile profile, cpu_set_t *out);
const cpu_set_t *sp_topo_cluster(const struct sp_topology *topo, enum sp_cluster cluster);
void sp_topo_summary(const struct sp_topology *topo, char *buf, size_t size);

#endif
