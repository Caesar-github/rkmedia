#ifndef __RKSOCKET__
#define __RKSOCKET__

#define SOCKERR_IO -1
#define SOCKERR_CLOSED -2
#define SOCKERR_INVARG -3
#define SOCKERR_TIMEOUT -4
#define SOCKERR_OK 0

#define CS_PATH "/var/tmp/rkmedia"

int cli_begin(char *name);
int cli_end(int fd);
int cli_connect(const char *name);
int serv_listen(const char *name);
int serv_accept(int fd);
int sock_write(int fd, const void *buff, int count);
int sock_read(int fd, void *buff, int count);
int sock_send_devfd(int fd, int devfd);
int sock_recv_devfd(int fd, int *devfd);

#endif