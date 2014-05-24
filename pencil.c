/* pencil, a network socket caching intermediate layer
 *         to sit between apache and php.
*/

#include <stdio.h>
#include <stdarg.h>
#include <syslog.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pwd.h>

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define false 0
#define true !false

#define FLUSH_FORCE_FALSE false
#define FLUSH_FORCE_TRUE  true

#define P_LOGERR_X(lev, str) logmsg(lev, str ": %s", strerror(errno))
#define P_LOGERR_CRIT(str) P_LOGERR_X(LOG_CRIT,    str)
#define P_LOGERR_ERR(str)  P_LOGERR_X(LOG_ERR,     str)
#define P_LOGERR_WARN(str) P_LOGERR_X(LOG_WARNING, str)
#define P_LOGERR_INFO(str) P_LOGERR_X(LOG_INFO,    str)

#define FD_OPEN           1
#define FD_CLOSED         2
#define FD_CONNECTING     3
#define FD_WAIT_FOR_RETRY 4 /* future development maybe */
struct Socket
{
	char *buf;
	int buf_allocated, buf_len, buf_writepos;
	int fd, fd_state;

	struct timeval opened_at, closed_at;
};
struct Connection
{
	/* stuff that's buffered to send to apache */
	struct Socket apache;

	/* stuff that's buffered to send to php */
	struct Socket php;
};

struct EndPoint
{
	/* pointers into argv for reference only */
	char *host, *port;

	/* for split_ip_port to remember if these parts were explicitly
	 * given on the commandline (if not we can auto-fill some later)
	*/
	int host_supplied, port_supplied;

	/* after parsing, it's fed into getaddrinfo() which fills this in */
	struct addrinfo *res;
};
struct Conf
{
	struct EndPoint local;
	struct EndPoint remote;

	struct EndPoint *remote_backups;
	unsigned short remote_backups_size, remote_backups_cur;

	int foreground, detached;
	
	struct passwd *run_as;

	int nonblocking_connects;
} *conf;

/* kq contains FDs that we wait on in the main loop. kq_w is a special case
 * temporary queue that we use to test if a socket is ready to write to in
 * realtime */
int kq, kq_w;

int fd_acceptor;

int time_to_die, dying_gracefully, open_connections;

struct timespec kqpoll;

static void
sigint_handler(int s) /* {{{ */
{
	time_to_die = s;
} /* }}} */

static void
logmsg(int priority, const char *fmt, ...) /* {{{ */
{
	va_list args;
	va_start(args, fmt);

	if (! conf->detached)
	{
		struct tm now_tm;
		time_t now = time(NULL);
		localtime_r(&now, &now_tm);

		printf("%04d-%02d-%02d %02d:%02d:%02d ",
		    now_tm.tm_year+1900, now_tm.tm_mon+1, now_tm.tm_mday,
		    now_tm.tm_hour, now_tm.tm_min, now_tm.tm_sec);
		vprintf(fmt, args);
		printf("\n");
	}
	else if (priority != LOG_DEBUG)
		vsyslog(priority, fmt, args);
} /* }}} */

