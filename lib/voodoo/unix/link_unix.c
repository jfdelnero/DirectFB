/*
   (c) Copyright 2001-2009  The world wide DirectFB Open Source Community (directfb.org)
   (c) Copyright 2000-2004  Convergence (integrated media) GmbH

   All rights reserved.

   Written by Denis Oliver Kropp <dok@directfb.org>,
              Andreas Hundt <andi@fischlustig.de>,
              Sven Neumann <neo@directfb.org>,
              Ville Syrj�l� <syrjala@sci.fi> and
              Claudio Ciccani <klan@users.sf.net>.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
*/

//#define DIRECT_ENABLE_DEBUG

#include <config.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <direct/debug.h>
#include <direct/list.h>
#include <direct/mem.h>
#include <direct/messages.h>
#include <direct/util.h>

#include <voodoo/client.h>
#include <voodoo/conf.h>
#include <voodoo/internal.h>
#include <voodoo/link.h>
#include <voodoo/manager.h>
#include <voodoo/play.h>


D_DEBUG_DOMAIN( Voodoo_Link, "Voodoo/Link", "Voodoo Link" );

/**********************************************************************************************************************/

#if !VOODOO_BUILD_NO_SETSOCKOPT
static const int one = 1;
static const int tos = IPTOS_LOWDELAY;
#endif

/**********************************************************************************************************************/

