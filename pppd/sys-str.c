/*
 * sys-str.c - System-dependent procedures for setting up
 * PPP interfaces on systems which use the STREAMS ppp interface.
 *
 * Copyright (c) 1989 Carnegie Mellon University.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by Carnegie Mellon University.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef lint
static char rcsid[] = "$Id: sys-str.c,v 1.14 1994/09/21 06:47:37 paulus Exp $";
#endif

/*
 * TODO:
 */

#include <stdio.h>
#include <errno.h>
#include <syslog.h>
#include <termios.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <utmp.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <sys/stream.h>
#include <sys/stropts.h>

#include <net/if.h>
#include <net/ppp_defs.h>
#include <net/ppp_str.h>
#include <net/route.h>
#include <net/if_arp.h>
#include <netinet/in.h>

#include "pppd.h"

#ifndef ifr_mtu
#define ifr_mtu		ifr_metric
#endif

#define	MAXMODULES	10	/* max number of module names to save */
static struct	modlist {
    char	modname[FMNAMESZ+1];
} str_modules[MAXMODULES];
static int	str_module_count = 0;
static int	pushed_ppp;
static int	closed_stdio;

static int	restore_term;	/* 1 => we've munged the terminal */
static struct termios inittermios; /* Initial TTY termios */

/*
 * sys_init - System-dependent initialization.
 */
void
sys_init()
{
    openlog("pppd", LOG_PID | LOG_NDELAY, LOG_PPP);
    setlogmask(LOG_UPTO(LOG_INFO));
    if (debug)
	setlogmask(LOG_UPTO(LOG_DEBUG));
}

/*
 * note_debug_level - note a change in the debug level.
 */
void
note_debug_level()
{
    if (debug) {
	syslog(LOG_INFO, "Debug turned ON, Level %d", debug);
	setlogmask(LOG_UPTO(LOG_DEBUG));
    } else {
	setlogmask(LOG_UPTO(LOG_WARNING));
    }
}

/*
 * daemon - Detach us from the terminal session.
 */
int
daemon(nochdir, noclose)
    int nochdir, noclose;
{
    int pid;

    if ((pid = fork()) < 0)
	return -1;
    if (pid != 0)
	exit(0);		/* parent dies */
    setsid();
    if (!nochdir)
	chdir("/");
    if (!noclose) {
	fclose(stdin);		/* don't need stdin, stdout, stderr */
	fclose(stdout);
	fclose(stderr);
    }
    return 0;
}


/*
 * ppp_available - check if this kernel supports PPP.
 */
int
ppp_available()
{
    int fd, ret;

    fd = open("/dev/tty", O_RDONLY, 0);
    if (fd < 0)
	return 1;		/* can't find out - assume we have ppp */
    ret = ioctl(fd, I_FIND, "pppasync") >= 0;
    close(fd);
    return ret;
}


/*
 * establish_ppp - Turn the serial port into a ppp interface.
 */
void
establish_ppp()
{
    /* go through and save the name of all the modules, then pop em */
    for (;;) { 
	if (ioctl(fd, I_LOOK, str_modules[str_module_count].modname) < 0 ||
	    ioctl(fd, I_POP, 0) < 0)
	    break;
	MAINDEBUG((LOG_DEBUG, "popped stream module : %s",
		   str_modules[str_module_count].modname));
	str_module_count++;
    }

    /* now push the async/fcs module */
    if (ioctl(fd, I_PUSH, "pppasync") < 0) {
	syslog(LOG_ERR, "ioctl(I_PUSH, ppp_async): %m");
	die(1);
    }
    /* push the compress module */
    if (ioctl(fd, I_PUSH, "pppcomp") < 0) {
	syslog(LOG_WARNING, "ioctl(I_PUSH, ppp_comp): %m");
    }
    /* finally, push the ppp_if module that actually handles the */
    /* network interface */ 
    if (ioctl(fd, I_PUSH, "pppif") < 0) {
	syslog(LOG_ERR, "ioctl(I_PUSH, ppp_if): %m");
	die(1);
    }
    pushed_ppp = 1;
    /* read mode, message non-discard mode */
    if (ioctl(fd, I_SRDOPT, RMSGN) < 0) {
	syslog(LOG_ERR, "ioctl(I_SRDOPT, RMSGN): %m");
	die(1);
    }
    /*
     * Find out which interface we were given.
     * (ppp_if handles this ioctl)
     */
    if (ioctl(fd, SIOCGETU, &ifunit) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCGETU): %m");
	die(1);
    }

    /* Set debug flags in driver */
    if (ioctl(fd, SIOCSIFDEBUG, &kdebugflag) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFDEBUG): %m");
    }

    /* close stdin, stdout, stderr if they might refer to the device */
    if (default_device && !closed_stdio) {
	int i;

	for (i = 0; i <= 2; ++i)
	    if (i != fd && i != s)
		close(i);
	closed_stdio = 1;
    }
}

