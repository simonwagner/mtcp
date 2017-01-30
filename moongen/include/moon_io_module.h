#ifndef __MOON_IO_MODULE_H_
#define __MOON_IO_MODULE_H_

#include <stdint.h>

#define MOONGEN_MTCP_MAX_INTERFACES 16

struct moongen_mtcp_interface {
    uint32_t ip_addr;
    uint32_t netmask;

    uint8_t dpdk_port_id;
};

struct moongen_mtcp_dpdk_config {
    int num_cores;
    int max_concurrency;
    int max_num_buffers;
    int num_mem_ch;


    int rcvbuf_size;
    int sndbuf_size;
    int tcp_timeout;
    int tcp_timewait;

    _Bool multi_process;

    int interfaces_count;
    struct moongen_mtcp_interface interfaces[MOONGEN_MTCP_MAX_INTERFACES];
};

void moongen_mtcp_set_default_config(struct moongen_mtcp_dpdk_config* config);
int moongen_mtcp_load_config(void* context);

#endif //__MOON_IO_MODULE_H_
