
#include "moon_mtcp.h"

/* std lib funcs */
#include <stdlib.h>
/* std io funcs */
#include <stdio.h>
/* strcmp func etc. */
#include <string.h>
#ifndef DISABLE_DPDK
/* for dpdk ethernet functions (get mac addresses) */
#include <rte_ethdev.h>
#include <rte_ether.h>
#endif
#include "mtcp.h"
/* for num_devices decl */
#include "config.h"
/* for I/O module def'ns */
#include "io_module.h"
#include "tcp_in.h"
/*----------------------------------------------------------------------------*/
static int
moon_mtcp_configure_interface(struct moon_mtcp_interface* ifc)
{
    int index = CONFIG.eths_num;

    if(current_iomodule_func != &dpdk_module_func) {
        return -1;
    }

    struct eth_table* mtcp_ifc = &CONFIG.eths[index];

    sprintf(mtcp_ifc->dev_name, "dpdk%d", (int)ifc->dpdk_port_id);
    mtcp_ifc->ifindex = index;
    mtcp_ifc->stat_print = 0;

    struct ether_addr haddr;
    rte_eth_macaddr_get(ifc->dpdk_port_id, &haddr);
    memcpy(mtcp_ifc->haddr, haddr.addr_bytes, ETH_ALEN);

    mtcp_ifc->ip_addr = ifc->ip_addr;
    mtcp_ifc->netmask = ifc->netmask;

    devices_attached[num_devices_attached] = ifc->dpdk_port_id;
    num_devices_attached++;
    CONFIG.eths_num++;

    return 0;
}
/*----------------------------------------------------------------------------*/
void moon_mtcp_set_default_config(struct moon_mtcp_dpdk_config* config)
{
    config->max_concurrency = 100000;
    config->max_num_buffers = 100000;
    config->rcvbuf_size = 8192;
    config->sndbuf_size = 8192;
    config->tcp_timeout = TCP_TIMEOUT;
    config->tcp_timewait = TCP_TIMEWAIT;
    config->num_mem_ch = 0;
}
/*----------------------------------------------------------------------------*/
int moon_mtcp_load_config(void* context)
{
    struct moon_mtcp_dpdk_config* config = context;

    CONFIG.num_cores = config->num_cores;
    CONFIG.num_mem_ch = config->num_mem_ch;
    CONFIG.max_concurrency = config->max_concurrency;

    CONFIG.max_num_buffers = config->max_num_buffers;
    CONFIG.rcvbuf_size = config->rcvbuf_size;
    CONFIG.sndbuf_size = config->sndbuf_size;

    CONFIG.tcp_timeout = config->tcp_timeout;
    CONFIG.tcp_timewait = config->tcp_timewait;

    //set dpdk as IO method
    current_iomodule_func = &dpdk_module_func;

    num_queues = config->num_cores < MAX_CPUS ? config->num_cores : MAX_CPUS;

    CONFIG.eths = (struct eth_table *)
            calloc(MAX_DEVICES, sizeof(struct eth_table));
    if (!CONFIG.eths)
        return -1;
    CONFIG.eths_num = 0;
    num_devices_attached = 0;

    for(int i = 0; i < config->interfaces_count; i++) {
        moon_mtcp_configure_interface(&config->interfaces[i]);
    }

    return 0;
}
