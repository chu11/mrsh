/*
 * $Id$
 * $Source$
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include "version.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>     /* sockaddr_in, htonl */
#include <arpa/inet.h>
#include <net/if.h>         /* struct ifreq, struct ifconf */

#include <munge.h> 

#include "fd.h"
#include "mauth.h"

#define MAX_MBUF_SIZE     4096

/* Static function prototypes */
static char *munge_parse(struct mauth *, char *, char *);
static int   getifrlen(struct ifreq *);
static int   check_interfaces(struct mauth *, void *, int h_length);
static int   check_munge_ip(struct mauth *, char *);

char *munge_parse(struct mauth *ma, char *buf, char *end) {
    int len = strlen(buf);

    buf += len + 1;
    if (buf >= end) {
        syslog(LOG_ERR, "parser went beyond valid data");
        snprintf(ma->errmsg, MAXERRMSGLEN, "Internal Error");
        return NULL;
    }
    return buf;
}

int getifrlen(struct ifreq *ifr) {
    int len;

    /* Calculations below are necessary b/c socket addresses can have
     * variable length
     */

#if HAVE_SA_LEN
    if (sizeof(struct sockaddr) > ifr->ifr_addr.sa_len)
        len = sizeof(struct sockaddr);
    else
        len = ifr->ifr_addr.sa_len;
#else /* !HAVE_SA_LEN */
    /* For now we only assume AF_INET and AF_INET6 */
    switch(ifr->ifr_addr.sa_family) {
#ifdef HAVE_IPV6
        case AF_INET6:
            len = sizeof(struct sockaddr_in6);
            break;
#endif /* HAVE_IPV6 */
        case AF_INET:
        default:
            len = sizeof(struct sockaddr_in);
            break;
    }
    
    /* On ia32 struct sockaddr_in6/sockaddr_in was the largest
     * structure in struct ifreq, but not on ia64.  This fixes things
     */
    if (len < (sizeof(struct ifreq) - IFNAMSIZ))
        len = sizeof(struct ifreq) - IFNAMSIZ;
#endif /* HAVE_SA_LEN */

    return len;
}

int check_interfaces(struct mauth *ma, void *munge_addr, int addr_len) {
    struct ifconf ifc;
    struct ifreq *ifr;
    int s, found = 0, lastlen = -1;
    int len = sizeof(struct ifreq) * 100;
    void *buf = NULL, *ptr = NULL;
    struct sockaddr_in *sin;
    char *addr;

    /* Significant amounts of this code are from Unix Network
     * Programming, by R. Stevens, Chapter 16
     */

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        syslog(LOG_ERR, "socket call failed: %m");
        snprintf(ma->errmsg, MAXERRMSGLEN, "Internal System Error");
        goto bad;
    }

    /* get all active interfaces */
    while(1) {
        if ((buf = (char *)malloc(len)) == NULL) {
            syslog(LOG_ERR, "malloc failed: %m");
            snprintf(ma->errmsg, MAXERRMSGLEN, "Out of Memory");
            goto bad;
        }

        ifc.ifc_len = len;
        ifc.ifc_buf = buf;

        if (ioctl(s, SIOCGIFCONF, &ifc) < 0) {
            syslog(LOG_ERR, "ioctl SIOCGIFCONF failed: %m");
            snprintf(ma->errmsg, MAXERRMSGLEN, "Internal System Error");
            goto bad;
        }
        else {
            if (ifc.ifc_len == lastlen)
                break;
            lastlen = ifc.ifc_len;
        }

        /* Run ioctl() twice for portability reasons.  See Unix Network
         * Programming, section 16.6
         */

        len += 10 * sizeof(struct ifreq);
        free(buf);
    }
    
    /* get IP addresses for all interfaces */
    for (ptr = buf; ptr < buf + ifc.ifc_len; ) {

        ifr = (struct ifreq *)ptr;

        len = getifrlen(ifr);

        ptr += sizeof(ifr->ifr_name) + len;

        /* Currently, we only care about IPv4 (i.e. AF_INET) */
        if (ifr->ifr_addr.sa_family != AF_INET)
            continue;

        sin = (struct sockaddr_in *)&ifr->ifr_addr;

        /* Skip 127.0.0.1 */
        addr = inet_ntoa(sin->sin_addr);
        if (strcmp(addr,"127.0.0.1") == 0)
            continue;

        if (memcmp(munge_addr, (void *)&sin->sin_addr.s_addr, addr_len) == 0) {
            found++;
            break;
        }
    }

    free(buf);
    return found;

 bad:
    free(buf);
    return -1;
}

