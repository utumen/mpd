
/*
 * msg.h
 *
 * Written by Archie Cobbs <archie@freebsd.org>
 * Copyright (c) 1995-1999 Whistle Communications, Inc. All rights reserved.
 * See ``COPYRIGHT.whistle''
 */

#ifndef _MSG_H_
#define _MSG_H_

/*
 * DEFINITIONS
 */

/* Messages you can send to a link or a bundle */

  #define MSG_OPEN		1	/* Bring yourself up */
  #define MSG_CLOSE		2	/* Bring yourself down */
  #define MSG_UP		3	/* Lower layer went up */
  #define MSG_DOWN		4	/* Lower layer went down */

/* Forward decl */

  struct msghandler;
  typedef struct msghandler	*MsgHandler;

/*
 * FUNCTIONS
 */

  extern MsgHandler	MsgRegister(void (*func)(int typ, void *arg), int pri);
  extern void		MsgUnRegister(MsgHandler *m);
  extern void		MsgSend(MsgHandler m, int type, void *arg);
  extern const char	*MsgName(int msg);

#endif

