/* A little hacked up program that listens on a given port and allows a script
to play the part of a remote MTA for testing purposes. This scripted version is
hacked from my original interactive version. A further hack allows it to listen
on a Unix domain socket as an alternative to a TCP/IP port.

In an IPv6 world, listening happens on both an IPv6 and an IPv4 socket, always
on all interfaces, unless the option -noipv6 is given. */

/* ANSI C standard includes */

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Unix includes */

#include <errno.h>
#include <dirent.h>
#include <sys/types.h>

#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>

#ifdef HAVE_NETINET_IP_VAR_H
#include <netinet/ip_var.h>
#endif

#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>

#ifdef AF_INET6
#define HAVE_IPV6 1
#endif

#ifndef S_ADDR_TYPE
#define S_ADDR_TYPE u_long
#endif


typedef struct line {
  struct line *next;
  char line[1];
} line;


/*************************************************
*            SIGALRM handler - crash out         *
*************************************************/

static void
sigalrm_handler(int sig)
{
sig = sig;    /* Keep picky compilers happy */
printf("\nServer timed out\n");
exit(99);
}


/*************************************************
*          Get textual IP address                *
*************************************************/

/* This function is copied from Exim */

char *
host_ntoa(const void *arg, char *buffer)
{
char *yield;

/* The new world. It is annoying that we have to fish out the address from
different places in the block, depending on what kind of address it is. It
is also a pain that inet_ntop() returns a const char *, whereas the IPv4
function inet_ntoa() returns just char *, and some picky compilers insist
on warning if one assigns a const char * to a char *. Hence the casts. */

#if HAVE_IPV6
char addr_buffer[46];
int family = ((struct sockaddr *)arg)->sa_family;
if (family == AF_INET6)
  {
  struct sockaddr_in6 *sk = (struct sockaddr_in6 *)arg;
  yield = (char *)inet_ntop(family, &(sk->sin6_addr), addr_buffer,
    sizeof(addr_buffer));
  }
else
  {
  struct sockaddr_in *sk = (struct sockaddr_in *)arg;
  yield = (char *)inet_ntop(family, &(sk->sin_addr), addr_buffer,
    sizeof(addr_buffer));
  }

/* If the result is a mapped IPv4 address, show it in V4 format. */

if (strncmp(yield, "::ffff:", 7) == 0) yield += 7;

#else /* HAVE_IPV6 */

/* The old world */

yield = inet_ntoa(((struct sockaddr_in *)arg)->sin_addr);
#endif

strcpy(buffer, yield);
return buffer;
}


/*************************************************
*                 Main Program                   *
*************************************************/

#define v6n 0    /* IPv6 socket number */
#define v4n 1    /* IPv4 socket number */
#define udn 2    /* Unix domain socket number */
#define skn 2    /* Potential number of sockets */