/*
 * disestablish_ppp - Restore the serial port to normal operation.
 * It attempts to reconstruct the stream with the previously popped
 * modules.  This shouldn't call die() because it's called from die().
 */
void
disestablish_ppp()
{
    int flags;
    char *s;

    if (hungup) {
	/* we can't push or pop modules after the stream has hung up */
	str_module_count = 0;
	restore_term = 0;	/* nor can we fix up terminal settings */
	return;
    }

    if (pushed_ppp) {
	/*
	 * Check whether the link seems not to be 8-bit clean.
	 */
	if (ioctl(fd, SIOCGIFDEBUG, (caddr_t) &flags) == 0) {
	    s = NULL;
	    switch (~flags & PAI_FLAGS_HIBITS) {
	    case PAI_FLAGS_B7_0:
		s = "bit 7 set to 1";
		break;
	    case PAI_FLAGS_B7_1:
		s = "bit 7 set to 0";
		break;
	    case PAI_FLAGS_PAR_EVEN:
		s = "odd parity";
		break;
	    case PAI_FLAGS_PAR_ODD:
		s = "even parity";
		break;
	    }
	    if (s != NULL) {
		syslog(LOG_WARNING, "Serial link is not 8-bit clean:");
		syslog(LOG_WARNING, "All received characters had %s", s);
	    }
	}
    }

    while (ioctl(fd, I_POP, 0) == 0)	/* pop any we pushed */
	;
    pushed_ppp = 0;
  
    for (; str_module_count > 0; str_module_count--) {
	if (ioctl(fd, I_PUSH, str_modules[str_module_count-1].modname)) {
	    if (errno != ENXIO)
		syslog(LOG_WARNING, "str_restore: couldn't push module %s: %m",
		       str_modules[str_module_count-1].modname);
	} else {
	    MAINDEBUG((LOG_INFO, "str_restore: pushed module %s",
		       str_modules[str_module_count-1].modname));
	}
    }
}


/*
 * List of valid speeds.
 */
struct speed {
    int speed_int, speed_val;
} speeds[] = {
#ifdef B50
    { 50, B50 },
#endif
#ifdef B75
    { 75, B75 },
#endif
#ifdef B110
    { 110, B110 },
#endif
#ifdef B134
    { 134, B134 },
#endif
#ifdef B150
    { 150, B150 },
#endif
#ifdef B200
    { 200, B200 },
#endif
#ifdef B300
    { 300, B300 },
#endif
#ifdef B600
    { 600, B600 },
#endif
#ifdef B1200
    { 1200, B1200 },
#endif
#ifdef B1800
    { 1800, B1800 },
#endif
#ifdef B2000
    { 2000, B2000 },
#endif
#ifdef B2400
    { 2400, B2400 },
#endif
#ifdef B3600
    { 3600, B3600 },
#endif
#ifdef B4800
    { 4800, B4800 },
#endif
#ifdef B7200
    { 7200, B7200 },
#endif
#ifdef B9600
    { 9600, B9600 },
#endif
#ifdef B19200
    { 19200, B19200 },
#endif
#ifdef B38400
    { 38400, B38400 },
#endif
#ifdef EXTA
    { 19200, EXTA },
#endif
#ifdef EXTB
    { 38400, EXTB },
#endif
#ifdef B57600
    { 57600, B57600 },
#endif
#ifdef B115200
    { 115200, B115200 },
#endif
    { 0, 0 }
};

/*
 * Translate from bits/second to a speed_t.
 */
int
translate_speed(bps)
    int bps;
{
    struct speed *speedp;

    if (bps == 0)
	return 0;
    for (speedp = speeds; speedp->speed_int; speedp++)
	if (bps == speedp->speed_int)
	    return speedp->speed_val;
    syslog(LOG_WARNING, "speed %d not supported", bps);
    return 0;
}

