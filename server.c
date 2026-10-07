/*
 * server.c -- TCP + UDP demo server (IPv4 only)
 * Build: gcc -Wall -Wextra -o server server.c
 * Run:   ./server <port>
 *
 * Based on the course sources tcp_server.c (chap02) and uecho_server.c (chap06).
 * One TCP socket and one UDP socket share the same port number; select() picks
 * whichever one a client contacts. One client is served at a time.
 * The client sends "SEND <count>" and the server streams <count> numbered lines.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>

#define REQ_SIZE 64
#define LINE_FMT "msg %06d: The quick brown fox jumps over the lazy dog\n"

void error_handling(char *message);
double now_sec(void);
int parse_request(const char *req);
int write_all(int fd, const char *buf, int len);
void local_ip_for(struct in_addr peer, char *out);
void serve_tcp(int serv_sock);
void serve_udp(int udp_sock);

int main(int argc, char *argv[])
{
	int tcp_sock, udp_sock, port, on = 1;
	struct sockaddr_in serv_addr;
	fd_set reads;

	if (argc != 2) {
		printf("Usage : %s <port>\n", argv[0]);
		exit(1);
	}
	port = atoi(argv[1]);
	setvbuf(stdout, NULL, _IOLBF, 0);   /* show log lines immediately, even when redirected */
	signal(SIGPIPE, SIG_IGN);      /* a client that disconnects must not kill the server */

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(port);

	tcp_sock = socket(PF_INET, SOCK_STREAM, 0);
	udp_sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (tcp_sock == -1 || udp_sock == -1)
		error_handling("socket() error");
	setsockopt(tcp_sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	if (bind(tcp_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
		error_handling("TCP bind() error");
	if (bind(udp_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
		error_handling("UDP bind() error");
	if (listen(tcp_sock, 5) == -1)
		error_handling("listen() error");

	printf("Server listening on port %d (TCP and UDP). Ctrl+C to stop.\n", port);
	while (1) {
		FD_ZERO(&reads);
		FD_SET(tcp_sock, &reads);
		FD_SET(udp_sock, &reads);
		if (select((tcp_sock > udp_sock ? tcp_sock : udp_sock) + 1, &reads, NULL, NULL, NULL) == -1)
			error_handling("select() error");
		if (FD_ISSET(tcp_sock, &reads))
			serve_tcp(tcp_sock);
		if (FD_ISSET(udp_sock, &reads))
			serve_udp(udp_sock);
	}
	return 0;
}

/* TCP: accept a connection, then stream the text over it. */
void serve_tcp(int serv_sock)
{
	struct sockaddr_in clnt_addr, local_addr;
	socklen_t clnt_sz = sizeof(clnt_addr), local_sz = sizeof(local_addr);
	char req[REQ_SIZE] = {0}, line[64], clnt_ip[INET_ADDRSTRLEN], serv_ip[INET_ADDRSTRLEN];
	int clnt_sock, count, i, len;
	double start;

	clnt_sock = accept(serv_sock, (struct sockaddr *)&clnt_addr, &clnt_sz);
	if (clnt_sock == -1)
		error_handling("accept() error");
	if (read(clnt_sock, req, REQ_SIZE - 1) <= 0 || (count = parse_request(req)) < 0) {
		close(clnt_sock);
		return;
	}

	getsockname(clnt_sock, (struct sockaddr *)&local_addr, &local_sz);
	inet_ntop(AF_INET, &clnt_addr.sin_addr, clnt_ip, sizeof(clnt_ip));
	inet_ntop(AF_INET, &local_addr.sin_addr, serv_ip, sizeof(serv_ip));
	printf("[TCP] server %s:%d <-> client %s:%d\n", serv_ip, ntohs(local_addr.sin_port),
	       clnt_ip, ntohs(clnt_addr.sin_port));

	start = now_sec();
	for (i = 0; i < count; i++) {
		len = snprintf(line, sizeof(line), LINE_FMT, i);
		if (write_all(clnt_sock, line, len) == -1)
			break;             /* write() blocks when the receiver is slow (flow control) */
	}
	printf("[TCP] sent %d lines in %.4fs\n", i, now_sec() - start);
	close(clnt_sock);              /* closing signals end-of-stream to the client */
}

/* UDP: no connection. The client's first datagram tells us where to reply. */
void serve_udp(int udp_sock)
{
	struct sockaddr_in clnt_addr;
	socklen_t clnt_sz = sizeof(clnt_addr);
	char req[REQ_SIZE] = {0}, line[64], clnt_ip[INET_ADDRSTRLEN], serv_ip[INET_ADDRSTRLEN];
	int count, i, len;
	double start;

	if (recvfrom(udp_sock, req, REQ_SIZE - 1, 0, (struct sockaddr *)&clnt_addr, &clnt_sz) <= 0
	    || (count = parse_request(req)) < 0)
		return;

	local_ip_for(clnt_addr.sin_addr, serv_ip);
	inet_ntop(AF_INET, &clnt_addr.sin_addr, clnt_ip, sizeof(clnt_ip));
	printf("[UDP] server %s <-> client %s:%d\n", serv_ip, clnt_ip, ntohs(clnt_addr.sin_port));

	start = now_sec();
	for (i = 0; i < count; i++) {
		len = snprintf(line, sizeof(line), LINE_FMT, i);
		sendto(udp_sock, line, len, 0, (struct sockaddr *)&clnt_addr, clnt_sz);
	}
	sendto(udp_sock, "END", 3, 0, (struct sockaddr *)&clnt_addr, clnt_sz);
	printf("[UDP] sent %d datagrams in %.4fs (sendto() never waits for the receiver)\n",
	       count, now_sec() - start);
}

/* A UDP socket has no peer, so ask the OS which local address routes to the client. */
void local_ip_for(struct in_addr peer, char *out)
{
	struct sockaddr_in probe, local;
	socklen_t sz = sizeof(local);
	int sock = socket(PF_INET, SOCK_DGRAM, 0);

	memset(&probe, 0, sizeof(probe));
	probe.sin_family = AF_INET;
	probe.sin_addr = peer;
	probe.sin_port = htons(9);
	connect(sock, (struct sockaddr *)&probe, sizeof(probe));   /* sends nothing */
	getsockname(sock, (struct sockaddr *)&local, &sz);
	inet_ntop(AF_INET, &local.sin_addr, out, INET_ADDRSTRLEN);
	close(sock);
}

int parse_request(const char *req)
{
	int n = 0;
	return (sscanf(req, "SEND %d", &n) == 1 && n > 0) ? n : -1;
}

int write_all(int fd, const char *buf, int len)
{
	int off = 0, n;
	while (off < len) {
		n = write(fd, buf + off, len - off);
		if (n <= 0)
			return -1;
		off += n;
	}
	return 0;
}

double now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

void error_handling(char *message)
{
	fputs(message, stderr);
	fputc('\n', stderr);
	exit(1);
}
