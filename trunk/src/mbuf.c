
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

#include "ppp.h"

/*
 * Malloc()
 *
 * Replacement for the ususal malloc()
 */

void *
Malloc(const char *type, int size)
{
  Mbuf	bp;

  bp = mballoc(type, size + sizeof(Mbuf));
  memcpy(MBDATA(bp), &bp, sizeof(bp));
  return(MBDATA(bp) + sizeof(Mbuf));
}

/*
 * Freee()
 *
 * Replacement for the ususal free()
 */

void
Freee(const char *type, const void *ptr)
{
  Mbuf	bp;

  if (ptr == NULL)
    return;
  bp = *((Mbuf *) ptr - 1);
  PFREE(bp);
}

/*
 * mballoc()
 *
 * Allocate an mbuf with memory
 */

Mbuf
mballoc(const char *type, int size)
{
  u_char	*memory;
  u_long	amount;
  Mbuf		bp;

  amount = sizeof(*bp) + size;

  if ((memory = MALLOC(type, amount)) == NULL)
  {
    Perror("mballoc: malloc");
    DoExit(EX_ERRDEAD);
  }
  memset(memory, 0, amount);

/* Put mbuf at front of memory region */

  bp = (Mbuf)(void *)memory;
  bp->size = bp->cnt = size;
  bp->type = type;

  bp->base = memory + sizeof(*bp);

  return(bp);
}

/*
 * mbfree()
 *
 * Free head of chain, return next
 */

Mbuf
mbfree(Mbuf bp)
{
  Mbuf	next;

  if (bp)
  {

   /* Sanity checks */

    assert(bp->base);
    assert(bp == (Mbuf)(void *)(bp->base - sizeof(*bp)));

    #ifdef MBUF_CHECK_OVERRUNS
      assert(*((u_int32_t *)(void *)(bp->base
	- sizeof(u_int32_t))) == MBUF_MAGIC_1);
      assert(*((u_int32_t *)(void *)(bp->base
	+ bp->size)) == MBUF_MAGIC_2);
    #endif

  /* Free it */

    next = bp->next;
    bp->base = NULL;
    FREE(bp->type, bp);
    return(next);
  }
  return(NULL);
}

/*
 * mbread()
 *
 * Read contents of an mbuf chain into buffer, consuming len bytes.
 * If all of the chain is consumed, return NULL.
 *
 * This should ALWAYS be called like this:
 *	bp = mbread(bp, ... );
 */

Mbuf
mbread(Mbuf bp, u_char *buf, int remain, int *nreadp)
{
  int	nread, total;

  for (total = 0; bp && remain > 0; total += nread)
  {
    if (remain > bp->cnt)
      nread = bp->cnt;
    else
      nread = remain;
    memcpy(buf, MBDATA(bp), nread);
    buf += nread;
    remain -= nread;
    bp->offset += nread;
    bp->cnt -= nread;
    while (bp != NULL && bp->cnt == 0)
      bp = mbfree(bp);
  }
  if (nreadp != NULL)
    *nreadp = total;
  return(bp);
}

/*
 * mbcopy()
 *
 * Copy contents of an mbuf chain into buffer, up to "remain" bytes.
 * This does not consume any of the mbuf chain. Returns number copied.
 */

int
mbcopy(Mbuf bp, u_char *buf, int remain)
{
  int	nread, total;

  for (total = 0; bp && remain > 0; total += nread, bp = bp->next)
  {
    if (remain > bp->cnt)
      nread = bp->cnt;
    else
      nread = remain;
    memcpy(buf, MBDATA(bp), nread);
    buf += nread;
    remain -= nread;
  }
  return(total);
}

/*
 * mbwrite()
 *
 * Write bytes from buffer into an mbuf chain. Returns first argument.
 */