/*
 * Translate from a speed_t to bits/second.
 */
int
baud_rate_of(speed)
    int speed;
{
    struct speed *speedp;

    if (speed == 0)
	return 0;
    for (speedp = speeds; speedp->speed_int; speedp++)
	if (speed == speedp->speed_val)
	    return speedp->speed_int;
    return 0;
}

/*
 * set_up_tty: Set up the serial port on `fd' for 8 bits, no parity,
 * at the requested speed, etc.  If `local' is true, set CLOCAL
 * regardless of whether the modem option was specified.
 */
set_up_tty(fd, local)
    int fd, local;
{
    int speed;
    struct termios tios;

    if (tcgetattr(fd, &tios) < 0) {
	syslog(LOG_ERR, "tcgetattr: %m");
	die(1);
    }

    if (!restore_term)
	inittermios = tios;

    tios.c_cflag &= ~(CSIZE | CSTOPB | PARENB | CLOCAL);
    if (crtscts > 0)
	tios.c_cflag |= CRTSCTS;
    else if (crtscts < 0)
	tios.c_cflag &= ~CRTSCTS;

    tios.c_cflag |= CS8 | CREAD | HUPCL;
    if (local || !modem)
	tios.c_cflag |= CLOCAL;
    tios.c_iflag = IGNBRK | IGNPAR;
    tios.c_oflag = 0;
    tios.c_lflag = 0;
    tios.c_cc[VMIN] = 1;
    tios.c_cc[VTIME] = 0;

    if (crtscts == 2) {
	tios.c_iflag |= IXOFF;
	tios.c_cc[VSTOP] = 0x13;	/* DC3 = XOFF = ^S */
	tios.c_cc[VSTART] = 0x11;	/* DC1 = XON  = ^Q */
    }

    speed = translate_speed(inspeed);
    if (speed) {
	cfsetospeed(&tios, speed);
	cfsetispeed(&tios, speed);
    } else {
	speed = cfgetospeed(&tios);
	/*
	 * We can't proceed if the serial port speed is B0,
	 * since that implies that the serial port is disabled.
	 */
	if (speed == B0) {
	    syslog(LOG_ERR, "Baud rate for %s is 0; need explicit baud rate",
		   devnam);
	    die(1);
	}
    }

    if (tcsetattr(fd, TCSAFLUSH, &tios) < 0) {
	syslog(LOG_ERR, "tcsetattr: %m");
	die(1);
    }

    baud_rate = inspeed = baud_rate_of(speed);
    restore_term = 1;
}

/*
 * restore_tty - restore the terminal to the saved settings.
 */
void
restore_tty()
{
    if (restore_term) {
	if (tcsetattr(fd, TCSAFLUSH, &inittermios) < 0)
	    if (errno != ENXIO)
		syslog(LOG_WARNING, "tcsetattr: %m");
	restore_term = 0;
    }
}

/*
 * setdtr - control the DTR line on the serial port.
 * This is called from die(), so it shouldn't call die().
 */
setdtr(fd, on)
int fd, on;
{
    int modembits = TIOCM_DTR;

    ioctl(fd, (on? TIOCMBIS: TIOCMBIC), &modembits);
}


/*
 * output - Output PPP packet.
 */
void
output(unit, p, len)
    int unit;
    u_char *p;
    int len;
{
    struct strbuf	str;

    if (unit != 0)
	MAINDEBUG((LOG_WARNING, "output: unit != 0!"));
    if (debug)
	log_packet(p, len, "sent ");

    str.len = len;
    str.buf = (caddr_t) p;
    if (putmsg(fd, NULL, &str, 0) < 0) {
	if (errno != ENXIO) {
	    syslog(LOG_ERR, "putmsg: %m");
	    die(1);
	}
    }
}

/*
 * wait_input - wait for input, for a length of time specified in *timo.
 */
wait_input(timo)
    struct timeval *timo;
{
    int t;
    struct pollfd pfd;

    t = timo == NULL? -1: timo->tv_sec * 1000 + timo->tv_usec / 1000;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLPRI | POLLHUP;
    if (poll(&pfd, 1, t) < 0 && errno != EINTR) {
	syslog(LOG_ERR, "poll: %m");
	die(1);
    }
}

/*
 * read_packet - get a PPP packet from the serial device.
 */
