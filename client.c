/*
 * client.c -- TCP + UDP demo client (IPv4 only)
 * Build: gcc -Wall -Wextra -o client client.c
 * Run:   ./client <server IPv4> <port>
 *
 * Based on tcp_client.c (chap02) and uecho_client.c (chap06).
 * Asks the server for COUNT numbered lines, then measures speed and reliability.
 *
 * Assumptions and limitations:
 *  - RCVBUF is deliberately tiny so a fast sender can overflow the receiver. On
 *    loopback, UDP rarely loses packets otherwise, so loss would not show.
 *  - Results depend on your machine and OS; run several times.
 *  - Time runs from sending the request to the last data received. The 2 s idle
 *    timeout that ends a UDP run is not counted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#define COUNT 20000
#define RCVBUF 4096
#define REQ_SIZE 64

typedef struct {
	const char *name;
	int received, lost, dups, out_of_order, reads;
	double elapsed, connect_ms, mbps;
} Result;

static unsigned char seen[COUNT];   /* seen[seq] = 1 once line <seq> has arrived */

void error_handling(char *message);
double now_sec(void);
int new_sock(int type);
void show_addresses(const char *name, int sock);
void record(int seq, int *highest, Result *r);
void finish(Result *r, long bytes, double elapsed);
int run_tcp(struct sockaddr_in *serv, Result *r);
int run_udp(struct sockaddr_in *serv, Result *r);
void print_report(Result *r, int n);

int main(int argc, char *argv[])
{
	struct sockaddr_in serv_addr;
	Result results[2];
	char input[16];
	int n = 0, choice = 3;

	if (argc != 3) {
		printf("Usage : %s <IP> <port>\n", argv[0]);
		exit(1);
	}
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(atoi(argv[2]));
	if (!inet_aton(argv[1], &serv_addr.sin_addr))
		error_handling("invalid IPv4 address");

	printf("Select protocol: 1) TCP  2) UDP  3) Both [3]: ");
	if (fgets(input, sizeof(input), stdin) && input[0] != '\n')
		choice = atoi(input);
	if (choice < 1 || choice > 3)
		error_handling("Invalid choice.");

	if (choice != 2) {
		if (run_tcp(&serv_addr, &results[n]) == 0) n++;
		else fputs("TCP failed: could not connect (is the server running?)\n", stderr);
	}
	if (choice != 1) {
		if (run_udp(&serv_addr, &results[n]) == 0) n++;
		else fputs("UDP failed: socket error\n", stderr);
	}
	if (n > 0)
		print_report(results, n);
	return 0;
}

/* TCP: connect (three-way handshake), then read the byte stream until the server closes. */
int run_tcp(struct sockaddr_in *serv, Result *r)
{
	char req[REQ_SIZE], buf[2048], line[128];
	int sock = new_sock(SOCK_STREAM), highest = -1, linelen = 0, seq, n, i;
	long bytes = 0;
	double t0, t_req, last;

	memset(r, 0, sizeof(*r));
	memset(seen, 0, sizeof(seen));
	r->name = "TCP";

	t0 = now_sec();
	if (connect(sock, (struct sockaddr *)serv, sizeof(*serv)) == -1) {
		close(sock);
		return -1;
	}
	r->connect_ms = (now_sec() - t0) * 1000;
	show_addresses("TCP", sock);

	snprintf(req, sizeof(req), "SEND %d", COUNT);
	t_req = last = now_sec();
	write(sock, req, strlen(req));

	/* TCP has no message boundaries: rebuild lines from arbitrary chunks. */
	while ((n = read(sock, buf, sizeof(buf))) > 0) {
		r->reads++;
		bytes += n;
		last = now_sec();
		for (i = 0; i < n; i++) {
			if (buf[i] != '\n') {
				if (linelen < (int)sizeof(line) - 1)
					line[linelen++] = buf[i];
				continue;
			}
			line[linelen] = '\0';
			linelen = 0;
			if (sscanf(line, "msg %d:", &seq) == 1)
				record(seq, &highest, r);
		}
	}
	close(sock);
	finish(r, bytes, last - t_req);
	return 0;
}