static int
accept_new_connection(void)
{
	int tmp, fd_incoming, fd_outgoing;
	struct Connection *c;
	struct kevent kev[2];
	struct addrinfo *res;

	/* accept a new connection from fd_acceptor */
	if ((fd_incoming = accept(fd_acceptor, NULL, NULL)) == -1)
	{
		if (errno == ECONNABORTED)
		{
			P_LOGERR_WARN("accept");
			return -1;
		}

		P_LOGERR_CRIT("accept");
		exit(1);
	}

	open_connections++;

	c = calloc(sizeof(struct Connection), 1);

	/* initialise the rest of the connection struct explicitly */
	c->apache.buf = NULL;
	c->apache.buf_writepos = 0;
	c->apache.buf_len = 0;
	c->apache.buf_allocated = 0;
	c->php.buf = NULL;
	c->php.buf_writepos = 0;
	c->php.buf_len = 0;
	c->php.buf_allocated = 0;

	c->apache.fd = fd_incoming;
	c->apache.fd_state = FD_OPEN;
	gettimeofday(&c->apache.opened_at, NULL);

	/* split everythign below here into a new function? (try_connect) ? */


	res = conf->remote.res;
	if ((fd_outgoing = socket(res->ai_family,
	                         res->ai_socktype,
	                         res->ai_protocol)) == -1)
	{
		P_LOGERR_CRIT("socket");
		exit(1);
	}

	c->php.fd = fd_outgoing;

	if (conf->nonblocking_connects)
	{
		tmp = fcntl(fd_outgoing, F_GETFL, NULL);
		tmp &= O_NONBLOCK;
		fcntl(fd_outgoing, F_SETFL, tmp);
	}

	if ((connect(fd_outgoing, res->ai_addr, res->ai_addrlen)) == -1)
	{
		if (errno == EINPROGRESS)
		{
			c->php.fd_state = FD_CONNECTING;

			/* come back and check this socket later */
			EV_SET(kev, fd_outgoing, EVFILT_WRITE, EV_ADD|EV_ONESHOT, 0, 0, c);
			kevent(kq, kev, 1, NULL, 0, 0);

			return 0;
		}

		/* connect didn't work */

		P_LOGERR_CRIT("connect");
		close(fd_incoming);
		close(fd_outgoing);
		open_connections--;
		free(c);
		return -1;
	}

	c->php.fd_state = FD_OPEN;
	gettimeofday(&c->php.opened_at, NULL);

	/* monitor the new connections */
	EV_SET(&kev[0], c->php.fd, EVFILT_READ, EV_ADD, 0, 0, c);
	EV_SET(&kev[1], c->apache.fd, EVFILT_READ, EV_ADD, 0, 0, c);
	kevent(kq, kev, 2, NULL, 0, 0);


	/*logmsg(LOG_DEBUG, "newconn: php=%d apache=%d", c->php.fd, c->apache.fd);*/

	return 0;
}


static int
reset_socket(struct Socket *s)
{
	if (s->buf_allocated > 0)
	{
		free(s->buf);
		s->buf = NULL;
		s->buf_len = 0;
		s->buf_allocated = 0;
		s->buf_writepos = 0;
	}

	if (s->fd)
	{
		close(s->fd);
		s->fd = 0;
		s->fd_state = FD_CLOSED;
		gettimeofday(&s->closed_at, NULL);
	}

	return 0;
}

static int
reset_connection(struct Connection *c)
{

#if DEBUG_LOG_TIMERS
	struct timeval dphp, dapache, ddiff;
#endif

	reset_socket(&c->apache);
	reset_socket(&c->php);

#if DEBUG_LOG_TIMERS
	timersub(&c->php.closed_at, &c->php.opened_at, &dphp);
	timersub(&c->apache.closed_at, &c->apache.opened_at, &dapache);
	timersub(&dapache, &dphp, &ddiff);
	if (ddiff.tv_sec > 3)
	{
		logmsg(LOG_INFO, "php duration=%d.%d, apache duration=%d.%d",
		    dphp.tv_sec, dphp.tv_usec, dapache.tv_sec, dapache.tv_usec);
	}
#endif

	return 0;
}

static int
complete_nonblocking_connect(struct kevent *kev)
{
	int arg;
	struct kevent tkev[2];
	struct Socket *s;
	struct Connection *c;

	struct sockaddr_storage t;
	socklen_t tlen = sizeof(t);

	/* php file descriptor should be FD_CONNECTING */
	c = kev->udata;
	s = &c->php;

	if (s->fd_state != FD_CONNECTING)
		return -1; /* ?? we shouldn't have been called */

	if ((getpeername(s->fd, (struct sockaddr *)&t, &tlen)) == -1)
	{
		P_LOGERR_ERR("nonblocking connect failed");
		reset_connection(c);
		return -1;
	}
	s->fd_state = FD_OPEN;
	gettimeofday(&s->opened_at, NULL);

	/* turn off nonblocking again */
	arg = fcntl(s->fd, F_GETFL, NULL); 
	arg &= (~O_NONBLOCK); 
	fcntl(s->fd, F_SETFL, arg); 

	/* monitor the new connections */
	EV_SET(&tkev[0], c->php.fd, EVFILT_READ, EV_ADD, 0, 0, c);
	EV_SET(&tkev[1], c->apache.fd, EVFILT_READ, EV_ADD, 0, 0, c);
	kevent(kq, tkev, 2, NULL, 0, 0);

	/*logmsg(LOG_DEBUG, "nonblocknewconn: php=%d apache=%d", c->php.fd, c->apache.fd); */

	return 0;
}

