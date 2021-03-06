/*-
 * Copyright (c) 1996-1999
 * Kazutaka YOKOTA (yokota@zodiac.mech.utsunomiya-u.ac.jp)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote 
 *    products derived from this software without specific prior written 
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: atkbdc.c,v 1.1 1999/01/09 02:44:50 yokota Exp $
 * from kbdio.c,v 1.13 1998/09/25 11:55:46 yokota Exp
 */

#include "atkbdc.h"
#include "opt_kbd.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/syslog.h>

#include <machine/clock.h>

#include <dev/kbd/atkbdcreg.h>

#ifndef __i386__
#include <isa/isareg.h>
#else
#include <i386/isa/isa.h>
#endif

/* constants */

#define MAXKBDC		MAX(NATKBDC, 1)

/* macros */

#ifndef MAX
#define MAX(x, y)	((x) > (y) ? (x) : (y))
#endif

#define kbdcp(p)	((atkbdc_softc_t *)(p))
#define nextq(i)	(((i) + 1) % KBDQ_BUFSIZE)
#define availq(q)	((q)->head != (q)->tail)
#if KBDIO_DEBUG >= 2
#define emptyq(q)	((q)->tail = (q)->head = (q)->qcount = 0)
#else
#define emptyq(q)	((q)->tail = (q)->head = 0)
#endif

/* local variables */

/*
 * We always need at least one copy of the kbdc_softc struct for the
 * low-level console.  As the low-level console accesses the keyboard
 * controller before kbdc, and all other devices, is probed, we
 * statically allocate one entry. XXX
 */
static atkbdc_softc_t default_kbdc;
static atkbdc_softc_t *atkbdc_softc[MAXKBDC] = { &default_kbdc };

static int verbose = KBDIO_DEBUG;

/* function prototypes */

static int atkbdc_setup(atkbdc_softc_t *sc, int port);
static int addq(kqueue *q, int c);
static int removeq(kqueue *q);
static int wait_while_controller_busy(atkbdc_softc_t *kbdc);
static int wait_for_data(atkbdc_softc_t *kbdc);
static int wait_for_kbd_data(atkbdc_softc_t *kbdc);
static int wait_for_kbd_ack(atkbdc_softc_t *kbdc);
static int wait_for_aux_data(atkbdc_softc_t *kbdc);
static int wait_for_aux_ack(atkbdc_softc_t *kbdc);

#if NATKBDC > 0

atkbdc_softc_t
*atkbdc_get_softc(int unit)
{
	atkbdc_softc_t *sc;

	if (unit >= sizeof(atkbdc_softc)/sizeof(atkbdc_softc[0]))
		return NULL;
	sc = atkbdc_softc[unit];
	if (sc == NULL) {
		sc = atkbdc_softc[unit]
		   = malloc(sizeof(*sc), M_DEVBUF, M_NOWAIT);
		if (sc == NULL)
			return NULL;
		bzero(sc, sizeof(*sc));
		sc->port = -1;	/* XXX */
	}
	return sc;
}

int
atkbdc_probe_unit(atkbdc_softc_t *sc, int unit, int port)
{
	return atkbdc_setup(sc, port);
}

#endif /* NATKBDC > 0 */

/* the backdoor to the keyboard controller! XXX */
int
atkbdc_configure(void)
{
	return atkbdc_setup(atkbdc_softc[0], -1);
}

static int
atkbdc_setup(atkbdc_softc_t *sc, int port)
{
	if (port <= 0)
		port = IO_KBD;

	if (sc->port <= 0) {
	    sc->command_byte = -1;
	    sc->command_mask = 0;
	    sc->lock = FALSE;
	    sc->kbd.head = sc->kbd.tail = 0;
	    sc->aux.head = sc->aux.tail = 0;
#if KBDIO_DEBUG >= 2
	    sc->kbd.call_count = 0;
	    sc->kbd.qcount = sc->kbd.max_qcount = 0;
	    sc->aux.call_count = 0;
	    sc->aux.qcount = sc->aux.max_qcount = 0;
#endif
	}
	sc->port = port;	/* may override the previous value */
	return 0;
}

