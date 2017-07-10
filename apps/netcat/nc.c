#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/queue.h>
#include <assert.h>

#include <rte_ethdev.h>
#include <rte_common.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>
#include <moon_mtcp.h>

#define MAX_CPUS 16

#define IP_RANGE 1
#define MAX_IP_STR_LEN 16

#define BUF_SIZE (1448)

#define CALC_MD5SUM FALSE

#define TIMEVAL_TO_MSEC(t)		((t.tv_sec * 1000) + (t.tv_usec / 1000))
#define TIMEVAL_TO_USEC(t)		((t.tv_sec * 1000000) + (t.tv_usec))
#define TS_GT(a,b)				((int64_t)((a)-(b)) > 0)

#define MAX(a, b) ((a)>(b)?(a):(b))
#define MIN(a, b) ((a)<(b)?(a):(b))

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

static bool running;

/*----------------------------------------------------------------------------*/
struct nc_ctx {
    in_addr_t daddr;
    in_port_t dport;
    mctx_t mctx;
    int ep;
    FILE* f;
};

/*----------------------------------------------------------------------------*/
uint32_t ParseIPv4(const char* str)
{
    int b[4];
    sscanf(str, "%d.%d.%d.%d", b, b + 1, b + 2, b + 3);

    uint32_t ipv4 = b[0] << 0 | b[1] << 8 | b[2] << 16 | b[3] << 24;

    return ipv4;
}

struct nc_ctx*
CreateContext(int core, int lcore, in_addr_t daddr, in_port_t dport, FILE* f)
{
    struct nc_ctx* ctx;

    ctx = (struct nc_ctx*)calloc(1, sizeof(struct nc_ctx));

    ctx->mctx = mtcp_create_context_on_lcore(core, lcore);
	if (!ctx->mctx) {
        fprintf(stderr, "Failed to create mtcp context.\n");
		return NULL;
	}
    ctx->daddr = daddr;
    ctx->dport = dport;
    ctx->f = f;

	return ctx;
}
/*----------------------------------------------------------------------------*/
void 
DestroyContext(struct nc_ctx* ctx)
{
	mtcp_destroy_context(ctx->mctx);
	free(ctx);
}
/*----------------------------------------------------------------------------*/
static int
CreateConnection(struct nc_ctx* ctx)
{
	mctx_t mctx = ctx->mctx;
	struct sockaddr_in addr;
	int sockid;
	int ret;

	sockid = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);
	if (sockid < 0) {
        fprintf(stderr, "Failed to create socket!\n");
		return -1;
	}

	addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ctx->daddr;
    addr.sin_port = ctx->dport;
	
	ret = mtcp_connect(mctx, sockid, 
			(struct sockaddr *)&addr, sizeof(struct sockaddr_in));

    if(ret < 0) {
        return ret;
    }

    mtcp_setsock_nonblock(ctx->mctx, sockid);

    struct mtcp_epoll_event ev;
    ev.events = MTCP_EPOLLOUT;
    ev.data.sockid = sockid;
    mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, sockid, &ev);

	return sockid;
}
/*----------------------------------------------------------------------------*/
static void
CloseConnection(struct nc_ctx* ctx, int sockid)
{
	mtcp_close(ctx->mctx, sockid);
}
/*----------------------------------------------------------------------------*/

int
setup_dpdk(const char* program_name, struct moon_mtcp_dpdk_config* config, const char* netif_pci_address)
{
    int cpumask = 0;
    char cpumaskbuf[10];
    char mem_channels[5];

    /* get the cpu mask */
    for (int i = 0; i < config->num_cores + 1; i++)
        cpumask = (cpumask | (1 << i));
    sprintf(cpumaskbuf, "%X", cpumask);
    sprintf(mem_channels, "%d", config->num_mem_ch);

    const char* argv[] = {
        program_name,
        "-c",
        cpumaskbuf,
        "-n",
        mem_channels,
        "--proc-type=auto",
        "",
        "",
        ""
    };
    int argc = 6;

    //if netif_pci_address is given, add it to the whitelist
    if(netif_pci_address != NULL) {
        argv[argc] = "-w";
        argv[argc + 1] = netif_pci_address;
        argc += 2;
    }

    printf("Initializing dpdk with arguments:");
    for(int i = 0; i < argc; i++) {
        printf(" %s", argv[i]);
    }
    printf("\n");

    int ret = rte_eal_init(argc, argv);
    if(ret < 0) {
        rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
    }

    int nr_eth_dev = rte_eth_dev_count();
    printf("Found %d dpdk ports\n", nr_eth_dev);

    if(nr_eth_dev <= 0) {
        rte_exit(EXIT_FAILURE, "No devices found\n");
    }

    return 0;
}

void
SignalHandler(int signal)
{
    running = FALSE;
}