static int
process_read(struct kevent *kev)
{
	/* a socket we are listening to has done something. figure out what
	 * it is, and do whatever necessary to clear it up */

	struct Connection *c = kev->udata; /* Connection lookup */
	struct Socket *rs, *ws;
	char *readdst;

	int n;
	struct kevent t_kev, t_kev_r; /* temporary kev-> */

	/* get the socket we're going to write to */
	if ((int)kev->ident == c->php.fd) /* kev->ident == reading socket */
	{
		rs = &c->php;
		ws = &c->apache;
	}
	else
	{
		rs = &c->apache;
		ws = &c->php;
	}

	if (kev->data > 0)
	{
		/* there is some data to read */
		/*logmsg(LOG_DEBUG, "%d has %d bytes to read", kev->ident, * kev->data);*/

		if (! ws->fd)
		{
			/* there is no socket available to write to. discard. */
			readdst = alloca(kev->data);
			read(kev->ident, readdst, kev->data);

			goto CHECK_CLOSE; /* fix this later */
		}

		/* see if the partner FD has sufficient buffer space to
		 * accept the data we're about to read. If not, we'll buffer
		 * it. */

		/* get bufspace remaining for write at tfd */
		EV_SET(&t_kev, ws->fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, 0);
		n = kevent(kq_w, &t_kev, 1, &t_kev_r, 1, &kqpoll);

		if (n != 1 || ws->buf_len || t_kev_r.data < kev->data)
		{
			int nread;

			/* we get here in the following situations:
			 * - the kev->nt call failed
			 * - there is pending data to be written; we have to
			 *   keep queueing, to keep it in order
			 * - there is insufficient write buffer space
			*/
			EV_SET(&t_kev, ws->fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
			n = kevent(kq_w, &t_kev, 1, NULL, 0, 0);

			/* store the data to be written, and wait for the
			 * target fd to become ready in the main loop */
			EV_SET(&t_kev, ws->fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, c);
			kevent(kq, &t_kev, 1, NULL, 0, 0);

			/* allocate enough space to store read data */
			if (!ws->buf ||
			    ws->buf_allocated < ws->buf_len + kev->data)
			{
				int x = 0, newsize;
				char *newbuf;

				newsize = (ws->buf_len * 2) + kev->data;
				while (++x * 2 < newsize); newsize = x * 2;

				newbuf = (char *)realloc(ws->buf, newsize);
				if (! newbuf) abort();

				ws->buf = newbuf;
				ws->buf_allocated = newsize;
			}

			/* pointer into ws->buf to read into */
			readdst = ws->buf + ws->buf_len;

			nread = read(kev->ident, readdst, kev->data);
			if (nread == -1)
			{
				P_LOGERR_CRIT("read");
				abort();
			}

			ws->buf_len += nread;

			/*logmsg(LOG_DEBUG, "stored %d bytes, now at %d/%d", nread, *twritepos, *tbuflen);*/
		}
		else
		{
			/* we can directly copy */

			/* use a temporary buffer */
			char *buf = malloc(kev->data);
			int nread = read(kev->ident, buf, kev->data);
			write(ws->fd, buf, nread);
			free(buf);

			/*logmsg(LOG_DEBUG, "directly copied %d bytes (kqueue said %d) of data from %d to %d", nread, kev->data, kev->ident, tfd);*/
		}

	}

CHECK_CLOSE:
	/* after doing the read, see if the socket has closed */
	if (kev->flags & EV_EOF)
	{
		/* no more reads from the other end; it'd have nowhere to go */
		/*shutdown(ws->fd, SHUT_RD);*/

		/*logmsg(LOG_DEBUG, "%d has closed", rs->fd);*/

		/* if there is data left to be sent to this fd, it has to die */
		reset_socket(rs);

		if (ws->buf_len == 0)
		{
			/* no data left to be passed, can close the other end */
			reset_socket(ws);
		}
	}

	return 0;
}

#define MIN(a,b) (a) < (b) ? (a) : (b);
#define MAX(a,b) (a) > (b) ? (a) : (b);
static int
process_write(struct kevent *kev)
{
	struct Connection *c = kev->udata; /* Connection lookup */
	struct Socket *rs, *ws;
	int bytes_to_write, nwritten;

	/*logmsg(LOG_DEBUG, "%d woke us up for write", kev->ident);*/

	/* get the socket we're going to write to */
	if ((int)kev->ident == c->php.fd) /* kev->ident == writing socket */
	{
		rs = &c->apache;
		ws = &c->php;
	}
	else
	{
		rs = &c->php;
		ws = &c->apache;
	}

	/* see if this is a nonblocking connect which has returned */
	if (ws->fd_state == FD_CONNECTING)
	{
		return complete_nonblocking_connect(kev);
	}

	/* do not attempt to write to the socket if it has been closed */
	if (kev->flags & EV_EOF)
	{
		/*logmsg(LOG_DEBUG, "%d has closed", kev->ident);*/

		/* no more reads from the other end; it'd have nowhere to go */
		/*shutdown(rs->fd, SHUT_RD);*/

		reset_socket(ws);

		/* see if the other end can be closed too */
		if (rs->buf_len == 0)
			reset_socket(rs);

		return -1;
	}

	/* work out how much we can send */
	bytes_to_write = MIN(ws->buf_len - ws->buf_writepos, kev->data);

	nwritten = write(ws->fd, ws->buf + ws->buf_writepos,bytes_to_write);
	if (nwritten == -1)
	{
		if (errno == EPIPE || errno == ECONNRESET)
		{
			/* Broken pipe, indicates the receiver has gone away */
			reset_connection(c);

			return -1;
		}
		else
		{
			/* another unhandled error code */
			P_LOGERR_WARN("write");
			abort();
		}
	}

	/*logmsg(LOG_DEBUG, "sent %d bytes to %d, actually wrote %d", bytes_to_write, tfd, nwritten);*/

	ws->buf_writepos += nwritten;

	/* see if there's more left to write. If there is, we need another
	 * notification when the socket is ready again, and we need to shift
	 * the to-write pointer along. If not, we can clean up */
	if (ws->buf_writepos < ws->buf_len)
	{
		/* we wrote less than we wanted; ergo there's some left */

		/* we need to be told when they're ready again */
		struct kevent t_kev;
		EV_SET(&t_kev, ws->fd, EVFILT_WRITE, EV_ADD|EV_ONESHOT, 0,0, c);
		kevent(kq, &t_kev, 1, NULL, 0, 0);
	}
	else
	{
		/* the buffer is now empty, so we can clean it up */
		free(ws->buf);
		ws->buf = NULL;
		ws->buf_len = 0;
		ws->buf_writepos = 0;
		ws->buf_allocated = 0;

		/* if the other end has closed, close this end */
		if (rs->fd_state == FD_CLOSED)
			reset_socket(ws);
	}

	return 0;
}

static int
split_ip_port(char *arg, struct EndPoint *ep, int hostreq, int portreq)
{
	/* split arg into an ip and port, using : as the separator
	 * if hostreq/portreq are false, allow those to remain unset
	*/

	char *s;

	if (! *arg) return -1;

	s = strchr(arg, ':');

	/* unused atm, we never require both */
	#if 0
	if (!s && portreq && hostreq)
	{
		fprintf(stderr, "host:port format required in '%s'\n", arg);
		return -1;
	}
	#endif

	/* just a port? */
	if (!s && portreq && !hostreq)
	{
		ep->port = arg;
		ep->host_supplied = false;
		ep->port_supplied = true;
		return 0;
	}

	/* just a host? */
	if (!s && !portreq && hostreq)
	{
		ep->host = arg;
		ep->host_supplied = true;
		ep->port_supplied = false;
		return 0;
	}

	/* both */

	/* first part is a host */
	*s = (char)NULL;

	ep->host = arg;
	ep->host_supplied = true;

	/* check that we have something after the : */
	if (! *(s+1))
	{
		*s = ':';
		fprintf(stderr, "no port found in '%s'\n", arg);
		return -1;
	}

	ep->port = s+1;
	ep->port_supplied = true;


	return 0;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage:\n"
	    "  pencil [-f] [-u user] [-c maxconns] [-m cachelimit] -l [ip:]port -r ip[:port]\n"
	    "      [-b ip[:port] -b .. ]\n"
	    "\n"
	    " -f             Run in foreground; do not daemonise\n"
	    " -u user        Run as user/uid (default: no change)\n"
	    " -c maxconns    NDY cap on connections to proxy simultaneously\n"
	    " -m cachelimit  NDY cap on total cached bytes\n"
	    " -l [ip:]port   listen on local ip:port\n"
	    "                  ip is optional (will use INADDR_ANY if not set)\n"
	    " -r ip[:port]   primarily connect to remote ip:port\n"
	    "                  port is optional (will inherit localport if not set)\n"
	    " -b ip[:port]   backups for remote connection (repeat for more)\n"
	    "                  port is optional (will inherit localport if not set)\n"
	    "                  (NOT WORKING YET)\n"
	    "\n");

	exit(1);
}

