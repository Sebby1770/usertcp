#ifndef USERTCP_APP_H
#define USERTCP_APP_H

/* Demo applications that run on top of the stack: a UDP echo server, a TCP
 * echo server, and a minimal HTTP/1.0 server. Registering them is all the
 * wiring main.c needs. */

struct stack;

/* Ports the demo apps listen on. */
#define APP_UDP_ECHO_PORT 7
#define APP_TCP_ECHO_PORT 9999
#define APP_HTTP_PORT     80

void apps_register(struct stack *s);

#endif /* USERTCP_APP_H */