#define DUMP_SOCKET_OPTION(fd,o)                                                \
do {                                                                            \
     int val = 0;                                                               \
     unsigned int len = 4;                                                      \
                                                                                \
     if (getsockopt( fd, SOL_SOCKET, o, &val, &len ))                           \
          D_PERROR( "Voodoo/Manager: getsockopt() for " #o " failed!\n" );      \
     else                                                                       \
          D_DEBUG( "Voodoo/Manager: " #o " is %d\n", val );                     \
} while (0)

/**********************************************************************************************************************/

typedef struct {
     int fd[2];
     int wakeup_fds[2];
} Link;

static void
Close( VoodooLink *link )
{
     Link *l = link->priv;

     D_INFO( "Voodoo/Link: Closing connection.\n" );

     close( l->fd[0] );

     if (l->fd[1] != l->fd[0])
          close( l->fd[1] );

     D_FREE( link->priv );
     link->priv = NULL;
}

static ssize_t
Read( VoodooLink *link,
      void       *buffer,
      size_t      count )
{
     Link *l = link->priv;

     return recv( l->fd[0], buffer, count, 0 );
}

static ssize_t
Write( VoodooLink *link,
       const void *buffer,
       size_t      count )
{
     Link *l = link->priv;

     return send( l->fd[1], buffer, count, 0 );
}

static DirectResult
SendReceive( VoodooLink  *link,
             VoodooChunk *sends,
             size_t       num_send,
             VoodooChunk *recvs,
             size_t       num_recv )
{
     Link    *l = link->priv;
     size_t   i;
     ssize_t  ret;
     int      select_result;

     D_DEBUG_AT( Voodoo_Link, "%s( link %p, sends %p, num_send %zu, recvs %p, num_recv %zu )\n",
                 __func__, link, sends, num_send, recvs, num_recv );
/*
     for (i=0; i<num_send; i++) {
          //printf("writing %d\n",sends[i].length);
          ret = send( l->fd, sends[i].ptr, sends[i].length, 0 );
          printf("wrote %d/%d\n",ret,sends[i].length);
          if (ret < 0) {
               if (errno == EAGAIN) {
                    break;
               }
               D_PERROR( "Voodoo/Link: Failed to send() data!\n" );
          }
          else {
               sends[i].done = ret;

               if (sends[i].done != sends[i].length) {
                    D_UNIMPLEMENTED();
               //               direct_mutex_unlock( &l->lock );
                    return DR_UNIMPLEMENTED;
               }

               return DR_OK;
          }
     }
*/

     while (true) {
          fd_set         fds_read;
          fd_set         fds_write;
          struct timeval tv;

          FD_ZERO( &fds_read );
          FD_ZERO( &fds_write );

          if (num_recv) {
               //printf("wait for recv\n");
               FD_SET( l->fd[0], &fds_read );
          }

          if (num_send) {
               //printf("wait for send\n");
               FD_SET( l->fd[1], &fds_write );
          }

          FD_SET( l->wakeup_fds[0], &fds_read );

          tv.tv_sec  = 0;
          tv.tv_usec = 100000;

          D_DEBUG_AT( Voodoo_Link, "  -> select( %s%s )...\n", num_recv ? "R" : " ", num_send ? "W" : " " );
          select_result = select( MAX(MAX(l->wakeup_fds[0],l->fd[0]),l->fd[1])+1, &fds_read, &fds_write, NULL, &tv );
          D_DEBUG_AT( Voodoo_Link, "  -> select result: %d\n", select_result );
          switch (select_result) {
               default:
                    if (FD_ISSET( l->fd[1], &fds_write )) {
                         D_DEBUG_AT( Voodoo_Link, "  => WRITE\n" );

                         //printf("<-- event write\n");

                         for (i=0; i<num_send; i++) {
                              //printf("writing %d\n",sends[i].length);
                              ret = send( l->fd[1], sends[i].ptr, sends[i].length, 0 );
                              //printf("wrote %d/%d\n",ret,sends[i].length);
                              if (ret < 0) {
                                   if (errno == EAGAIN) {
                                        break;
                                   }
                                   D_PERROR( "Voodoo/Link: Failed to send() data!\n" );
                              }
                              else {
                                   sends[i].done = ret;
                         
                                   if (sends[i].done != sends[i].length) {
                                        D_UNIMPLEMENTED();
                                        return DR_UNIMPLEMENTED;
                                   }
                         
                                   return DR_OK;
                              }
                         }
                    }
     
                    if (FD_ISSET( l->fd[0], &fds_read )) {
                         D_DEBUG_AT( Voodoo_Link, "  => READ\n" );

                         //printf("<-- event read\n");

                         for (i=0; i<num_recv; i++) {
                              ret = recv( l->fd[0], recvs[i].ptr, recvs[i].length, 0 );
                              //printf("read %d\n",ret);
                              if (ret < 0) {
                                   if (errno == EAGAIN) {
                                        break;
                                   }
                                   D_PERROR( "Voodoo/Link: Failed to recv() data!\n" );
                                   return DR_FAILURE;
                              }

                              if (!ret)
                                   return DR_IO;
          
                              recvs[i].done = ret;
          
                              if (recvs[i].done < recvs[i].length)
                                   return DR_OK;

                              return DR_OK;
                         }
                    }

                    if (FD_ISSET( l->wakeup_fds[0], &fds_read )) {
                         D_DEBUG_AT( Voodoo_Link, "  => WAKE UP\n" );

                         static char buf[1000];
                         read( l->wakeup_fds[0], buf, sizeof(buf) );
                         return DR_INTERRUPTED;
                    }
                    break;
     
               case 0:
                    D_DEBUG_AT( Voodoo_Link, "  => TIMEOUT\n" );

                    //printf("<-- timeout\n");
                    return DR_TIMEOUT;
                    break;
     
               case -1:
                    D_ERROR( "Voodoo/Link: select() failed!\n" );
                    return DR_FAILURE;
          }
     }

     return DR_OK;
}

static DirectResult
WakeUp( VoodooLink *link )
{
     Link *l = link->priv;
     char  c = 0;

     write( l->wakeup_fds[1], &c, 1 );

     return DR_OK;
}

/**********************************************************************************************************************/

DirectResult
voodoo_link_init_connect( VoodooLink *link,
                          const char *hostname,
                          int         port,
                          bool        raw )
{
     DirectResult     ret;
     int              err;
     struct addrinfo  hints;
     struct addrinfo *addr;
     char             portstr[10];
     Link            *l;


     memset( &hints, 0, sizeof(hints) );
     hints.ai_flags    = AI_CANONNAME;
     hints.ai_socktype = SOCK_STREAM;
     hints.ai_family   = PF_UNSPEC;

     D_INFO( "Voodoo/Link: Looking up host '%s'...\n", hostname );

     snprintf( portstr, sizeof(portstr), "%d", port );

     err = getaddrinfo( hostname, portstr, &hints, &addr );
     if (err) {
          switch (err) {
               case EAI_FAMILY:
                    D_ERROR( "Direct/Log: Unsupported address family!\n" );
                    return DR_UNSUPPORTED;

               case EAI_SOCKTYPE:
                    D_ERROR( "Direct/Log: Unsupported socket type!\n" );
                    return DR_UNSUPPORTED;

               case EAI_NONAME:
                    D_ERROR( "Direct/Log: Host not found!\n" );
                    return DR_FAILURE;

               case EAI_SERVICE:
                    D_ERROR( "Direct/Log: Service is unreachable!\n" );
                    return DR_FAILURE;

#ifdef EAI_ADDRFAMILY
               case EAI_ADDRFAMILY:
#endif
               case EAI_NODATA:
                    D_ERROR( "Direct/Log: Host found, but has no address!\n" );
                    return DR_FAILURE;

               case EAI_MEMORY:
                    return D_OOM();

               case EAI_FAIL:
                    D_ERROR( "Direct/Log: A non-recoverable name server error occurred!\n" );
                    return DR_FAILURE;

               case EAI_AGAIN:
                    D_ERROR( "Direct/Log: Temporary error, try again!\n" );
                    return DR_TEMPUNAVAIL;

               default:
                    D_ERROR( "Direct/Log: Unknown error occured!?\n" );
                    return DR_FAILURE;
          }
     }


     l = D_CALLOC( 1, sizeof(Link) );
     if (!l)
          return D_OOM();

     /* Create the client socket. */
     l->fd[0] = socket( addr->ai_family, SOCK_STREAM, 0 );
     if (l->fd[0] < 0) {
          ret = errno2result( errno );
          D_PERROR( "Voodoo/Link: Socket creation failed!\n" );
          freeaddrinfo( addr );
          D_FREE( l );
          return ret;
     }
     l->fd[1] = l->fd[0];

     D_INFO( "Voodoo/Link: Connecting to '%s:%d'...\n", addr->ai_canonname, port );

     /* Connect to the server. */
     err = connect( l->fd[0], addr->ai_addr, addr->ai_addrlen );
     freeaddrinfo( addr );

     if (err) {
          ret = errno2result( errno );
          D_PERROR( "Voodoo/Link: Socket connect failed!\n" );
          close( l->fd[0] );
          D_FREE( l );
          return ret;
     }


#if !VOODOO_BUILD_NO_SETSOCKOPT
     if (setsockopt( l->fd[0], SOL_IP, IP_TOS, &tos, sizeof(tos) ) < 0)
          D_PERROR( "Voodoo/Manager: Could not set IP_TOS!\n" );

     if (setsockopt( l->fd[0], SOL_TCP, TCP_NODELAY, &one, sizeof(one) ) < 0)
          D_PERROR( "Voodoo/Manager: Could not set TCP_NODELAY!\n" );
#endif

     DUMP_SOCKET_OPTION( l->fd[0], SO_SNDLOWAT );
     DUMP_SOCKET_OPTION( l->fd[0], SO_RCVLOWAT );
     DUMP_SOCKET_OPTION( l->fd[0], SO_SNDBUF );
     DUMP_SOCKET_OPTION( l->fd[0], SO_RCVBUF );

     if (!raw) {
          link->code = 0x80008676;

          if (write( l->fd[1], &link->code, sizeof(link->code) ) != 4) {
               D_ERROR( "Voodoo/Link: Coult not write initial four bytes!\n" );
               close( l->fd[0] );
               D_FREE( l );
               return DR_IO;
          }
     }

     pipe( l->wakeup_fds );


     link->priv        = l;
     link->Close       = Close;
     link->Read        = Read;
     link->Write       = Write;
     link->SendReceive = SendReceive;
     link->WakeUp      = WakeUp;

     return DR_OK;
}

DirectResult
voodoo_link_init_fd( VoodooLink *link,
                     int         fd[2] )
{
     Link *l;

     if (read( fd[0], &link->code, sizeof(link->code) ) != 4) {
          D_ERROR( "Voodoo/Link: Coult not read initial four bytes!\n" );
          return DR_IO;
     }

     l = D_CALLOC( 1, sizeof(Link) );
     if (!l)
          return D_OOM();

     l->fd[0] = fd[0];
     l->fd[1] = fd[1];

     pipe( l->wakeup_fds );

     link->priv        = l;
     link->Close       = Close;
     link->Read        = Read;
     link->Write       = Write;
     link->SendReceive = SendReceive;
     link->WakeUp      = WakeUp;

     return DR_OK;
}