int main(int argc, char **argv)
{
int i;
int port = 0;
int listen_socket[3] = { -1, -1, -1 };
int accept_socket;
int dup_accept_socket;
int connection_count = 1;
int count;
int on = 1;
int timeout = 5;
int use_ipv4 = 1;
int use_ipv6 = 1;
int debug = 0;
int na = 1;
line *script = NULL;
line *last = NULL;
line *s;
FILE *in, *out;

char *sockname = NULL;
unsigned char buffer[10240];

struct sockaddr_un sockun;            /* don't use "sun" */
struct sockaddr_un sockun_accepted;
int sockun_len = sizeof(sockun_accepted);

#if HAVE_IPV6
struct sockaddr_in6 sin6;
struct sockaddr_in6 accepted;
struct in6_addr anyaddr6 =  IN6ADDR_ANY_INIT ;
#else
struct sockaddr_in accepted;
#endif

/* Always need an IPv4 structure */

struct sockaddr_in sin4;

int len = sizeof(accepted);


/* Sort out the arguments */

while (na < argc && argv[na][0] == '-')
  {
  if (strcmp(argv[na], "-d") == 0) debug = 1;
  else if (strcmp(argv[na], "-t") == 0) timeout = atoi(argv[++na]);
  else if (strcmp(argv[na], "-noipv4") == 0) use_ipv4 = 0;
  else if (strcmp(argv[na], "-noipv6") == 0) use_ipv6 = 0;
  else
    {
    printf("server: unknown option %s\n", argv[na]);
    exit(1);
    }
  na++;
  }

if (!use_ipv4 && !use_ipv6)
  {
  printf("server: -noipv4 and -noipv6 cannot both be given\n");
  exit(1);
  }

if (na >= argc)
  {
  printf("server: no port number or socket name given\n");
  exit(1);
  }

if (argv[na][0] == '/')
  {
  sockname = argv[na];
  unlink(sockname);       /* in case left lying around */
  }
else port = atoi(argv[na]);
na++;

if (na < argc) connection_count = atoi(argv[na]);


/* Create sockets */

if (port == 0)  /* Unix domain */
  {
  if (debug) printf("Creating Unix domain socket\n");
  listen_socket[udn] = socket(PF_UNIX, SOCK_STREAM, 0);
  if (listen_socket[udn] < 0)
    {
    printf("Unix domain socket creation failed: %s\n", strerror(errno));
    exit(1);
    }
  }
else
  {
  #if HAVE_IPV6
  if (use_ipv6)
    {
    if (debug) printf("Creating IPv6 socket\n");
    listen_socket[v6n] = socket(AF_INET6, SOCK_STREAM, 0);
    if (listen_socket[v6n] < 0)
      {
      printf("IPv6 socket creation failed: %s\n", strerror(errno));
      exit(1);
      }

    /* If this is an IPv6 wildcard socket, set IPV6_V6ONLY if that option is
    available. */

    #ifdef IPV6_V6ONLY
    if (setsockopt(listen_socket[v6n], IPPROTO_IPV6, IPV6_V6ONLY, (char *)(&on),
          sizeof(on)) < 0)
      printf("Setting IPV6_V6ONLY on IPv6 wildcard "
        "socket failed (%s): carrying on without it\n", strerror(errno));
    #endif  /* IPV6_V6ONLY */
    }
  #endif  /* HAVE_IPV6 */

  /* Create an IPv4 socket if required */

  if (use_ipv4)
    {
    if (debug) printf("Creating IPv4 socket\n");
    listen_socket[v4n] = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket[v4n] < 0)
      {
      printf("IPv4 socket creation failed: %s\n", strerror(errno));
      exit(1);
      }
    }
  }


/* Set SO_REUSEADDR on the IP sockets so that the program can be restarted
while a connection is being handled - this can happen as old connections lie
around for a bit while crashed processes are tidied away.  Without this, a
connection will prevent reuse of the smtp port for listening. */

for (i = v6n; i <= v4n; i++)
  {
  if (listen_socket[i] >= 0 &&
      setsockopt(listen_socket[i], SOL_SOCKET, SO_REUSEADDR, (char *)(&on),
        sizeof(on)) < 0)
    {
    printf("setting SO_REUSEADDR on socket failed: %s\n", strerror(errno));
    exit(1);
    }
  }


/* Now bind the sockets to the required port or path. If a path, ensure
anyone can write to it. */

if (port == 0)
  {
  struct stat statbuf;
  sockun.sun_family = AF_UNIX;
  if (debug) printf("Binding Unix domain socket\n");
  sprintf(sockun.sun_path, "%.*s", (int)(sizeof(sockun.sun_path)-1), sockname);
  if (bind(listen_socket[udn], (struct sockaddr *)&sockun, sizeof(sockun)) < 0)
    {
    printf("Unix domain socket bind() failed: %s\n", strerror(errno));
    exit(1);
    }
  (void)stat(sockname, &statbuf);
  if (debug) printf("Setting Unix domain socket mode: %0x\n",
    statbuf.st_mode | 0777);
  if (chmod(sockname, statbuf.st_mode | 0777) < 0)
    {
    printf("Unix domain socket chmod() failed: %s\n", strerror(errno));
    exit(1);
    }
  }

