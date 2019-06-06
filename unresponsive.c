#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <unistd.h>
#include <fcntl.h>

static int server_port;
static int single_client;
static int response_delay;

void dolog(int fd, const char *fmt, va_list ap)
{
	char buf[512];
	struct tm tm;
	time_t now = time(NULL);
	size_t n = strftime(buf, sizeof(buf), "[%T]", localtime_r(&now, &tm));
	n += sprintf(buf + n, " [%ld] ", (long)getpid());
	n += vsprintf(buf + n, fmt, ap);
	buf[n++] = '\n';
	write(fd, buf, n);
}

void info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	dolog(1, fmt, ap);
	va_end(ap);
}

void error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	dolog(2, fmt, ap);
	va_end(ap);
}

void xerror(const char *message)
{
	error("%s: %s\n", message, strerror(errno));
	exit(1);
}

int writestr(int fd, const char *name, const char *str)
{
	size_t len = strlen(str);
	while (len)
	{
		ssize_t n = write(fd, str, len);
		if (n == -1)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			error("[%s] %s", name, strerror(errno));
			return -1;
		}

		str += n;
		len -= n;
	}

	return 0;
}

void respond_slowly(int fd, struct sockaddr_in sa)
{
	static const char hello[] = "Hello, world!\r\n";

	char name[256];
	char buf[4096];
	ssize_t used = 0;
	int http = 0;
	long end = time(NULL) + response_delay;
	int eof = 0;
	fd_set rfds;

	struct hostent *hent = gethostbyaddr(&sa.sin_addr, sizeof(sa.sin_addr), AF_INET);
	sprintf(name, "%s:%d", hent ? hent->h_name : inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));

	info("[%s] CONNECTED", name);

	int fl = fcntl(fd, F_GETFL);
	fl |= O_NONBLOCK;
	fcntl(fd, F_SETFL, fl);

	for (;;)
	{
		struct timeval tv;
		time_t now = time(NULL);
		int remaining = end - now;
		if (remaining <= 0)
			break;

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);

		tv.tv_sec = remaining;
		tv.tv_usec = 0;
		errno = 0;
		if (select(fd + 1, &rfds, NULL, NULL, &tv) == 1)
		{
			ssize_t avail = sizeof(buf) - used;
			char *p = avail > 0 ? buf + used : buf;
			ssize_t n = avail > 0 ? avail : sizeof(buf);

			n = read(fd, p, n);
			if (n == 0)
			{
				eof = 1;
				info("[%s] EOF", name);
				break;
			}

			if (n == -1)
			{
				if (errno != EAGAIN && errno != EINTR)
				{
					error("[%s] %s", name, strerror(errno));
					goto done;
				}
			}
			else
			{
				info("[%s] Received %d bytes", name, (int)n);
				if (avail)
					used += n;

				if (avail && !http
					&& (memmem(buf, used, "HTTP/1.0\r\n", 10)
						|| memmem(buf, used, "HTTP/1.1\r\n", 10))) {
					info("[%s] %.*s", name, (int)(strchr(buf, '\r') - buf), buf);
					http = 1;
				}
			}
		}
		else if (errno && errno != EINTR)
		{
			error("[%s] select: %s", name, strerror(errno));
			goto done;
		}
	}
	
	if (http)
	{
		eof |= writestr(fd, name, "HTTP/1.1 503 Service Unavailable\r\n");
		eof |= writestr(fd, name, "Content-Type: text/plain\r\n");
		if (!eof)
		{
			info("[%s] Sent HTTP 503", name);
		}
	}
	
	if (!eof)
	{
		for (;;)
		{
			int remaining = end - time(NULL);
			if (remaining <= 0)
				break;

			if (!eof && read(fd, buf, sizeof(buf)) == 0)
			{
				eof = 1;
				info("[%s] EOF\n", name);
				break;
			}

			info("[%s] %d seconds remaining", name, remaining);
			sleep(remaining > 10 ? 10 : remaining);
		}

		if (!eof)
		{
			if (http)
				writestr(fd, name, "Content-Length: 0\r\n\r\n");
			else
				writestr(fd, name, hello);
		}
	}

done:
	shutdown(fd, SHUT_RDWR);
	close(fd);
	info("[%s] CLOSED", name);
}

void server(void)
{
	int sock;
	int opt;
	struct sockaddr_in sa;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((short)server_port);
	sa.sin_addr.s_addr = INADDR_ANY;
	
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == -1)
		xerror("socket");
	
	opt = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
		xerror("setsockopt");

	if (bind(sock, (struct sockaddr*)&sa, sizeof(sa)) == -1)
		xerror("bind");
	
	if (listen(sock, 5) == -1)
		xerror("listen");
	
	for (;;)
	{
		socklen_t len = sizeof(sa);
		int client = accept(sock, (struct sockaddr*)&sa, &len);
		if (client == -1)
			xerror("accept");

		fflush(NULL);

		if (!single_client)
		{
			pid_t pid = fork();
			if (pid == (pid_t)-1)
				xerror("fork");

			if (pid == 0)
			{
				respond_slowly(client, sa);
				exit(1);
			}
			else
			{
				close(client);
			}
		}
		else
		{
			respond_slowly(client, sa);
		}
	}

	close(sock);
}
			
void reaper(int signal)
{
	int status;
	pid_t pid;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
		info("Reaped %ld", (long)pid);
}

void syntax()
{
	printf("Syntax: unresponsive [OPTIONS] PORT DELAY\n");
	printf("Options:\n");
	printf("  -1   only one client\n");
}

int main(int argc, char *argv[])
{
	int i;
	char *port = NULL;
	char *delay = NULL;

	signal(SIGCHLD, reaper);
	signal(SIGPIPE, SIG_IGN);

	for (i = 1; i < argc; i++)
	{
		if (strcmp("-1", argv[i]) == 0)
		{
			single_client = 1;
		}
		else if (argv[i][0] == '-')
		{
			fprintf(stderr, "%s: unrecognized option: %s\n", argv[0], argv[i]);
			exit(1);
		}
		else if (port == NULL)
		{
			port = argv[i];
		}
		else if (delay == NULL)
		{
			delay = argv[i];
		}
		else
		{
			fprintf(stderr, "%s: too many arguments\n", argv[0]);
			exit(1);
		}
	}

	if (!port || !delay)
	{
		syntax();
		exit(1);
	}

	server_port = atoi(port);
	response_delay = atoi(delay);
	
	if (server_port <= 0 || response_delay <= 0)
	{
		syntax();
		exit(1);
	}

	server();

	return 0;
}