/* associate a port number with a KBDC */

KBDC 
kbdc_open(int port)
{
    int s;
    int i;

    if (port <= 0)
	port = IO_KBD;

    s = spltty();
    for (i = 0; i < sizeof(atkbdc_softc)/sizeof(atkbdc_softc[0]); ++i) {
	if (atkbdc_softc[i] == NULL)
	    continue;
	if (atkbdc_softc[i]->port == port) {
	    splx(s);
	    return (KBDC)atkbdc_softc[i];
	}
	if (atkbdc_softc[i]->port <= 0) {
	    if (atkbdc_setup(atkbdc_softc[i], port))
		break;
	    splx(s);
	    return (KBDC)atkbdc_softc[i];
	}
    }
    splx(s);
    return NULL;
}

/*
 * I/O access arbitration in `kbdio'
 *
 * The `kbdio' module uses a simplistic convention to arbitrate
 * I/O access to the controller/keyboard/mouse. The convention requires
 * close cooperation of the calling device driver.
 *
 * The device driver which utilizes the `kbdio' module are assumed to
 * have the following set of routines.
 *    a. An interrupt handler (the bottom half of the driver).
 *    b. Timeout routines which may briefly polls the keyboard controller.
 *    c. Routines outside interrupt context (the top half of the driver).
 * They should follow the rules below:
 *    1. The interrupt handler may assume that it always has full access 
 *       to the controller/keyboard/mouse.
 *    2. The other routines must issue `spltty()' if they wish to 
 *       prevent the interrupt handler from accessing 
 *       the controller/keyboard/mouse.
 *    3. The timeout routines and the top half routines of the device driver
 *       arbitrate I/O access by observing the lock flag in `kbdio'.
 *       The flag is manipulated via `kbdc_lock()'; when one wants to
 *       perform I/O, call `kbdc_lock(kbdc, TRUE)' and proceed only if
 *       the call returns with TRUE. Otherwise the caller must back off.
 *       Call `kbdc_lock(kbdc, FALSE)' when necessary I/O operaion
 *       is finished. This mechanism does not prevent the interrupt 
 *       handler from being invoked at any time and carrying out I/O.
 *       Therefore, `spltty()' must be strategically placed in the device
 *       driver code. Also note that the timeout routine may interrupt
 *       `kbdc_lock()' called by the top half of the driver, but this
 *       interruption is OK so long as the timeout routine observes the
 *       the rule 4 below.
 *    4. The interrupt and timeout routines should not extend I/O operation
 *       across more than one interrupt or timeout; they must complete
 *       necessary I/O operation within one invokation of the routine.
 *       This measns that if the timeout routine acquires the lock flag,
 *       it must reset the flag to FALSE before it returns.
 */

/* set/reset polling lock */
int 
kbdc_lock(KBDC p, int lock)
{
    int prevlock;

    prevlock = kbdcp(p)->lock;
    kbdcp(p)->lock = lock;

    return (prevlock != lock);
}

/* check if any data is waiting to be processed */
int
kbdc_data_ready(KBDC p)
{
    return (availq(&kbdcp(p)->kbd) || availq(&kbdcp(p)->aux) 
	|| (inb(kbdcp(p)->port + KBD_STATUS_PORT) & KBDS_ANY_BUFFER_FULL));
}

/* queuing functions */

static int
addq(kqueue *q, int c)
{
    if (nextq(q->tail) != q->head) {
	q->q[q->tail] = c;
	q->tail = nextq(q->tail);
#if KBDIO_DEBUG >= 2
        ++q->call_count;
        ++q->qcount;
	if (q->qcount > q->max_qcount)
            q->max_qcount = q->qcount;
#endif
	return TRUE;
    }
    return FALSE;
}

static int
removeq(kqueue *q)
{
    int c;

    if (q->tail != q->head) {
	c = q->q[q->head];
	q->head = nextq(q->head);
#if KBDIO_DEBUG >= 2
        --q->qcount;
#endif
	return c;
    }
    return -1;
}

/* 
 * device I/O routines
 */
