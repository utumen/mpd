
/*
 * mbuf.c
 *
 * Written by Toshiharu OHNO <tony-o@iij.ad.jp>
 * Copyright (c) 1993, Internet Initiative Japan, Inc. All rights reserved.
 * See ``COPYRIGHT.iij''
 * 
 * Rewritten by Archie Cobbs <archie@freebsd.org>
 * Copyright (c) 1995-1999 Whistle Communications, Inc. All rights reserved.
 * See ``COPYRIGHT.whistle''
 */

#ifndef _MBUF_H_
#define _MBUF_H_

/*
 * DEFINITIONS
 */

  struct mpdmbuf {
    int			size;		/* size allocated */
    int			offset;		/* offset to start position */
    int			cnt;		/* available byte count in buffer */
  };

  typedef struct mpdmbuf	*Mbuf;

  /* Macros */
  #define MBDATAU(bp)	((u_char *)(bp) + sizeof(struct mpdmbuf) + (bp)->offset)
  #define MBDATA(bp)	((bp) ? MBDATAU(bp) : NULL)
  #define MBLEN(bp)	((bp) ? (bp)->cnt : 0)
  #define MBSPACE(bp)	((bp) ? (bp)->size - (bp)->offset : 0)

  /* Types of allocated memory */
  #define MB_AUTH	"AUTH"
  #define MB_CONS	"CONSOLE"
  #define MB_WEB	"WEB"
  #define MB_IFACE	"IFACE"
  #define MB_BUND	"BUND"
  #define MB_REP	"REP"
  #define MB_LINK	"LINK"
  #define MB_CHAT	"CHAT"
  #define MB_CMD	"CMD"
  #define MB_CMDL	"CMDL"
  #define MB_COMP	"COMP"
  #define MB_CRYPT	"CRYPT"
  #define MB_ECHO	"ECHO"
  #define MB_EVENT	"EVENT"
  #define MB_FSM	"FSM"
  #define MB_LOG	"LOG"
  #define MB_MP		"MP"
  #define MB_MBUF	"MBUF"
  #define MB_PHYS	"PHYS"
  #define MB_PPTP	"PPTP"
  #define MB_RADIUS	"RADIUS"
  #define MB_UTIL	"UTIL"
  #define MB_VJCOMP	"VJCOMP"
  #define MB_IPPOOL	"IPPOOL"

/*
 * FUNCTIONS
 */

/* Replacements for malloc() & free() */

  extern void	*Malloc(const char *type, size_t size);
  extern void	*Mdup(const char *type, const void *src, size_t size);
  extern void	*Mstrdup(const char *type, const void *src);
  extern void	Freee(void *ptr);

/* Mbuf manipulation */

  extern Mbuf	mballoc(int size);
  extern void	mbfree(Mbuf bp);
  extern Mbuf	mbread(Mbuf bp, void *ptr, int cnt);
  extern int	mbcopy(Mbuf bp, int offset, void *buf, int cnt);
  extern Mbuf	mbcopyback(Mbuf bp, int offset, const void *buf, int cnt);
  extern Mbuf	mbtrunc(Mbuf bp, int max);
  extern Mbuf	mbadj(Mbuf bp, int cnt);
  extern Mbuf	mbsplit(Mbuf bp, int cnt);

/* Etc */

  extern int	MemStat(Context ctx, int ac, char *av[], void *arg);
  extern void	DumpBp(Mbuf bp);

#endif