int
read_packet(buf)
    u_char *buf;
{
    struct strbuf str, ctl;
    int len, i;
    unsigned char ctlbuf[16];

    str.maxlen = PPP_MTU + PPP_HDRLEN;
    str.buf = (caddr_t) buf;
    ctl.maxlen = sizeof(ctlbuf);
    ctl.buf = (caddr_t) ctlbuf;
    i = 0;
    len = getmsg(fd, &ctl, &str, &i);
    if (len < 0) {
	if (errno == EAGAIN || errno == EWOULDBLOCK) {
	    return -1;
	}
	syslog(LOG_ERR, "getmsg(fd) %m");
	die(1);
    }
    if (len) 
	MAINDEBUG((LOG_DEBUG, "getmsg returned 0x%x",len));
    if (ctl.len > 0)
	syslog(LOG_NOTICE, "got ctrl msg len %d %x %x\n", ctl.len,
	       ctlbuf[0], ctlbuf[1]);

    if (str.len < 0) {
	MAINDEBUG((LOG_DEBUG, "getmsg short return length %d", str.len));
	return -1;
    }

    return str.len;
}


/*
 * ppp_send_config - configure the transmit characteristics of
 * the ppp interface.
 */
void
ppp_send_config(unit, mtu, asyncmap, pcomp, accomp)
    int unit, mtu;
    u_int32_t asyncmap;
    int pcomp, accomp;
{
    char c;
    struct ifreq ifr;

    strncpy(ifr.ifr_name, ifname, sizeof (ifr.ifr_name));
    ifr.ifr_mtu = mtu;
    if (ioctl(s, SIOCSIFMTU, (caddr_t) &ifr) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFMTU): %m");
	quit();
    }

    if(ioctl(fd, SIOCSIFASYNCMAP, (caddr_t) &asyncmap) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFASYNCMAP): %m");
	quit();
    }

    c = (pcomp? 1: 0);
    if(ioctl(fd, SIOCSIFCOMPPROT, &c) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFCOMPPROT): %m");
	quit();
    }

    c = (accomp? 1: 0);
    if(ioctl(fd, SIOCSIFCOMPAC, &c) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFCOMPAC): %m");
	quit();
    }
}


/*
 * ppp_set_xaccm - set the extended transmit ACCM for the interface.
 */
void
ppp_set_xaccm(unit, accm)
    int unit;
    ext_accm accm;
{
    if (ioctl(fd, SIOCSIFXASYNCMAP, accm) < 0 && errno != ENOTTY)
	syslog(LOG_WARNING, "ioctl(set extended ACCM): %m");
}


/*
 * ppp_recv_config - configure the receive-side characteristics of
 * the ppp interface.
 */
void
ppp_recv_config(unit, mru, asyncmap, pcomp, accomp)
    int unit, mru;
    u_int32_t asyncmap;
    int pcomp, accomp;
{
    char c;

    if (ioctl(fd, SIOCSIFMRU, &mru) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFMRU): %m");
    }

    if (ioctl(fd, SIOCSIFRASYNCMAP, (caddr_t) &asyncmap) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFRASYNCMAP): %m");
    }

    c = 2 + (pcomp? 1: 0);
    if(ioctl(fd, SIOCSIFCOMPPROT, &c) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFCOMPPROT): %m");
    }

    c = 2 + (accomp? 1: 0);
    if (ioctl(fd, SIOCSIFCOMPAC, &c) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFCOMPAC): %m");
    }
}

/*
 * ccp_test - ask kernel whether a given compression method
 * is acceptable for use.
 */
ccp_test(unit, opt_ptr, opt_len, for_transmit)
    int unit, opt_len, for_transmit;
    u_char *opt_ptr;
{
    struct ppp_option_data data;

    if ((unsigned) opt_len > MAX_PPP_OPTION)
	opt_len = MAX_PPP_OPTION;
    data.length = opt_len;
    data.transmit = for_transmit;
    BCOPY(opt_ptr, data.opt_data, opt_len);
    return ioctl(fd, SIOCSCOMPRESS, (caddr_t) &data) >= 0;
}

/*
 * ccp_flags_set - inform kernel about the current state of CCP.
 */
void
ccp_flags_set(unit, isopen, isup)
    int unit, isopen, isup;
{
    int x;

    x = (isopen? 1: 0) + (isup? 2: 0);
    if (ioctl(fd, SIOCSIFCOMP, (caddr_t) &x) < 0 && errno != ENOTTY)
	syslog(LOG_ERR, "ioctl (SIOCSIFCOMP): %m");
}