static int
wait_while_controller_busy(struct atkbdc_softc *kbdc)
{
    /* CPU will stay inside the loop for 100msec at most */
    int retry = 5000;
    int port = kbdc->port;
    int f;

    while ((f = inb(port + KBD_STATUS_PORT)) & KBDS_INPUT_BUFFER_FULL) {
	if ((f & KBDS_BUFFER_FULL) == KBDS_KBD_BUFFER_FULL) {
	    DELAY(KBDD_DELAYTIME);
	    addq(&kbdc->kbd, inb(port + KBD_DATA_PORT));
	} else if ((f & KBDS_BUFFER_FULL) == KBDS_AUX_BUFFER_FULL) {
	    DELAY(KBDD_DELAYTIME);
	    addq(&kbdc->aux, inb(port + KBD_DATA_PORT));
	}
        DELAY(KBDC_DELAYTIME);
        if (--retry < 0)
    	    return FALSE;
    }
    return TRUE;
}

/*
 * wait for any data; whether it's from the controller, 
 * the keyboard, or the aux device.
 */
static int
wait_for_data(struct atkbdc_softc *kbdc)
{
    /* CPU will stay inside the loop for 200msec at most */
    int retry = 10000;
    int port = kbdc->port;
    int f;

    while ((f = inb(port + KBD_STATUS_PORT) & KBDS_ANY_BUFFER_FULL) == 0) {
        DELAY(KBDC_DELAYTIME);
        if (--retry < 0)
    	    return 0;
    }
    DELAY(KBDD_DELAYTIME);
    return f;
}

/* wait for data from the keyboard */
static int
wait_for_kbd_data(struct atkbdc_softc *kbdc)
{
    /* CPU will stay inside the loop for 200msec at most */
    int retry = 10000;
    int port = kbdc->port;
    int f;

    while ((f = inb(port + KBD_STATUS_PORT) & KBDS_BUFFER_FULL) 
	    != KBDS_KBD_BUFFER_FULL) {
        if (f == KBDS_AUX_BUFFER_FULL) {
	    DELAY(KBDD_DELAYTIME);
	    addq(&kbdc->aux, inb(port + KBD_DATA_PORT));
	}
        DELAY(KBDC_DELAYTIME);
        if (--retry < 0)
    	    return 0;
    }
    DELAY(KBDD_DELAYTIME);
    return f;
}

/* 
 * wait for an ACK(FAh), RESEND(FEh), or RESET_FAIL(FCh) from the keyboard.
 * queue anything else.
 */
static int
wait_for_kbd_ack(struct atkbdc_softc *kbdc)
{
    /* CPU will stay inside the loop for 200msec at most */
    int retry = 10000;
    int port = kbdc->port;
    int f;
    int b;

    while (retry-- > 0) {
        if ((f = inb(port + KBD_STATUS_PORT)) & KBDS_ANY_BUFFER_FULL) {
	    DELAY(KBDD_DELAYTIME);
            b = inb(port + KBD_DATA_PORT);
	    if ((f & KBDS_BUFFER_FULL) == KBDS_KBD_BUFFER_FULL) {
		if ((b == KBD_ACK) || (b == KBD_RESEND) 
		    || (b == KBD_RESET_FAIL))
		    return b;
		addq(&kbdc->kbd, b);
	    } else if ((f & KBDS_BUFFER_FULL) == KBDS_AUX_BUFFER_FULL) {
		addq(&kbdc->aux, b);
	    }
	}
        DELAY(KBDC_DELAYTIME);
    }
    return -1;
}

/* wait for data from the aux device */
static int
wait_for_aux_data(struct atkbdc_softc *kbdc)
{
    /* CPU will stay inside the loop for 200msec at most */
    int retry = 10000;
    int port = kbdc->port;
    int f;

    while ((f = inb(port + KBD_STATUS_PORT) & KBDS_BUFFER_FULL) 
	    != KBDS_AUX_BUFFER_FULL) {
        if (f == KBDS_KBD_BUFFER_FULL) {
	    DELAY(KBDD_DELAYTIME);
	    addq(&kbdc->kbd, inb(port + KBD_DATA_PORT));
	}
        DELAY(KBDC_DELAYTIME);
        if (--retry < 0)
    	    return 0;
    }
    DELAY(KBDD_DELAYTIME);
    return f;
}