/*----------------------------------------------------------------------------*/
int
RunNcMain(struct nc_ctx* ctx)
{
    running = TRUE;
    int maxevents = 5;

    ctx->ep = mtcp_epoll_create(ctx->mctx, maxevents);

    int socketid = CreateConnection(ctx);

    if(socketid < 0) {
        fprintf(stderr, "Failed to connect\n");
        return -1;
    }

    char* buffer = malloc(BUF_SIZE);
    int buffer_offset = 0;

    struct mtcp_epoll_event *events = (struct mtcp_epoll_event *) calloc(maxevents, sizeof(struct mtcp_epoll_event));
    int nevents = 0;

    while(running) {
        nevents = mtcp_epoll_wait(ctx->mctx, ctx->ep, events, maxevents, -1);

        if(nevents < 0) {
            printf("ERROR: mtcp_epoll_wait failed: %d\n", nevents);
            break;
        }

        for (int i = 0; i < nevents && running; i++) {
            if (events[i].events & MTCP_EPOLLERR) {
                int err;
                socklen_t len = sizeof(err);

                printf("Detected error on socket!\n");
                if (mtcp_getsockopt(ctx->mctx, events[i].data.sockid,
                            SOL_SOCKET, SO_ERROR, (void *)&err, &len) == 0) {
                    printf("Error on socket was %d\n", err);
                    running = FALSE;
                    break;
                }
            }
            else if(events[i].events & MTCP_EPOLLOUT) {
                //read data from file
                int bytes_to_read = BUF_SIZE - buffer_offset;
                int bytes_read = fread(buffer + buffer_offset, 1, bytes_to_read, ctx->f);
                bool write_more = TRUE;

                while(write_more && running) {
                    if(bytes_read < 0) {
                        fprintf(stderr, "error reading data\n");
                        running = FALSE;
                        break;
                    }

                    int bytes_to_write = bytes_read + buffer_offset;
                    int bytes_written = mtcp_write(ctx->mctx, socketid, buffer, bytes_to_write);
                    if(bytes_written < bytes_to_write) {
                        write_more = FALSE;
                    }
                    if(bytes_written < 0 && errno != EAGAIN) {
                        printf("Error, could not write data: mtcp_write returned %d\n", bytes_written);
                        running = FALSE;
                        break;
                    }

                    if(bytes_read < bytes_to_read) {
                        fprintf(stderr, "Finished sending data\n");
                        running = false;
                        break;
                    }

                    //move unsend bytes to beginning of the buffer
                    if(bytes_written > 0 && bytes_written < bytes_to_write) {
                        memmove(buffer, buffer + bytes_written, BUF_SIZE - bytes_written);
                        buffer_offset = bytes_written;
                    }
                    else if(bytes_written >= bytes_to_write) {
                        buffer_offset = 0;
                    }
                }

                if(!running) {
                    break;
                }
            }
        }
    }

    CloseConnection(ctx, socketid);
    free(buffer);
}

/*----------------------------------------------------------------------------*/
int 
main(int argc, char **argv)
{
	struct mtcp_conf mcfg;
    struct nc_ctx* ctx;
    struct moon_mtcp_dpdk_config moon_cfg;
    int core_limit = 1;
    int num_cores = 1;
    const char* netif_pci_address = NULL;

	if (argc < 3) {
        fprintf(stderr, "Too few arguments!\n");
        fprintf(stderr, "Usage: %s -a LOCALADDRESS -m LOCALNETMASK -p DPDKPORT -H REMOTEHOST -P REMOTEPORT -f INPUTFILE\n", argv[0]);
		return FALSE;
	}

    moon_mtcp_set_default_config(&moon_cfg);
    moon_cfg.num_cores = num_cores;
    moon_cfg.num_mem_ch = num_cores;
    moon_cfg.interfaces_count = 1;
    moon_cfg.max_concurrency = 2;

    printf("Reading command line arguments..\n");
    FILE* f = NULL;
    in_addr_t daddr = 0;
    in_port_t dport = 0;

    for (int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-a") == 0) {
            moon_cfg.interfaces[0].ip_addr = ParseIPv4(argv[i+1]);
            i++;
        }
        else if(strcmp(argv[i], "-m") == 0) {
            moon_cfg.interfaces[0].netmask = ParseIPv4(argv[i+1]);
            i++;
        }
        else if(strcmp(argv[i], "-p") == 0) {
            moon_cfg.interfaces[0].dpdk_port_id = atoi(argv[i+1]);
            i++;
        }
        else if(strcmp(argv[i], "-H") == 0) {
            daddr = ParseIPv4(argv[i+1]);
            i++;
        }
        else if(strcmp(argv[i], "-P") == 0) {
            dport = htons(atoi(argv[i+1]));
            i++;
        }
        else if(strcmp(argv[i], "-w") == 0) {
            netif_pci_address = argv[i+1];
            i++;
        }
        else if(strcmp(argv[i], "-f") == 0) {
            const char* fpath = argv[i + 1];
            if(strcmp(fpath, "-") == 0) {
                f = stdin;
            }
            else {
                f = fopen(fpath, "rb");
            }

            if(f == NULL) {
                fprintf(stderr, "Error, can't open %s\n", fpath);
                return 1;
            }
            i++;
        }
	}

    printf("Setting up dpdk...\n");
    setup_dpdk(argv[0], &moon_cfg, netif_pci_address);

    if(!rte_eth_dev_is_valid_port(moon_cfg.interfaces[0].dpdk_port_id)) {
        printf("DPDK port %d is not valid\n", moon_cfg.interfaces[0].dpdk_port_id);
        exit(1);
    }
    printf("Configuring mtcp...\n");
    if(!rte_eth_dev_is_valid_port(moon_cfg.interfaces[0].dpdk_port_id)) {
        printf("DPDK port %d is not valid\n", moon_cfg.interfaces[0].dpdk_port_id);
        exit(1);
    }
    else {
        printf("valid DPDK port\n");
    }

    int ret = mtcp_init_with_configuration_func(&moon_cfg, moon_mtcp_load_config);

	if (ret) {
        fprintf(stderr, "Failed to initialize mtcp.\n");
		exit(EXIT_FAILURE);
	}
    printf("done\n");

	mtcp_register_signal(SIGINT, SignalHandler);

    ctx = CreateContext(0, 1, daddr, dport, f);
    if(ctx == NULL) {
        fprintf(stderr, "Failed to create context\n");
    }

    RunNcMain(ctx);

    DestroyContext(ctx);
	mtcp_destroy();

    return 0;
}
/*----------------------------------------------------------------------------*/