int check_munge_ip(struct mauth *ma, char *ip) {
    struct in_addr in;
 
    strncpy(ma->ip, ip, INET_ADDRSTRLEN);

    if (inet_pton(AF_INET, &(ma->ip[0]), &in) <= 0) {
        syslog(LOG_ERR, "failed inet_pton: %m");
        snprintf(ma->errmsg, MAXERRMSGLEN, "Internal System Error");
        return -1;
    }

    return check_interfaces(ma, &in, sizeof(struct in_addr));
} 

static void _copy_passwd_struct(struct passwd *to, struct passwd *from) {
    to->pw_uid    = from->pw_uid;  
    to->pw_gid    = from->pw_gid;
    to->pw_name   = strdup(from->pw_name);
    to->pw_passwd = strdup(from->pw_passwd);
    to->pw_gecos  = strdup(from->pw_gecos);
    to->pw_dir    = strdup(from->pw_dir);
    to->pw_shell  = strdup(from->pw_shell);

    return;
}

int mauth(struct mauth *ma, int fd, int cport) {
    int rv, buf_length;
    char mbuf[MAX_MBUF_SIZE];
    char *mptr = NULL;
    char *m_head = NULL;
    char *m_end = NULL;

    if (ma == NULL)
        return -1;

    memset(&mbuf[0], '\0', MAX_MBUF_SIZE);
    if ((buf_length = fd_null_read_n(fd, &mbuf[0], MAX_MBUF_SIZE)) < 0) {
        syslog(LOG_ERR, "%s: %m", "bad read error.");
        snprintf(ma->errmsg, MAXERRMSGLEN, "Internal System Error");
        return -1;
    }

    if (buf_length == 0) {
        syslog(LOG_ERR, "%s", "null munge credential.");
        snprintf(ma->errmsg, MAXERRMSGLEN, "Protocol Error");
        return -1;
    }

    /*
     * The format of our munge buffer is as follows (each a string terminated
     * with a '\0' (null):
     *
     * No stderr wanted if stderr_port_number & random number are 0
     *
     *                                         SIZE            EXAMPLE
     *                                         ==========      =============
     * remote_user_name                        variable        "mhaskell"
     * '\0'
     * version number                          < 12 bytes      "1.2"
     * '\0'
     * dotted_decimal_address_of_this_server   7-15 bytes      "134.9.11.155"
     * '\0'
     * stderr_port_number                      4-8 bytes       "50111"
     * '\0'
     * random_number                           1-8 bytes       "1f79ca0e"
     * '\0'
     * users_command                           variable        "ls -al"
     * '\0' '\0'
     *
     */
  
    mptr = &mbuf[0];
    if ((rv = munge_decode(mbuf, 0, (void **)&mptr, &buf_length, 
                           &ma->uid, &ma->gid)) != EMUNGE_SUCCESS) {
        syslog(LOG_ERR, "%s: %s", "munge_decode error", munge_strerror(rv));
        snprintf(ma->errmsg, MAXERRMSGLEN, "Authentication Failure: %s",
                 munge_strerror(rv));
        return -1;
    }
  
    if ((mptr == NULL) || (buf_length <= 0)) {
        syslog(LOG_ERR, "Null munge buffer");
        snprintf(ma->errmsg, MAXERRMSGLEN, "Protocol Error");
        return -1;
    }

    m_head = mptr;
    m_end = mptr + buf_length;
  
    /* Verify User Id */

    strncpy(ma->username, m_head, MAXUSERNAMELEN);
    if ((ma->pwd = getpwnam(ma->username)) == NULL) {
        syslog(LOG_ERR, "bad getpwnam(): %m"); 
        snprintf(ma->errmsg, MAXERRMSGLEN, "Permission Denied");
        goto bad;
    }

    /*  Copy struct passwd from this machine into local password
     *  structure, and point "pwd" to it
     */
    _copy_passwd_struct(&(ma->cred), ma->pwd);
    ma->pwd = &(ma->cred);

    if (ma->pwd->pw_uid != ma->uid) {
        if (ma->uid != 0) {
            syslog(LOG_ERR, "failed credential check: %m");
            snprintf(ma->errmsg, MAXERRMSGLEN, "Permission Denied");
            goto bad;
        }
    }

    /* Verify version number */

    if ((m_head = munge_parse(ma, m_head, m_end)) == NULL)
        goto bad;
    
    strncpy(ma->version, m_head, MAXVERSIONLEN);
  
    if (strcmp(ma->version, MRSH_PROTOCOL_VERSION) != 0) {
        syslog(LOG_ERR, 
               "Client protocol version (%s) does not match server version (%s)",
               ma->version, MRSH_PROTOCOL_VERSION);
        snprintf(ma->errmsg, MAXERRMSGLEN, 
                 "Client protocol version (%s) does not match server version (%s)",
                 ma->version, MRSH_PROTOCOL_VERSION);
        goto bad;
    }

    /* Verify IP address */
    
    if ((m_head = munge_parse(ma, m_head, m_end)) == NULL)
        goto bad;

    if ((rv = check_munge_ip(ma, m_head)) < 0)
        goto bad;

    if (rv == 0) {
        syslog(LOG_ERR, "%s: %s","Munge IP address doesn't match", m_head);
        snprintf(ma->errmsg, MAXERRMSGLEN, "Permission Denied");
        goto bad;
    }

    /* Verify Port */

    if ((m_head = munge_parse(ma, m_head, m_end)) == NULL)
        goto bad;

    errno = 0;
    ma->port = strtol(m_head, (char **)NULL, 10);
    if (errno != 0) {
        syslog(LOG_ERR, "%s: %s", "Bad port number from client.", m_head);
        snprintf(ma->errmsg, MAXERRMSGLEN, "Internal Error");
        goto bad;
    }

    if (ma->port != cport) {
        syslog(LOG_ERR, "%s: %d, %d", "Port mismatch", cport, ma->port);
        snprintf(ma->errmsg, MAXERRMSGLEN, "Protocol Error");
        return -1;
    }

    /* Get Random Number */
    
    if ((m_head = munge_parse(ma, m_head, m_end)) == NULL)
        goto bad;

    errno = 0;
    ma->rand = strtol(m_head,(char **)NULL,10);
    if (errno != 0) {
        syslog(LOG_ERR, "%s: %d", "Bad random number from client.", ma->rand);
        snprintf(ma->errmsg, MAXERRMSGLEN, "Internal Error");
        goto bad;
    }
  
    if (cport == 0 && ma->rand != 0) { 
        syslog(LOG_ERR,"protocol error, rand should be 0, %d", ma->rand);
        snprintf(ma->errmsg, MAXERRMSGLEN, "Protocol Error");
        goto bad;
    }

    /* Get Command */

    if ((m_head = munge_parse(ma, m_head, m_end)) == NULL)
        goto bad;
    
    if ((int)strlen(m_head) < ARG_MAX) {
        strncpy(ma->cmd, m_head, ARG_MAX);
        ma->cmd[ARG_MAX - 1] = '\0';
    } else {
        syslog(LOG_ERR, "Not enough space for command: %s", m_head);
        snprintf(ma->errmsg, MAXERRMSGLEN, "Command too long");
        goto bad;
    }

    free(mptr);
    
    snprintf(ma->errmsg, MAXERRMSGLEN, "Success");
    return 0;

 bad:
    free(mptr);
    return -1;
}