/*
 * ccp_fatal_error - returns 1 if decompression was disabled as a
 * result of an error detected after decompression of a packet,
 * 0 otherwise.  This is necessary because of patent nonsense.
 */
int
ccp_fatal_error(unit)
    int unit;
{
    int x;

    if (ioctl(fd, SIOCGIFCOMP, (caddr_t) &x) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCGIFCOMP): %m");
	return 0;
    }
    return x & CCP_FATALERROR;
}

/*
 * sifvjcomp - config tcp header compression
 */
int
sifvjcomp(u, vjcomp, cidcomp, maxcid)
    int u, vjcomp, cidcomp, maxcid;
{
    char x;

    x = (vjcomp? 1: 0) + (cidcomp? 0: 2) + (maxcid << 4);
    if (ioctl(fd, SIOCSIFVJCOMP, (caddr_t) &x) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFVJCOMP): %m");
	return 0;
    }
    return 1;
}

/*
 * sifup - Config the interface up.
 */
int
sifup(u)
    int u;
{
    struct ifreq ifr;

    strncpy(ifr.ifr_name, ifname, sizeof (ifr.ifr_name));
    if (ioctl(s, SIOCGIFFLAGS, (caddr_t) &ifr) < 0) {
	syslog(LOG_ERR, "ioctl (SIOCGIFFLAGS): %m");
	return 0;
    }
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(s, SIOCSIFFLAGS, (caddr_t) &ifr) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFFLAGS): %m");
	return 0;
    }
    return 1;
}

/*
 * sifdown - Config the interface down.
 */
int
sifdown(u)
    int u;
{
    struct ifreq ifr;
    strncpy(ifr.ifr_name, ifname, sizeof (ifr.ifr_name));
    if (ioctl(s, SIOCGIFFLAGS, (caddr_t) &ifr) < 0) {
	syslog(LOG_ERR, "ioctl (SIOCGIFFLAGS): %m");
	return 0;
    }
    ifr.ifr_flags &= ~IFF_UP;
    if (ioctl(s, SIOCSIFFLAGS, (caddr_t) &ifr) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFFLAGS): %m");
	return 0;
    }
    return 1;
}

/*
 * SET_SA_FAMILY - initialize a struct sockaddr, setting the sa_family field.
 */
#define SET_SA_FAMILY(addr, family)		\
    BZERO((char *) &(addr), sizeof(addr));	\
    addr.sa_family = (family);

/*
 * sifaddr - Config the interface IP addresses and netmask.
 */
int
sifaddr(u, o, h, m)
    int u;
    u_int32_t o, h, m;
{
    int ret;
    struct ifreq ifr;

    ret = 1;
    strncpy(ifr.ifr_name, ifname, sizeof (ifr.ifr_name));
    SET_SA_FAMILY(ifr.ifr_addr, AF_INET);
    ((struct sockaddr_in *) &ifr.ifr_addr)->sin_addr.s_addr = o;
    if (ioctl(s, SIOCSIFADDR, (caddr_t) &ifr) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFADDR): %m");
	ret = 0;
    }
    ((struct sockaddr_in *) &ifr.ifr_dstaddr)->sin_addr.s_addr = h;
    if (ioctl(s, SIOCSIFDSTADDR, (caddr_t) &ifr) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSIFDSTADDR): %m");
	ret = 0;
    }
    if (m != 0) {
	((struct sockaddr_in *) &ifr.ifr_addr)->sin_addr.s_addr = m;
	syslog(LOG_INFO, "Setting interface mask to %s\n", ip_ntoa(m));
	if (ioctl(s, SIOCSIFNETMASK, (caddr_t) &ifr) < 0) {
	    syslog(LOG_ERR, "ioctl(SIOCSIFNETMASK): %m");
	    ret = 0;
	}
    }
    return ret;
}

/*
 * cifaddr - Clear the interface IP addresses, and delete routes
 * through the interface if possible.
 */
int
cifaddr(u, o, h)
    int u;
    u_int32_t o, h;
{
    struct rtentry rt;

    SET_SA_FAMILY(rt.rt_dst, AF_INET);
    ((struct sockaddr_in *) &rt.rt_dst)->sin_addr.s_addr = h;
    SET_SA_FAMILY(rt.rt_gateway, AF_INET);
    ((struct sockaddr_in *) &rt.rt_gateway)->sin_addr.s_addr = o;
    rt.rt_flags = RTF_HOST;
    if (ioctl(s, SIOCDELRT, (caddr_t) &rt) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCDELRT): %m");
	return 0;
    }
    return 1;
}