else
  {
  for (i = 0; i < skn; i++)
    {
    if (listen_socket[i] < 0) continue;

    /* For an IPv6 listen, use an IPv6 socket */

    #if HAVE_IPV6
    if (i == v6n)
      {
      memset(&sin6, 0, sizeof(sin6));
      sin6.sin6_family = AF_INET6;
      sin6.sin6_port = htons(port);
      sin6.sin6_addr = anyaddr6;
      if (bind(listen_socket[i], (struct sockaddr *)&sin6, sizeof(sin6)) < 0)
        {
        printf("IPv6 socket bind() failed: %s\n", strerror(errno));
        exit(1);
        }
      }
    else
    #endif

    /* For an IPv4 bind, use an IPv4 socket, even in an IPv6 world. If an IPv4
    bind fails EADDRINUSE after IPv6 success, carry on, because it means the
    IPv6 socket will handle IPv4 connections. */

      {
      memset(&sin4, 0, sizeof(sin4));
      sin4.sin_family = AF_INET;
      sin4.sin_addr.s_addr = (S_ADDR_TYPE)INADDR_ANY;
      sin4.sin_port = htons(port);
      if (bind(listen_socket[i], (struct sockaddr *)&sin4, sizeof(sin4)) < 0)
        {
        if (listen_socket[v6n] < 0 || errno != EADDRINUSE)
          {
          printf("IPv4 socket bind() failed: %s\n", strerror(errno));
          exit(1);
          }
        else
          {
          close(listen_socket[i]);
          listen_socket[i] = -1;
          }
        }
      }
    }
  }


/* Start listening. If IPv4 fails EADDRINUSE after IPv6 succeeds, ignore the
error because it means that the IPv6 socket will handle IPv4 connections. Don't
output anything, because it will mess up the test output, which will be
different for systems that do this and those that don't. */

for (i = 0; i <= skn; i++)
  {
  if (listen_socket[i] >= 0 && listen(listen_socket[i], 5) < 0)
    {
    if (i != v4n || listen_socket[v6n] < 0 || errno != EADDRINUSE)
      {
      printf("listen() failed: %s\n", strerror(errno));
      exit(1);
      }
    }
  }


/* This program handles only a fixed number of connections, in sequence. Before
waiting for the first connection, read the standard input, which contains the
script of things to do. A line containing "++++" is treated as end of file.
This is so that the Perl driving script doesn't have to close the pipe -
because that would cause it to wait for this process, which it doesn't yet want
to do. The driving script adds the "++++" automatically - it doesn't actually
appear in the test script. */

while (fgets(buffer, sizeof(buffer), stdin) != NULL)
  {
  line *next;
  int n = (int)strlen(buffer);
  while (n > 0 && isspace(buffer[n-1])) n--;
  buffer[n] = 0;
  if (strcmp(buffer, "++++") == 0) break;
  next = malloc(sizeof(line) + n);
  next->next = NULL;
  strcpy(next->line, buffer);
  if (last == NULL) script = last = next;
    else last->next = next;
  last = next;
  }

fclose(stdin);

/* SIGALRM handler crashes out */

signal(SIGALRM, sigalrm_handler);

/* s points to the current place in the script */

s = script;