Mbuf
mbwrite(Mbuf bp, const u_char *buf, int len)
{
  Mbuf	wp;
  int	chunk;

  for (wp = bp; wp && len > 0; wp = wp->next) {
    chunk = (len > wp->cnt) ? wp->cnt : len;
    memcpy(MBDATA(wp), buf, chunk);
    buf += chunk;
    len -= chunk;
  }
  return(bp);
}

/*
 * mbtrunc()
 *
 * Truncate mbuf chain to total of "max" bytes. If max is zero
 * then a zero length mbuf is returned (rather than a NULL mbuf).
 */

Mbuf
mbtrunc(Mbuf bp, int max)
{
  Mbuf	wp;
  int	sum;

/* Find mbuf in chain where truncation point happens */

  for (sum = 0, wp = bp;
    wp && sum + wp->cnt <= max;
    sum += wp->cnt, wp = wp->next);

/* Shorten this mbuf and nuke others after this one */

  if (wp)
  {
    wp->cnt = max - sum;
    PFREE(wp->next);
  }

/* Done */

  return(bp);
}

/*
 * mbunify()
 *
 * Collect all of a chain into a single mbuf
 *
 * This should ALWAYS be called like this:
 *	bp = mbunify(bp);
 */

Mbuf
mbunify(Mbuf bp)
{
  Mbuf	new;
  int	len;

  if (!bp || !bp->next)
    return(bp);
  new = mballoc(bp->type, len = plength(bp));
  assert(mbread(bp, MBDATA(new), len, NULL) == NULL);
  return(new);
}

/*
 * mbclean()
 *
 * Remove zero (and negative!?) length mbufs from a chain
 */

Mbuf
mbclean(Mbuf bp)
{
  Mbuf	*pp;

  pp = &bp;
  while (*pp)
    if ((*pp)->cnt <= 0)
      *pp = mbfree(*pp);
    else
      pp = &(*pp)->next;
  return(bp);
}

/*
 * mbsplit()
 *
 * Break an mbuf chain after "cnt" bytes.
 * Return the newly created mbuf chain that
 * starts after "cnt" bytes. If plength(bp) <= cnt,
 * then returns NULL.  The first part of
 * the chain remains pointed to by "bp".
 */

Mbuf
mbsplit(Mbuf bp, int cnt)
{
  int	seen, extra, tail;
  Mbuf	mextra, next;

/* Find mbuf in chain containing the breakpoint */

  for (seen = 0; bp && seen + bp->cnt < cnt; seen += bp->cnt, bp = bp->next);
  if (bp == NULL)
    return(NULL);

/* "tail" is how much stays in first part, "extra" goes into second part */

  tail = cnt - seen;
  extra = bp->cnt - tail;

/* Split in the middle of "bp" if necessary, creating "mextra" */

  if (extra > 0)
  {
    mextra = mballoc(bp->type, extra);
    memcpy(MBDATA(mextra), MBDATA(bp) + tail, extra);
    bp->cnt = tail;
    mextra->next = bp->next;
    bp->next = mextra;
  }

/* Now break point is just after "bp", so break the chain there */

  next = bp->next;
  bp->next = NULL;
  return(next);
}

/*
 * MemStat()
 */

int
MemStat(int ac, char *av[], void *arg)
{
  int	i;

  struct typed_mem_stats stats;

  if (typed_mem_usage(&stats) == -1) {
    Log(LG_ERR, ("%s", strerror(errno)));
    return(-1);
  }

  for (i = 0; i < structs_array_length(&typed_mem_stats_type, NULL, &stats); i++) {
    char f[10];
    struct typed_mem_typestats *s = NULL;

    snprintf(f, sizeof(f), "%d", i);

    if (structs_get(&typed_mem_stats_type, f, &stats, s) == -1) {
      Log(LG_ERR, ("%s", strerror(errno)));
      return(-1);
    }

    printf("%20s %8d Bytes %8d Blocks\n", s->type, s->bytes, s->allocs);
    FREE("typed_mem_stats", s);
  }

  structs_free(&typed_mem_stats_type, NULL, &stats);
  return(0);
}

