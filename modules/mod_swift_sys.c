#include <sys/socket.h>
#include <fcntl.h>
#include <string.h>
#include "tsar.h"


#define RETRY_NUM 3
/* swift default port should not changed */
#define HOSTNAME "localhost"
#define PORT 81
#define EQUAL ":="
#define DEBUG 1

char *swift_sys_usage = "    --swift_sys         Swift connection infomation";
int mgrport=81;

/* string at swiftclient -p 81 mgr:info */
/*
 * Average HTTP respone time:      5min: 11.70 ms, 60min: 10.06 ms
 * Request Hit Ratios:     5min: 95.8%, 60min: 95.7%
 * Byte Hit Ratios:        5min: 96.6%, 60min: 96.6%
 * UP Time:        247256.904 seconds
 * CPU Time:       23487.042 seconds
 * StoreEntries           : 20776287
 * client_http.requests = 150291472
 * client_http.bytes_in = 6380253436
 * client_http.bytes_out = 5730106537327
 */
const static char *SWIFT_STORE[] = {
    "client_http.accepts",
    "client_http.conns",
};

/* struct for swift counters */
struct status_swift_sys {
    unsigned long long accepts;
    unsigned long long conns;
} stats;

/* swift register info for tsar */
struct mod_info swift_sys_info[] = {
    {"accept", DETAIL_BIT,  0,  STATS_NULL},
    {"  conn", DETAIL_BIT,  0,  STATS_NULL},
    {"  null", HIDE_BIT,  0,  STATS_NULL}
};
/* opens a tcp or udp connection to a remote host or local socket */
static int
my_swift_net_connect(const char *host_name, int port, int *sd, char* proto)
{
    int                 result;
    struct protoent    *ptrp;
    struct sockaddr_in  servaddr;

    bzero((char *)&servaddr, sizeof(servaddr));
    servaddr.sin_family=AF_INET;
    servaddr.sin_port=htons(port);
    inet_pton(AF_INET, host_name, &servaddr.sin_addr);

    /* map transport protocol name to protocol number */
    if (((ptrp=getprotobyname(proto)))==NULL) {
        if (DEBUG) {
            printf("Cannot map \"%s\" to protocol number\n", proto);
        }
        return 3;
    }

    /* create a socket */
    *sd = socket(PF_INET, (!strcmp(proto, "udp"))?SOCK_DGRAM:SOCK_STREAM, ptrp->p_proto);
    if (*sd < 0) {
        close(*sd);
        if (DEBUG) {
            printf("Socket creation failed\n");
        }
        return 3;
    }
    /* open a connection */
    result = connect(*sd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    if (result < 0) {
        close(*sd);
        switch (errno) {
            case ECONNREFUSED:
                if (DEBUG) {
                    printf("Connection refused by host\n");
                }
                break;
            case ETIMEDOUT:
                if (DEBUG) {
                    printf("Timeout while attempting connection\n");
                }
                break;
            case ENETUNREACH:
                if (DEBUG) {
                    printf("Network is unreachable\n");
                }
                break;
            default:
                if (DEBUG) {
                    printf("Connection refused or timed out\n");
                }
        }

        return 2;
    }
    return 0;
}

static ssize_t
mywrite_swift(int fd, void *buf, size_t len)
{
    return send(fd, buf, len, 0);
}

static ssize_t
myread_swift(int fd, void *buf, size_t len)
{
    return recv(fd, buf, len, 0);
}

/* get value from counter */
static int
read_swift_value(char *buf, const char *key, unsigned long long *ret)
{
    int    k = 0;
    char  *tmp;
    /* is str match the keywords? */
    if ((tmp = strstr(buf, key)) != NULL) {
        /* compute the offset */
        k = strcspn(tmp, EQUAL);
        sscanf(tmp + k + 1, "%lld", ret);
        return 1;

    } else {
        return 0;
    }
}

static int
parse_swift_info(char *buf)
{
    char *line;
    line = strtok(buf, "\n");
    while (line != NULL) {
        read_swift_value(line, SWIFT_STORE[0], &stats.accepts);
        read_swift_value(line, SWIFT_STORE[1], &stats.conns);
        line = strtok(NULL, "\n");
    }
    return 0;
}

static void
set_swift_record(struct module *mod, double st_array[],
    U_64 pre_array[], U_64 cur_array[], int inter)
{
    /* accepts */
    if (cur_array[0] > pre_array[0])
        st_array[0] = (cur_array[0] - pre_array[0]) / inter;
    else
        st_array[0] = 0;

    /* conns */
    if (cur_array[1] > 0)
        st_array[1] = cur_array[1];
    else
        st_array[1] = 0;
}

static int
read_swift_stat(char *cmd)
{
    char msg[LEN_512];
    char buf[1024*1024];
    sprintf(msg,
            "GET cache_object://localhost/%s "
            "HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Accept:*/*\r\n"
            "Connection: close\r\n\r\n",
            cmd);

    int len, conn = 0, bytesWritten, fsize = 0;

    if (my_swift_net_connect(HOSTNAME, mgrport, &conn, "tcp") != 0) {
        close(conn);
        return -1;
    }

    int flags;

    /* set socket fd noblock */
    if ((flags = fcntl(conn, F_GETFL, 0)) < 0) {
        close(conn);
        return -1;
    }

    if (fcntl(conn, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        close(conn);
        return -1;
    }

    struct timeval timeout = {10, 0};

    setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(struct timeval));
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(struct timeval));

    bytesWritten = mywrite_swift(conn, msg, strlen(msg));
    if (bytesWritten < 0) {
        close(conn);
        return -2;

    } else if (bytesWritten != strlen(msg)) {
        close(conn);
        return -3;
    }

    while ((len = myread_swift(conn, buf + fsize, sizeof(buf))) > 0) {
        fsize += len;
    }

    /* read error */
    if (fsize < 100) {
        close(conn);
        return -1;
    }

    if (parse_swift_info(buf) < 0) {
        close(conn);
        return -1;
    }

    close(conn);
    return 0;
}

static void
read_swift_stats(struct module *mod, char *parameter)
{
    int    retry = 0, pos = 0;
    char   buf[LEN_1024];

    memset(&stats, 0, sizeof(stats));
    mgrport = atoi(parameter);
    if (!mgrport) {
        mgrport = 81;
    }
    retry = 0;
    while (read_swift_stat("counters") < 0 && retry < RETRY_NUM) {
        retry++;
    }
    pos = sprintf(buf, "%lld,%lld",
            stats.accepts,
            stats.conns
             );
    buf[pos] = '\0';
    set_mod_record(mod, buf);
}

void
mod_register(struct module *mod)
{
    register_mod_fileds(mod, "--swift_sys", swift_sys_usage, swift_sys_info, 2, read_swift_stats, set_swift_record);
}