for (count = 0; count < connection_count; count++)
  {
  alarm(timeout);
  if (port <= 0)
    {
    printf("Listening on %s ... ", sockname);
    fflush(stdout);
    accept_socket = accept(listen_socket[udn],
      (struct sockaddr *)&sockun_accepted, &sockun_len);
    }

  else
    {
    int lcount;
    int max_socket = 0;
    fd_set select_listen;

    printf("Listening on port %d ... ", port);
    fflush(stdout);

    FD_ZERO(&select_listen);
    for (i = 0; i < skn; i++)
      {
      if (listen_socket[i] >= 0) FD_SET(listen_socket[i], &select_listen);
      if (listen_socket[i] > max_socket) max_socket = listen_socket[i];
      }

    lcount = select(max_socket + 1, &select_listen, NULL, NULL, NULL);
    if (lcount < 0)
      {
      printf("Select failed\n");
      fflush(stdout);
      continue;
      }

    accept_socket = -1;
    for (i = 0; i < skn; i++)
      {
      if (listen_socket[i] > 0 && FD_ISSET(listen_socket[i], &select_listen))
        {
        accept_socket = accept(listen_socket[i],
          (struct sockaddr *)&accepted, &len);
        FD_CLR(listen_socket[i], &select_listen);
        break;
        }
      }
    }
  alarm(0);

  if (accept_socket < 0)
    {
    printf("accept() failed: %s\n", strerror(errno));
    exit(1);
    }

  out = fdopen(accept_socket, "w");

  dup_accept_socket = dup(accept_socket);

  if (port > 0)
    printf("\nConnection request from [%s]\n", host_ntoa(&accepted, buffer));
  else
    {
    printf("\nConnection request\n");

    /* Linux supports a feature for acquiring the peer's credentials, but it
    appears to be Linux-specific. This code is untested and unused, just
    saved here for reference. */

    /**********--------------------
    struct ucred cr;
    int cl=sizeof(cr);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl)==0) {
      printf("Peer's pid=%d, uid=%d, gid=%d\n",
              cr.pid, cr.uid, cr.gid);
    --------------*****************/
    }

  if (dup_accept_socket < 0)
    {
    printf("Couldn't dup socket descriptor\n");
    printf("421 Connection refused: %s\n", strerror(errno));
    fprintf(out, "421 Connection refused: %s\r\n", strerror(errno));
    fclose(out);
    exit(2);
    }

  in = fdopen(dup_accept_socket, "r");

  /* Loop for handling the conversation(s). For use in SMTP sessions, there are
  default rules for determining input and output lines: the latter start with
  digits. This means that the input looks like SMTP dialog. However, this
  doesn't work for other tests (e.g. ident tests) so we have explicit '<' and
  '>' flags for input and output as well as the defaults. */

  for (; s != NULL; s = s->next)
    {
    char *ss = s->line;

    /* Output lines either start with '>' or a digit. In the '>' case we can
    fudge the sending of \r\n as required. Default is \r\n, ">>" send nothing,
    ">CR>" sends \r only, and ">LF>" sends \n only. We can also force a
    connection closedown by ">*eof". */

    if (ss[0] == '>')
      {
      char *end = "\r\n";
      printf("%s\n", ss++);

      if (strncmp(ss, "*eof", 4) == 0)
        {
        s = s->next;
        goto END_OFF;
        }

      if (*ss == '>')
        { end = ""; ss++; }
      else if (strncmp(ss, "CR>", 3) == 0)
        { end = "\r"; ss += 3; }
      else if (strncmp(ss, "LF>", 3) == 0)
        { end = "\n"; ss += 3; }

      fprintf(out, "%s%s", ss, end);
      }

    else if (isdigit((unsigned char)ss[0]))
      {
      printf("%s\n", ss);
      fprintf(out, "%s\r\n", ss);
      }

    /* If the script line starts with "*sleep" we just sleep for a while
    before continuing. */

    else if (strncmp(ss, "*sleep ", 7) == 0)
      {
      int sleepfor = atoi(ss+7);
      printf("%s\n", ss);
      fflush(out);
      sleep(sleepfor);
      }

    /* Otherwise the script line is the start of an input line we are expecting
    from the client, or "*eof" indicating we expect the client to close the
    connection. Read command line or data lines; the latter are indicated
    by the expected line being just ".". If the line starts with '<', that
    doesn't form part of the expected input. (This allows for incoming data
    starting with a digit.) */

    else
      {
      int offset;
      int data = strcmp(ss, ".") == 0;

      if (ss[0] == '<')
        {
        buffer[0] = '<';
        offset = 1;
        }
      else offset = 0;

      fflush(out);

      for (;;)
        {
        int n;
        alarm(timeout);
        if (fgets(buffer+offset, sizeof(buffer)-offset, in) == NULL)
          {
          printf("%sxpected EOF read from client\n",
            (strncmp(ss, "*eof", 4) == 0)? "E" : "Une");
          s = s->next;
          goto END_OFF;
          }
        alarm(0);
        n = (int)strlen(buffer);
        while (n > 0 && isspace(buffer[n-1])) n--;
        buffer[n] = 0;
        printf("%s\n", buffer);
        if (!data || strcmp(buffer, ".") == 0) break;
        }

      if (strncmp(ss, buffer, (int)strlen(ss)) != 0)
        {
        printf("Comparison failed - bailing out\n");
        printf("Expected: %s\n", ss);
        break;
        }
      }
    }

  END_OFF:
  fclose(in);
  fclose(out);
  }

if (s == NULL) printf("End of script\n");

if (sockname != NULL) unlink(sockname);
exit(0);
}

/* End of server.c */
