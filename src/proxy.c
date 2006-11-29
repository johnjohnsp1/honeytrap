/* proxy.c
 * Copyright (C) 2006 Tillmann Werner <tillmann.werner@gmx.de>
 *
 * This file is free software; as a special exception the author gives
 * unlimited permission to copy and/or distribute it, with or without
 * modifications, as long as this notice is preserved.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <strings.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "honeytrap.h"
#include "proxy.h"
#include "logging.h"

int proxy_connect(u_char mode, struct in_addr ipaddr, uint16_t l_port, u_int16_t port, Attack *attack) {
	int proxy_sock_fd, local_addr_len, flags, retval, error;
	socklen_t len;
	struct sockaddr_in proxy_socket, local_socket;
	char *logstr=NULL, *Logstr=NULL, *logact=NULL, *logpre=NULL;
	struct timeval timeout;
	fd_set rfds, wfds;

	error = 0;

	if (attack == NULL) {
		logmsg(LOG_ERR, 1, "Error - No attack record to fill.\n");
		return(-1);
	}

	if (mode == PORTCONF_PROXY) {
		logact = strdup("proxy");
		logstr = strdup("server");
		Logstr = strdup("Server");
		logpre = strdup("==");
	} else if (mode == PORTCONF_MIRROR) {
		logact = strdup("mirror");
		logstr = strdup("mirror");
		Logstr = strdup("Mirror");
		logpre = strdup("<>");
	} else {
		logmsg(LOG_ERR, 1, "%s %u\t  Error - Mode %u for connection handling is not supported.\n",
			logpre, l_port, mode);
		return(-1);
	}

	/* prevent loops - disallow connections to localhost */
	if ((ntohl(ipaddr.s_addr) == 0x7F000001) && PORTCONF_MIRROR) {
		logmsg(LOG_WARN, 1, "%s %u\t  Warning - Connection to %s:%u suppressed for loop prevention.\n",
			logpre, l_port, inet_ntoa(ipaddr), port);
		return(-1);
	}

	/* prepare client socket */

	logmsg(LOG_DEBUG, 1, "%s %u\t  Creating client socket for %s connection.\n", logpre, l_port, logact);
	if (!(proxy_sock_fd = socket(AF_INET, SOCK_STREAM, 0))) { 
		logmsg(LOG_ERR, 1, "%s %u\t  Error - Unable to create client socket for %s connection.\n",
			logpre, port, logact);
		return(-1);
	} else {
		logmsg(LOG_DEBUG, 1, "%s %u\t  Client socket for %s connection created.\n", logpre, l_port, logact);

		/* establish proxy connection */
		logmsg(LOG_DEBUG, 1, "%s %u\t  Establishing %s connection to %s:%u.\n",
			logpre, l_port, logact, inet_ntoa(ipaddr), port);

		bzero(&proxy_socket, sizeof(proxy_socket));
		proxy_socket.sin_family		= AF_INET;
		proxy_socket.sin_addr.s_addr	= ipaddr.s_addr;
		proxy_socket.sin_port		= htons(port);
		

		if (mode == PORTCONF_PROXY) {
			/* blocking connect() in proxy mode */
			if (connect(proxy_sock_fd, (struct sockaddr *) &proxy_socket, sizeof(proxy_socket)) != 0) {
				close(proxy_sock_fd);
				logmsg(LOG_DEBUG, 1, "%s %u\t  Error - Unable to establish %s connection to %s:%d.\n",
					logpre, l_port, logact, inet_ntoa(ipaddr), port);
				return(-1);
			}
		} else if (mode == PORTCONF_MIRROR) {
			/* non-blocking connect() with short timeout to prevent simultane connection timeouts */
			logmsg(LOG_DEBUG, 1, "%s %u\t  Non-blocking, short-timeout connect to %s:%d.\n",
				logpre, l_port, inet_ntoa(ipaddr), port);
			flags = fcntl(proxy_sock_fd, F_GETFL, 0);

			if (fcntl(proxy_sock_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
				fprintf(stderr, "Error in fcntl(): %s.\n", strerror(errno));
				logmsg(LOG_ERR, 1, "%s %u\t  Error - Unable to set mirror socket to non-blocking: %s.\n",
						logpre, l_port, strerror(errno));
				return(-1);
			}
			
			errno = 0;
			if ((retval = connect(proxy_sock_fd, (struct sockaddr *) &proxy_socket, sizeof(proxy_socket))) <0) {
				if (errno != EINPROGRESS) {
					logmsg(LOG_DEBUG, 1,
						"%s %u\t  Error - Unable to establish mirror connection to %s:%d.\n",
						logpre, l_port, inet_ntoa(ipaddr), port);
					return(-1);
				}
			}
			
			if (retval != 0) {
				FD_ZERO(&rfds);
				FD_SET(proxy_sock_fd, &rfds);
				wfds = rfds;
				timeout.tv_sec = 3;
				timeout.tv_usec = 0;
				if (select(proxy_sock_fd+1, &rfds, &wfds, NULL, &timeout) == -1) {
					close(proxy_sock_fd);
					errno = ETIMEDOUT;
					logmsg(LOG_ERR, 1, "%s %u\t  Error - select() call failed: %s \n",
						logpre, l_port, strerror(errno));
					return(-1);
				}
				if (FD_ISSET(proxy_sock_fd, &rfds) || FD_ISSET(proxy_sock_fd, &wfds)) {
					len = sizeof(error);
					if (getsockopt(proxy_sock_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
						logmsg(LOG_DEBUG, 1,
							"%s %u\t  Error - Mirror connection to %s:%d timed out.\n",
							logpre, l_port, inet_ntoa(ipaddr), port);
						return(-1);
					}
				} else {
					close(proxy_sock_fd);
					logmsg(LOG_DEBUG, 1, "Error - Unable to establish mirror connection: %s.\n",
						strerror(ETIMEDOUT));
					return(-1);
				}

			}
			fcntl(proxy_sock_fd, F_SETFL, flags);
			if (error) {
				close(proxy_sock_fd);
					logmsg(LOG_DEBUG, 1, "Error - Unable to establish mirror connection: %s.\n",
						strerror(error));
				return(-1);
			}
		}
		
		local_addr_len = 0;
		if (getsockname(proxy_sock_fd, (struct sockaddr *) &local_socket, &local_addr_len) != 0) {
			logmsg(LOG_ERR, 1, "%s %u\t  Error - Unable to get local address from %s socket: %s\n",
				logpre, port, logact, strerror(errno));
			return(-1);
		}
		attack->p_conn.l_addr	= local_socket.sin_addr; 
		attack->p_conn.r_addr	= proxy_socket.sin_addr;
		attack->p_conn.l_port	= local_socket.sin_port;
		attack->p_conn.r_port	= proxy_socket.sin_port;
	}
	return(proxy_sock_fd);
}