int main(int argc, char *argv[])
{
	int i, nevents, ch, y = 1;
	struct kevent kev;
	struct sigaction sa;

	char *p;
	int error, digit;
	struct addrinfo hints, *res;

	openlog("pencil", LOG_NDELAY | LOG_PID, LOG_LOCAL3);

	kq = kqueue();
	kq_w = kqueue();

	bzero(&kqpoll, sizeof(struct timespec));

	sa.sa_handler = sigint_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	signal(SIGPIPE, SIG_IGN);

	conf = calloc(sizeof(struct Conf), 1);
	conf->local.host = INADDR_ANY;
	conf->local.port = 0;
	conf->remote.host = 0;
	conf->remote.port = 0;

	conf->remote_backups = NULL;
	conf->remote_backups_cur = 0;
	conf->remote_backups_size = 0;

	conf->run_as = NULL; /* run as root unless instructed otherwise */
	conf->detached = false; /* not detached yet */
	conf->foreground = false; /* detach by default */
	conf->nonblocking_connects = true;

	while ((ch = getopt(argc, argv, "fl:r:b:u:")) != -1)
	{
		switch(ch)
		{
			case 'f':
			conf->foreground = true;
			break;

			case 'l':
			if ((split_ip_port(optarg, &conf->local,
			    false, /* host not required */
			    true /* port required */)) == -1)
				usage();
			break;

			case 'r':
			if ((split_ip_port(optarg, &conf->remote,
			    true, /* host required */
			    false /* port not required */)) == -1)
				usage();
			break;

			case 'b':
			conf->remote_backups = realloc(conf->remote_backups,
			    sizeof(struct EndPoint) * conf->remote_backups_size+1);
			if ((split_ip_port(optarg, &conf->remote_backups[conf->remote_backups_size],
			    true, /* host required */
			    false /* port not required */)) == -1)
				usage();
			conf->remote_backups_size++;
			break;

			case 'u':
			if ((conf->run_as = getpwnam(optarg)))
				continue;

			for(digit = true, p = optarg ; *p && digit ; p++)
				if (!isdigit(*p)) digit = false;

			if (!digit)
				logmsg(LOG_ERR, "unknown user: %s", optarg);
			else if (!(conf->run_as = getpwuid(atoi(optarg))))
				logmsg(LOG_ERR, "unknown uid: %s", optarg);

			break;

			default: usage(); break;
		}
	}

	/* fill in any missing configuration bits {{{ */
	if (!conf->local.port)
		usage();
	if (!conf->remote.host)
		usage();
	if (! conf->local.host_supplied)
		conf->local.host = NULL;
	
	if (! conf->remote.port_supplied)
		conf->remote.port = conf->local.port;

	for(i = 0 ; i < conf->remote_backups_size ; i++)
	{
		if (! conf->remote_backups[i].port_supplied)
			conf->remote_backups[i].port = conf->local.port;
	}
	/* }}} */

#define POPULATE_ENDPOINT(ep, hints) \
    getaddrinfo(ep.host, ep.port, hints, &ep.res);
	/* populate endpoint addrinfo structs {{{ */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	error = POPULATE_ENDPOINT(conf->local, &hints);
	if (error)
	{
		logmsg(LOG_ERR, "getaddrinfo: %s", gai_strerror(error));
		exit(1);
	}
	hints.ai_flags = 0;
	error = POPULATE_ENDPOINT(conf->remote, &hints);
	if (error)
	{
		logmsg(LOG_ERR, "getaddrinfo: %s", gai_strerror(error));
		exit(1);
	}
	for(i = 0 ; i < conf->remote_backups_size ; i++)
	{
		error = POPULATE_ENDPOINT(conf->remote_backups[i], &hints);
		if (error)
		{
			logmsg(LOG_ERR, "getaddrinfo: %s", gai_strerror(error));
			exit(1);
		}
	}
	/* }}} */


	/* prepare acceptor for incoming connections {{{ */
	res = conf->local.res;
	if ((fd_acceptor = socket(res->ai_family,
				  res->ai_socktype,
				  res->ai_protocol)) == -1)
	{
		P_LOGERR_CRIT("socket");
		exit(1);
	}

	if (setsockopt(fd_acceptor, SOL_SOCKET, SO_REUSEADDR, &y,sizeof(y)) < 0)
		P_LOGERR_WARN("setsockopt");

	while (bind(fd_acceptor, res->ai_addr, res->ai_addrlen) < 0)
	{
		P_LOGERR_ERR("bind (waiting 1s)");

		sleep(1);

		if (time_to_die) exit(1);
	}
	if (listen(fd_acceptor, 10) < 0)
	{
		P_LOGERR_CRIT("listen");
		exit(1);
	}
	logmsg(LOG_INFO, "got my listening socket!");
	/* }}} */

	/* add acceptor to kqueue, so we're told of connections from apache */
	EV_SET(&kev, fd_acceptor, EVFILT_READ, EV_ADD, 0, 0, 0);
	kevent(kq, &kev, 1, NULL, 0, 0);

	/* detach from terminal if necessary */
	if (! conf->foreground)
	{
		int fd; 
		switch(rfork(RFPROC | RFNOWAIT))
		{
			case -1: P_LOGERR_CRIT("fork"); exit(255); /* fail */
			case  0: break;                         /* child */
			default: exit(0);                       /* parent */
		}

		setsid(); /* make a new process group */

		fd = open("/dev/null", O_RDWR);
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > 2) close(fd);

		chroot("/var/empty");
		chdir("/");

		conf->detached = true;
	}

	/* drop root */
	if (conf->run_as && conf->run_as->pw_uid > 0)
	{
		if (setgid(conf->run_as->pw_gid))
			P_LOGERR_ERR("setgid");
		if (setuid(conf->run_as->pw_uid))
			P_LOGERR_ERR("setuid");
	}

	time_to_die = 0;
	dying_gracefully = 0;
	open_connections = 0;
	/* wait for something to happen */
	for(;;)
	{
		if (time_to_die && dying_gracefully)
			break; /* catch user insistence */
		else if (time_to_die)
		{
			close(fd_acceptor); /* no new connections */
			time_to_die = 0;
			dying_gracefully = 1;
			logmsg(LOG_INFO, "dying gracefully, signal again to die quickly");
		}

		if (dying_gracefully && !open_connections)
			break; /* die gracefully */

		nevents = kevent(kq, NULL, 0, &kev, 1, NULL);

		if (nevents == -1 && errno == EINTR) continue;
		if (nevents == -1)
		{
			P_LOGERR_CRIT("kevent");
			exit(1);
		}

		if ((int)kev.ident == fd_acceptor)
		{
			/* a new connection */
			accept_new_connection();
		}
		else
		{
			struct Connection *c = kev.udata;

			if (kev.filter == EVFILT_READ)
				process_read(&kev);

			if (kev.filter == EVFILT_WRITE)
				process_write(&kev);

/*			logmsg(LOG_DEBUG, "fd:%d bufs: to_php=%d/%d bytes, to_apache=%d/%d bytes", kev.ident, c->php.buf_writepos, c->php.buf_len, c->apache.buf_writepos, c->apache.buf_len);*/

			if (c->apache.fd_state == FD_CLOSED &&
			    c->php.fd_state == FD_CLOSED)
			{
				reset_connection(c);
				open_connections--;
				free(c);
			}

			if (dying_gracefully)
			{
				logmsg(LOG_DEBUG, "open_connections=%d", open_connections);
			}
		}
	}

	logmsg(LOG_INFO, "pencil shutting down");

	/* exiting, and no I can't be bothered to free everything */

	return 0;
}