/* 
 * wait for an ACK(FAh), RESEND(FEh), or RESET_FAIL(FCh) from the aux device.
 * queue anything else.
 */
static int
wait_for_aux_ack(struct atkbdc_softc *kbdc)
{
    /* CPU will stay inside the loop for 200msec at most */
    int retry = 10000;
    int port = kbdc->port;
    int f;
    int b;

    while (retry-- > 0) {
        if ((f = inb(port + KBD_STATUS_PORT)) & KBDS_ANY_BUFFER_FULL) {
	    DELAY(KBDD_DELAYTIME);
            b = inb(port + KBD_DATA_PORT);
	    if ((f & KBDS_BUFFER_FULL) == KBDS_AUX_BUFFER_FULL) {
		if ((b == PSM_ACK) || (b == PSM_RESEND) 
		    || (b == PSM_RESET_FAIL))
		    return b;
		addq(&kbdc->aux, b);
	    } else if ((f & KBDS_BUFFER_FULL) == KBDS_KBD_BUFFER_FULL) {
		addq(&kbdc->kbd, b);
	    }
	}
        DELAY(KBDC_DELAYTIME);
    }
    return -1;
}

/* write a one byte command to the controller */
int
write_controller_command(KBDC p, int c)
{
    if (!wait_while_controller_busy(kbdcp(p)))
	return FALSE;
    outb(kbdcp(p)->port + KBD_COMMAND_PORT, c);
    return TRUE;
}

/* write a one byte data to the controller */
int
write_controller_data(KBDC p, int c)
{
    if (!wait_while_controller_busy(kbdcp(p)))
	return FALSE;
    outb(kbdcp(p)->port + KBD_DATA_PORT, c);
    return TRUE;
}

/* write a one byte keyboard command */
int
write_kbd_command(KBDC p, int c)
{
    if (!wait_while_controller_busy(kbdcp(p)))
	return FALSE;
    outb(kbdcp(p)->port + KBD_DATA_PORT, c);
    return TRUE;
}

/* write a one byte auxiliary device command */
int
write_aux_command(KBDC p, int c)
{
    if (!write_controller_command(p, KBDC_WRITE_TO_AUX))
	return FALSE;
    return write_controller_data(p, c);
}

/* send a command to the keyboard and wait for ACK */
int
send_kbd_command(KBDC p, int c)
{
    int retry = KBD_MAXRETRY;
    int res = -1;

    while (retry-- > 0) {
	if (!write_kbd_command(p, c))
	    continue;
        res = wait_for_kbd_ack(kbdcp(p));
        if (res == KBD_ACK)
    	    break;
    }
    return res;
}

/* send a command to the auxiliary device and wait for ACK */
int
send_aux_command(KBDC p, int c)
{
    int retry = KBD_MAXRETRY;
    int res = -1;

    while (retry-- > 0) {
	if (!write_aux_command(p, c))
	    continue;
	/*
	 * FIXME: XXX
	 * The aux device may have already sent one or two bytes of 
	 * status data, when a command is received. It will immediately 
	 * stop data transmission, thus, leaving an incomplete data 
	 * packet in our buffer. We have to discard any unprocessed
	 * data in order to remove such packets. Well, we may remove 
	 * unprocessed, but necessary data byte as well... 
	 */
	emptyq(&kbdcp(p)->aux);
        res = wait_for_aux_ack(kbdcp(p));
        if (res == PSM_ACK)
    	    break;
    }
    return res;
}