/*
 * sifdefaultroute - assign a default route through the address given.
 */
int
sifdefaultroute(u, g)
    int u;
    u_int32_t g;
{
    struct rtentry rt;

    SET_SA_FAMILY(rt.rt_dst, AF_INET);
    SET_SA_FAMILY(rt.rt_gateway, AF_INET);
    ((struct sockaddr_in *) &rt.rt_gateway)->sin_addr.s_addr = g;
    rt.rt_flags = RTF_GATEWAY;
    if (ioctl(s, SIOCADDRT, &rt) < 0) {
	syslog(LOG_ERR, "default route ioctl(SIOCADDRT): %m");
	return 0;
    }
    return 1;
}

/*
 * cifdefaultroute - delete a default route through the address given.
 */
int
cifdefaultroute(u, g)
    int u;
    u_int32_t g;
{
    struct rtentry rt;

    SET_SA_FAMILY(rt.rt_dst, AF_INET);
    SET_SA_FAMILY(rt.rt_gateway, AF_INET);
    ((struct sockaddr_in *) &rt.rt_gateway)->sin_addr.s_addr = g;
    rt.rt_flags = RTF_GATEWAY;
    if (ioctl(s, SIOCDELRT, &rt) < 0) {
	syslog(LOG_ERR, "default route ioctl(SIOCDELRT): %m");
	return 0;
    }
    return 1;
}

/*
 * sifproxyarp - Make a proxy ARP entry for the peer.
 */
int
sifproxyarp(unit, hisaddr)
    int unit;
    u_int32_t hisaddr;
{
    struct arpreq arpreq;

    BZERO(&arpreq, sizeof(arpreq));

    /*
     * Get the hardware address of an interface on the same subnet
     * as our local address.
     */
    if (!get_ether_addr(hisaddr, &arpreq.arp_ha)) {
	syslog(LOG_WARNING, "Cannot determine ethernet address for proxy ARP");
	return 0;
    }

    SET_SA_FAMILY(arpreq.arp_pa, AF_INET);
    ((struct sockaddr_in *) &arpreq.arp_pa)->sin_addr.s_addr = hisaddr;
    arpreq.arp_flags = ATF_PERM | ATF_PUBL;
    if (ioctl(s, SIOCSARP, (caddr_t)&arpreq) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCSARP): %m");
	return 0;
    }

    return 1;
}

/*
 * cifproxyarp - Delete the proxy ARP entry for the peer.
 */
int
cifproxyarp(unit, hisaddr)
    int unit;
    u_int32_t hisaddr;
{
    struct arpreq arpreq;

    BZERO(&arpreq, sizeof(arpreq));
    SET_SA_FAMILY(arpreq.arp_pa, AF_INET);
    ((struct sockaddr_in *) &arpreq.arp_pa)->sin_addr.s_addr = hisaddr;
    if (ioctl(s, SIOCDARP, (caddr_t)&arpreq) < 0) {
	syslog(LOG_ERR, "ioctl(SIOCDARP): %m");
	return 0;
    }
    return 1;
}

/*
 * get_ether_addr - get the hardware address of an interface on the
 * the same subnet as ipaddr.  Code borrowed from myetheraddr.c
 * in the cslip-2.6 distribution, which is subject to the following
 * copyright notice (which also applies to logwtmp below):
 *
 * Copyright (c) 1990, 1992 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that: (1) source code distributions
 * retain the above copyright notice and this paragraph in its entirety, (2)
 * distributions including binary code include the above copyright notice and
 * this paragraph in its entirety in the documentation or other materials
 * provided with the distribution, and (3) all advertising materials mentioning
 * features or use of this software display the following acknowledgement:
 * ``This product includes software developed by the University of California,
 * Lawrence Berkeley Laboratory and its contributors.'' Neither the name of
 * the University nor the names of its contributors may be used to endorse
 * or promote products derived from this software without specific prior
 * written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <fcntl.h>
#include <nlist.h>
#include <kvm.h>
#include <arpa/inet.h>

/* XXX SunOS 4.1 defines this and 3.5 doesn't... */
#ifdef _nlist_h
#define SUNOS4
#endif

