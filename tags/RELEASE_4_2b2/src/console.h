
/*
 * console.h
 *
 * Written by Archie Cobbs <archie@freebsd.org>
 * Copyright (c) 1998-1999 Whistle Communications, Inc. All rights reserved.
 * See ``COPYRIGHT.whistle''
 */

#ifndef _CONSOLE_H_
#define	_CONSOLE_H_

#include "ppp.h"

/*
 * DEFINITIONS
 */

  #define MAX_CONSOLE_ARGS	50
  #define MAX_CONSOLE_LINE	400
  #define MAX_CONSOLE_BUF_LEN	4096

  #define Printf(fmt, args...)	({ 						\
	  			  if (ctx->cs) { 				\
	  			    ctx->cs->write(ctx->cs, fmt, ## args);	\
	  			  } else {					\
				    printf(fmt, ## args);			\
				  } 						\
  				})

  /* Configuration options */
  enum {
    CONSOLE_LOGGING,	/* enable logging */
  };

  struct console {
    int			fd;		/* listener */
    struct u_addr 	addr;
    in_port_t		port;
    struct ghash	*users;		/* allowed users */
    SLIST_HEAD(, console_session) sessions;	/* active sessions */
    EventRef		event;		/* connect-event */
    pthread_rwlock_t	lock;
  };

  typedef struct console *Console;

  struct console_session;

  typedef struct console_session *ConsoleSession;

  struct context {
	Bund		bund;
	Link		lnk;
	Rep		rep;
	PhysInfo	phys;
	ConsoleSession	cs;
  };

  struct console_user {
    char	*username;
    char	*password;
  };

  typedef struct console_user *ConsoleUser;

  struct console_session {
    Console		console;
    struct optinfo	options;	/* Configured options */
    char		active;		/* console active at this moment */
    int			fd;		/* connection fd */
    void		*cookie;	/* device dependent cookie */
    EventRef		readEvent;
    struct context	context;
    struct console_user	user;
    struct u_addr	peer_addr;
    in_port_t           peer_port;
    char		cmd[MAX_CONSOLE_LINE];
    int			cmd_len;
    void		(*prompt)(struct console_session *);
    void		(*write)(struct console_session *, const char *fmt, ...);
    void		(*writev)(struct console_session *, const char *fmt, va_list vl);
    void		(*close)(struct console_session *);
    int			state;
    int			telnet;
    int			escaped;
    char		history[MAX_CONSOLE_LINE];	/* last command */
    SLIST_ENTRY(console_session)	next;
  };

/*
 * VARIABLES
 */

  extern const struct cmdtab ConsoleSetCmds[];


/*
 * FUNCTIONS
 */

  extern int	ConsoleInit(Console c);
  extern int	ConsoleOpen(Console c);
  extern int	ConsoleClose(Console c);
  extern int	ConsoleStat(Context ctx, int ac, char *av[], void *arg);
  extern Context	StdConsoleConnect(Console c);

#endif