/* send a command and a data to the keyboard, wait for ACKs */
int
send_kbd_command_and_data(KBDC p, int c, int d)
{
    int retry;
    int res = -1;

    for (retry = KBD_MAXRETRY; retry > 0; --retry) {
	if (!write_kbd_command(p, c))
	    continue;
        res = wait_for_kbd_ack(kbdcp(p));
        if (res == KBD_ACK)
    	    break;
        else if (res != KBD_RESEND)
    	    return res;
    }
    if (retry <= 0)
	return res;

    for (retry = KBD_MAXRETRY, res = -1; retry > 0; --retry) {
	if (!write_kbd_command(p, d))
	    continue;
        res = wait_for_kbd_ack(kbdcp(p));
        if (res != KBD_RESEND)
    	    break;
    }
    return res;
}

/* send a command and a data to the auxiliary device, wait for ACKs */
int
send_aux_command_and_data(KBDC p, int c, int d)
{
    int retry;
    int res = -1;

    for (retry = KBD_MAXRETRY; retry > 0; --retry) {
	if (!write_aux_command(p, c))
	    continue;
	emptyq(&kbdcp(p)->aux);
        res = wait_for_aux_ack(kbdcp(p));
        if (res == PSM_ACK)
    	    break;
        else if (res != PSM_RESEND)
    	    return res;
    }
    if (retry <= 0)
	return res;

    for (retry = KBD_MAXRETRY, res = -1; retry > 0; --retry) {
	if (!write_aux_command(p, d))
	    continue;
        res = wait_for_aux_ack(kbdcp(p));
        if (res != PSM_RESEND)
    	    break;
    }
    return res;
}

/* 
 * read one byte from any source; whether from the controller,
 * the keyboard, or the aux device
 */
int
read_controller_data(KBDC p)
{
    if (availq(&kbdcp(p)->kbd)) 
        return removeq(&kbdcp(p)->kbd);
    if (availq(&kbdcp(p)->aux)) 
        return removeq(&kbdcp(p)->aux);
    if (!wait_for_data(kbdcp(p)))
        return -1;		/* timeout */
    return inb(kbdcp(p)->port + KBD_DATA_PORT);
}

#if KBDIO_DEBUG >= 2
static int call = 0;
#endif

/* read one byte from the keyboard */
int
read_kbd_data(KBDC p)
{
#if KBDIO_DEBUG >= 2
    if (++call > 2000) {
	call = 0;
	log(LOG_DEBUG, "kbdc: kbd q: %d calls, max %d chars, "
			     "aux q: %d calls, max %d chars\n",
		       kbdcp(p)->kbd.call_count, kbdcp(p)->kbd.max_qcount,
		       kbdcp(p)->aux.call_count, kbdcp(p)->aux.max_qcount);
    }
#endif

    if (availq(&kbdcp(p)->kbd)) 
        return removeq(&kbdcp(p)->kbd);
    if (!wait_for_kbd_data(kbdcp(p)))
        return -1;		/* timeout */
    return inb(kbdcp(p)->port + KBD_DATA_PORT);
}

/* read one byte from the keyboard, but return immediately if 
 * no data is waiting
 */
int
read_kbd_data_no_wait(KBDC p)
{
    int f;

#if KBDIO_DEBUG >= 2
    if (++call > 2000) {
	call = 0;
	log(LOG_DEBUG, "kbdc: kbd q: %d calls, max %d chars, "
			     "aux q: %d calls, max %d chars\n",
		       kbdcp(p)->kbd.call_count, kbdcp(p)->kbd.max_qcount,
		       kbdcp(p)->aux.call_count, kbdcp(p)->aux.max_qcount);
    }
#endif

    if (availq(&kbdcp(p)->kbd)) 
        return removeq(&kbdcp(p)->kbd);
    f = inb(kbdcp(p)->port + KBD_STATUS_PORT) & KBDS_BUFFER_FULL;
    if (f == KBDS_AUX_BUFFER_FULL) {
        DELAY(KBDD_DELAYTIME);
        addq(&kbdcp(p)->aux, inb(kbdcp(p)->port + KBD_DATA_PORT));
        f = inb(kbdcp(p)->port + KBD_STATUS_PORT) & KBDS_BUFFER_FULL;
    }
    if (f == KBDS_KBD_BUFFER_FULL) {
        DELAY(KBDD_DELAYTIME);
        return inb(kbdcp(p)->port + KBD_DATA_PORT);
    }
    return -1;		/* no data */
}