#ifdef SUNOS4
#include <netinet/in_var.h>
#endif
#include <netinet/if_ether.h>

/* Cast a struct sockaddr to a structaddr_in */
#define SATOSIN(sa) ((struct sockaddr_in *)(sa))

/* Determine if "bits" is set in "flag" */
#define ALLSET(flag, bits) (((flag) & (bits)) == (bits))

static struct nlist nl[] = {
#define N_IFNET 0
	{ "_ifnet" },
	{ 0 }
};

static void kread();

int
get_ether_addr(ipaddr, hwaddr)
    u_int32_t ipaddr;
    struct sockaddr *hwaddr;
{
    register kvm_t *kd;
    register struct ifnet *ifp;
    register struct arpcom *ac;
    struct arpcom arpcom;
    struct in_addr *inp;
#ifdef SUNOS4
    register struct ifaddr *ifa;
    register struct in_ifaddr *in;
    union {
	struct ifaddr ifa;
	struct in_ifaddr in;
    } ifaddr;
#endif
    u_int32_t addr, mask;

    /* Open kernel memory for reading */
    kd = kvm_open(0, 0, 0, O_RDONLY, NULL);
    if (kd == 0) {
	syslog(LOG_ERR, "kvm_open: %m");
	return 0;
    }

    /* Fetch namelist */
    if (kvm_nlist(kd, nl) != 0) {
	syslog(LOG_ERR, "kvm_nlist failed");
	return 0;
    }

    ac = &arpcom;
    ifp = &arpcom.ac_if;
#ifdef SUNOS4
    ifa = &ifaddr.ifa;
    in = &ifaddr.in;
#endif

    if (kvm_read(kd, nl[N_IFNET].n_value, (char *)&addr, sizeof(addr))
	!= sizeof(addr)) {
	syslog(LOG_ERR, "error reading ifnet addr");
	return 0;
    }
    for ( ; addr; addr = (u_int32_t)ifp->if_next) {
	if (kvm_read(kd, addr, (char *)ac, sizeof(*ac)) != sizeof(*ac)) {
	    syslog(LOG_ERR, "error reading ifnet");
	    return 0;
	}

	/* Only look at configured, broadcast interfaces */
	if (!ALLSET(ifp->if_flags, IFF_UP | IFF_BROADCAST))
	    continue;
#ifdef SUNOS4
	/* This probably can't happen... */
	if (ifp->if_addrlist == 0)
	    continue;
#endif

	/* Get interface ip address */
#ifdef SUNOS4
	if (kvm_read(kd, (u_int32_t)ifp->if_addrlist, (char *)&ifaddr,
		     sizeof(ifaddr)) != sizeof(ifaddr)) {
	    syslog(LOG_ERR, "error reading ifaddr");
	    return 0;
	}
	inp = &SATOSIN(&ifa->ifa_addr)->sin_addr;
#else
	inp = &SATOSIN(&ifp->if_addr)->sin_addr;
#endif

	/* Check if this interface on the right subnet */
#ifdef SUNOS4
	mask = in->ia_subnetmask;
#else
	mask = ifp->if_subnetmask;
#endif
	if ((ipaddr & mask) != (inp->s_addr & mask))
	    continue;

	/* Copy out the local ethernet address */
	hwaddr->sa_family = AF_UNSPEC;
	BCOPY((caddr_t) &arpcom.ac_enaddr, hwaddr->sa_data,
	      sizeof(arpcom.ac_enaddr));
	return 1;		/* success! */
    }

    /* couldn't find one */
    return 0;
}

#define	WTMPFILE	"/usr/adm/wtmp"

int
logwtmp(line, name, host)
    char *line, *name, *host;
{
    int fd;
    struct stat buf;
    struct utmp ut;

    if ((fd = open(WTMPFILE, O_WRONLY|O_APPEND, 0)) < 0)
	return;
    if (!fstat(fd, &buf)) {
	(void)strncpy(ut.ut_line, line, sizeof(ut.ut_line));
	(void)strncpy(ut.ut_name, name, sizeof(ut.ut_name));
	(void)strncpy(ut.ut_host, host, sizeof(ut.ut_host));
	(void)time(&ut.ut_time);
	if (write(fd, (char *)&ut, sizeof(struct utmp)) != sizeof(struct utmp))
	    (void)ftruncate(fd, buf.st_size);
    }
    close(fd);
}