/* UDP: connect() only stores the peer address (no handshake); each recv() is one datagram. */
int run_udp(struct sockaddr_in *serv, Result *r)
{
	char req[REQ_SIZE], buf[2048];
	int sock = new_sock(SOCK_DGRAM), highest = -1, seq, n;
	long bytes = 0;
	double t_req, last;

	memset(r, 0, sizeof(*r));
	memset(seen, 0, sizeof(seen));
	r->name = "UDP";

	connect(sock, (struct sockaddr *)serv, sizeof(*serv));
	show_addresses("UDP", sock);

	snprintf(req, sizeof(req), "SEND %d", COUNT);
	t_req = last = now_sec();
	send(sock, req, strlen(req), 0);

	/* Ends on "END", on the 2 s idle timeout, or on ICMP "port unreachable". */
	while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
		if (n == 3 && memcmp(buf, "END", 3) == 0)
			break;
		buf[n] = '\0';
		r->reads++;
		bytes += n;
		last = now_sec();
		if (sscanf(buf, "msg %d:", &seq) == 1)
			record(seq, &highest, r);
	}
	close(sock);
	finish(r, bytes, last - t_req);
	return 0;
}

int new_sock(int type)
{
	int sock = socket(PF_INET, type, 0), rcvbuf = RCVBUF;
	struct timeval tv = {2, 0};

	if (sock == -1)
		error_handling("socket() error");
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));  /* before connect() */
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return sock;
}

void show_addresses(const char *name, int sock)
{
	struct sockaddr_in local, peer;
	socklen_t sz = sizeof(local);
	char lip[INET_ADDRSTRLEN], pip[INET_ADDRSTRLEN];

	getsockname(sock, (struct sockaddr *)&local, &sz);
	sz = sizeof(peer);
	getpeername(sock, (struct sockaddr *)&peer, &sz);
	inet_ntop(AF_INET, &local.sin_addr, lip, sizeof(lip));  /* separate buffers: inet_ntoa() */
	inet_ntop(AF_INET, &peer.sin_addr, pip, sizeof(pip));   /* would reuse one static buffer */
	printf("[%s] client %s:%d  ->  server %s:%d\n", name, lip, ntohs(local.sin_port),
	       pip, ntohs(peer.sin_port));
}

void record(int seq, int *highest, Result *r)
{
	if (seq < 0 || seq >= COUNT)
		return;
	if (seen[seq]) r->dups++;
	else { seen[seq] = 1; r->received++; }
	if (seq < *highest) r->out_of_order++;
	else *highest = seq;
}

void finish(Result *r, long bytes, double elapsed)
{
	r->lost = COUNT - r->received;
	r->elapsed = elapsed;
	r->mbps = elapsed > 0 ? bytes / elapsed / 1e6 : 0.0;
}

void print_report(Result *r, int n)
{
	char tmp[24];
	int i, faster;

	printf("\n%-22s", "");
	for (i = 0; i < n; i++) printf("%14s", r[i].name);
	printf("\n%-22s", "Connect time (ms)");
	for (i = 0; i < n; i++) printf("%14.3f", r[i].connect_ms);
	printf("\n%-22s", "Transfer time (s)");
	for (i = 0; i < n; i++) printf("%14.4f", r[i].elapsed);
	printf("\n%-22s", "Throughput (MB/s)");
	for (i = 0; i < n; i++) printf("%14.2f", r[i].mbps);
	printf("\n%-22s", "Delivered");
	for (i = 0; i < n; i++) { snprintf(tmp, sizeof(tmp), "%d/%d", r[i].received, COUNT); printf("%14s", tmp); }
	printf("\n%-22s", "Lost");
	for (i = 0; i < n; i++) printf("%14d", r[i].lost);
	printf("\n%-22s", "Out of order");
	for (i = 0; i < n; i++) printf("%14d", r[i].out_of_order);
	printf("\n%-22s", "Duplicates");
	for (i = 0; i < n; i++) printf("%14d", r[i].dups);
	printf("\n%-22s", "Receiver reads");
	for (i = 0; i < n; i++) printf("%14d", r[i].reads);

	printf("\n\nObservations from this run:\n");
	for (i = 0; i < n; i++)
		printf("- %s: %d/%d delivered, %d lost, %d out of order, %.4fs.\n", r[i].name,
		       r[i].received, COUNT, r[i].lost, r[i].out_of_order, r[i].elapsed);
	if (n == 2) {
		faster = r[0].elapsed <= r[1].elapsed ? 0 : 1;
		if (r[1].lost)
			printf("- %s finished first, but UDP's time covers only the data that arrived.\n", r[faster].name);
		else
			printf("- %s finished first; UDP lost nothing this time.\n", r[faster].name);
	}
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