/* read one byte from the aux device */
int
read_aux_data(KBDC p)
{
    if (availq(&kbdcp(p)->aux)) 
        return removeq(&kbdcp(p)->aux);
    if (!wait_for_aux_data(kbdcp(p)))
        return -1;		/* timeout */
    return inb(kbdcp(p)->port + KBD_DATA_PORT);
}

/* read one byte from the aux device, but return immediately if 
 * no data is waiting
 */
int
read_aux_data_no_wait(KBDC p)
{
    int f;

    if (availq(&kbdcp(p)->aux)) 
        return removeq(&kbdcp(p)->aux);
    f = inb(kbdcp(p)->port + KBD_STATUS_PORT) & KBDS_BUFFER_FULL;
    if (f == KBDS_KBD_BUFFER_FULL) {
        DELAY(KBDD_DELAYTIME);
        addq(&kbdcp(p)->kbd, inb(kbdcp(p)->port + KBD_DATA_PORT));
        f = inb(kbdcp(p)->port + KBD_STATUS_PORT) & KBDS_BUFFER_FULL;
    }
    if (f == KBDS_AUX_BUFFER_FULL) {
        DELAY(KBDD_DELAYTIME);
        return inb(kbdcp(p)->port + KBD_DATA_PORT);
    }
    return -1;		/* no data */
}

/* discard data from the keyboard */
void
empty_kbd_buffer(KBDC p, int wait)
{
    int t;
    int b;
    int f;
#if KBDIO_DEBUG >= 2
    int c1 = 0;
    int c2 = 0;
#endif
    int delta = 2;

    for (t = wait; t > 0; ) { 
        if ((f = inb(kbdcp(p)->port + KBD_STATUS_PORT)) & KBDS_ANY_BUFFER_FULL) {
	    DELAY(KBDD_DELAYTIME);
            b = inb(kbdcp(p)->port + KBD_DATA_PORT);
	    if ((f & KBDS_BUFFER_FULL) == KBDS_AUX_BUFFER_FULL) {
		addq(&kbdcp(p)->aux, b);
#if KBDIO_DEBUG >= 2
		++c2;
            } else {
		++c1;
#endif
	    }
	    t = wait;
	} else {
	    t -= delta;
	}
        DELAY(delta*1000);
    }
#if KBDIO_DEBUG >= 2
    if ((c1 > 0) || (c2 > 0))
        log(LOG_DEBUG, "kbdc: %d:%d char read (empty_kbd_buffer)\n", c1, c2);
#endif

    emptyq(&kbdcp(p)->kbd);
}

/* discard data from the aux device */
void
empty_aux_buffer(KBDC p, int wait)
{
    int t;
    int b;
    int f;
#if KBDIO_DEBUG >= 2
    int c1 = 0;
    int c2 = 0;
#endif
    int delta = 2;

    for (t = wait; t > 0; ) { 
        if ((f = inb(kbdcp(p)->port + KBD_STATUS_PORT)) & KBDS_ANY_BUFFER_FULL) {
	    DELAY(KBDD_DELAYTIME);
            b = inb(kbdcp(p)->port + KBD_DATA_PORT);
	    if ((f & KBDS_BUFFER_FULL) == KBDS_KBD_BUFFER_FULL) {
		addq(&kbdcp(p)->kbd, b);
#if KBDIO_DEBUG >= 2
		++c1;
            } else {
		++c2;
#endif
	    }
	    t = wait;
	} else {
	    t -= delta;
	}
	DELAY(delta*1000);
    }
#if KBDIO_DEBUG >= 2
    if ((c1 > 0) || (c2 > 0))
        log(LOG_DEBUG, "kbdc: %d:%d char read (empty_aux_buffer)\n", c1, c2);
#endif

    emptyq(&kbdcp(p)->aux);
}

/* discard any data from the keyboard or the aux device */
void
empty_both_buffers(KBDC p, int wait)
{
    int t;
    int f;
#if KBDIO_DEBUG >= 2
    int c1 = 0;
    int c2 = 0;
#endif
    int delta = 2;

    for (t = wait; t > 0; ) { 
        if ((f = inb(kbdcp(p)->port + KBD_STATUS_PORT)) & KBDS_ANY_BUFFER_FULL) {
	    DELAY(KBDD_DELAYTIME);
            (void)inb(kbdcp(p)->port + KBD_DATA_PORT);
#if KBDIO_DEBUG >= 2
	    if ((f & KBDS_BUFFER_FULL) == KBDS_KBD_BUFFER_FULL)
		++c1;
            else
		++c2;
#endif
	    t = wait;
	} else {
	    t -= delta;
	}
	DELAY(delta*1000);
    }
#if KBDIO_DEBUG >= 2
    if ((c1 > 0) || (c2 > 0))
        log(LOG_DEBUG, "kbdc: %d:%d char read (empty_both_buffers)\n", c1, c2);
#endif

    emptyq(&kbdcp(p)->kbd);
    emptyq(&kbdcp(p)->aux);
}

/* keyboard and mouse device control */

/* NOTE: enable the keyboard port but disable the keyboard 
 * interrupt before calling "reset_kbd()".
 */
int
reset_kbd(KBDC p)
{
    int retry = KBD_MAXRETRY;
    int again = KBD_MAXWAIT;
    int c = KBD_RESEND;		/* keep the compiler happy */

    while (retry-- > 0) {
        empty_both_buffers(p, 10);
        if (!write_kbd_command(p, KBDC_RESET_KBD))
	    continue;
	emptyq(&kbdcp(p)->kbd);
        c = read_controller_data(p);
	if (verbose || bootverbose)
            log(LOG_DEBUG, "kbdc: RESET_KBD return code:%04x\n", c);
        if (c == KBD_ACK)	/* keyboard has agreed to reset itself... */
    	    break;
    }
    if (retry < 0)
        return FALSE;

    while (again-- > 0) {
        /* wait awhile, well, in fact we must wait quite loooooooooooong */
        DELAY(KBD_RESETDELAY*1000);
        c = read_controller_data(p);	/* RESET_DONE/RESET_FAIL */
        if (c != -1) 	/* wait again if the controller is not ready */
    	    break;
    }
    if (verbose || bootverbose)
        log(LOG_DEBUG, "kbdc: RESET_KBD status:%04x\n", c);
    if (c != KBD_RESET_DONE)
        return FALSE;
    return TRUE;
}

/* NOTE: enable the aux port but disable the aux interrupt
 * before calling `reset_aux_dev()'.
 */
int
reset_aux_dev(KBDC p)
{
    int retry = KBD_MAXRETRY;
    int again = KBD_MAXWAIT;
    int c = PSM_RESEND;		/* keep the compiler happy */

    while (retry-- > 0) {
        empty_both_buffers(p, 10);
        if (!write_aux_command(p, PSMC_RESET_DEV))
	    continue;
	emptyq(&kbdcp(p)->aux);
	/* NOTE: Compaq Armada laptops require extra delay here. XXX */
	for (again = KBD_MAXWAIT; again > 0; --again) {
            DELAY(KBD_RESETDELAY*1000);
            c = read_aux_data_no_wait(p);
	    if (c != -1)
		break;
	}
        if (verbose || bootverbose)
            log(LOG_DEBUG, "kbdc: RESET_AUX return code:%04x\n", c);
        if (c == PSM_ACK)	/* aux dev is about to reset... */
    	    break;
    }
    if (retry < 0)
        return FALSE;

    for (again = KBD_MAXWAIT; again > 0; --again) {
        /* wait awhile, well, quite looooooooooooong */
        DELAY(KBD_RESETDELAY*1000);
        c = read_aux_data_no_wait(p);	/* RESET_DONE/RESET_FAIL */
        if (c != -1) 	/* wait again if the controller is not ready */
    	    break;
    }
    if (verbose || bootverbose)
        log(LOG_DEBUG, "kbdc: RESET_AUX status:%04x\n", c);
    if (c != PSM_RESET_DONE)	/* reset status */
        return FALSE;

    c = read_aux_data(p);	/* device ID */
    if (verbose || bootverbose)
        log(LOG_DEBUG, "kbdc: RESET_AUX ID:%04x\n", c);
    /* NOTE: we could check the device ID now, but leave it later... */
    return TRUE;
}

/* controller diagnostics and setup */

int
test_controller(KBDC p)
{
    int retry = KBD_MAXRETRY;
    int again = KBD_MAXWAIT;
    int c = KBD_DIAG_FAIL;

    while (retry-- > 0) {
        empty_both_buffers(p, 10);
        if (write_controller_command(p, KBDC_DIAGNOSE))
    	    break;
    }
    if (retry < 0)
        return FALSE;

    emptyq(&kbdcp(p)->kbd);
    while (again-- > 0) {
        /* wait awhile */
        DELAY(KBD_RESETDELAY*1000);
        c = read_controller_data(p);	/* DIAG_DONE/DIAG_FAIL */
        if (c != -1) 	/* wait again if the controller is not ready */
    	    break;
    }
    if (verbose || bootverbose)
        log(LOG_DEBUG, "kbdc: DIAGNOSE status:%04x\n", c);
    return (c == KBD_DIAG_DONE);
}

int
test_kbd_port(KBDC p)
{
    int retry = KBD_MAXRETRY;
    int again = KBD_MAXWAIT;
    int c = -1;

    while (retry-- > 0) {
        empty_both_buffers(p, 10);
        if (write_controller_command(p, KBDC_TEST_KBD_PORT))
    	    break;
    }
    if (retry < 0)
        return FALSE;

    emptyq(&kbdcp(p)->kbd);
    while (again-- > 0) {
        c = read_controller_data(p);
        if (c != -1) 	/* try again if the controller is not ready */
    	    break;
    }
    if (verbose || bootverbose)
        log(LOG_DEBUG, "kbdc: TEST_KBD_PORT status:%04x\n", c);
    return c;
}

int
test_aux_port(KBDC p)
{
    int retry = KBD_MAXRETRY;
    int again = KBD_MAXWAIT;
    int c = -1;

    while (retry-- > 0) {
        empty_both_buffers(p, 10);
        if (write_controller_command(p, KBDC_TEST_AUX_PORT))
    	    break;
    }
    if (retry < 0)
        return FALSE;

    emptyq(&kbdcp(p)->kbd);
    while (again-- > 0) {
        c = read_controller_data(p);
        if (c != -1) 	/* try again if the controller is not ready */
    	    break;
    }
    if (verbose || bootverbose)
        log(LOG_DEBUG, "kbdc: TEST_AUX_PORT status:%04x\n", c);
    return c;
}

int
kbdc_get_device_mask(KBDC p)
{
    return kbdcp(p)->command_mask;
}

void
kbdc_set_device_mask(KBDC p, int mask)
{
    kbdcp(p)->command_mask = 
	mask & (KBD_KBD_CONTROL_BITS | KBD_AUX_CONTROL_BITS);
}

int
get_controller_command_byte(KBDC p)
{
    if (kbdcp(p)->command_byte != -1)
	return kbdcp(p)->command_byte;
    if (!write_controller_command(p, KBDC_GET_COMMAND_BYTE))
	return -1;
    emptyq(&kbdcp(p)->kbd);
    kbdcp(p)->command_byte = read_controller_data(p);
    return kbdcp(p)->command_byte;
}

int
set_controller_command_byte(KBDC p, int mask, int command)
{
    if (get_controller_command_byte(p) == -1)
	return FALSE;

    command = (kbdcp(p)->command_byte & ~mask) | (command & mask);
    if (command & KBD_DISABLE_KBD_PORT) {
	if (!write_controller_command(p, KBDC_DISABLE_KBD_PORT))
	    return FALSE;
    }
    if (!write_controller_command(p, KBDC_SET_COMMAND_BYTE))
	return FALSE;
    if (!write_controller_data(p, command))
	return FALSE;
    kbdcp(p)->command_byte = command;

    if (verbose)
        log(LOG_DEBUG, "kbdc: new command byte:%04x (set_controller...)\n",
	    command);

    return TRUE;
}
