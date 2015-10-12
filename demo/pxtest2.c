/*
 * pxtest2.c : This program simulates one type of user-space driver.
 *
 * Copy every character from input to output but add one to it in the
 * process.  The only exception is the newline character which we pass
 * through unmodified.  This program assumes each read or write is
 * atomic -- not a good * assumption in real life but OK for a test.
 *
 * Typical usage might be
 *    gcc -o pxtest2 pxtest2.c  
 *    ./pxtest2 /dev/proxy &
 *    echo 111aaa222 >/dev/proxy
 *    cat /dev/proxy
 */

#include <stdio.h>
#include <stdlib.h>
#include <termio.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <sys/fcntl.h>
#include <sys/time.h>


extern int errno;
#define PXBUFZS 100


int main (int argc, char *argv[])
{
    fd_set rfds;                /* bit masks for select statement */
    fd_set wfds;                /* bit masks for select statement */
    int    pfd = -1;            /* /dev/proxy file descriptor */
    int    slret,wrret;         /* select() write() return value */
    int    pcount = 0;          /* read() return value */
    char   pbuff[PXBUFZS];
    int    plen;                /* number of characters in proxy buff */
    int    i;


    if (argc != 2) {
        printf("usage: %s <proxy_device>\n", argv[0]);
        exit(1);
    }

    /* start with something in the buffer */
    strcpy(pbuff, "Hi, mom!\n");
    plen = strlen(pbuff);

    while(1) {
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);

        if (pfd == -1) {
            pfd =  open(argv[1], O_RDWR | O_NDELAY,0);
            if (pfd < 0 ) {
                printf("Unable to open proxy port %s\n", argv[1]);
                exit(1);
            }
        }

        FD_SET(pfd, &rfds);
        FD_SET(pfd, &wfds);

        slret = select((pfd + 1), &rfds, &wfds, (fd_set *)NULL, (struct timeval *) NULL);

        /* Read from proxy to pbuff */
        if (FD_ISSET(pfd, &rfds)) {
            pcount = read(pfd, pbuff, PXBUFZS);
            if (pcount > 0) {
                pbuff[pcount] = 0;
                printf("Got string: %s\n", pbuff);
                /* do the increment here */
                for (i = 0; i < pcount ; i++)
                    pbuff[i] += (pbuff[i] == '\n') ? 0 : 1;
                plen = pcount;
            }
            else if (pcount == 0) {
                close(pfd);
                pfd = -1;
                continue;
            }
            else if (pcount < 0) {
                perror("Proxy read error.  Exiting....\n");
                exit(1);
            }
        }

        /* Write from pbuff back to the proxy device */
        if (FD_ISSET(pfd, &wfds)) {
            // sleep(1);               /* simulate a slow device */
            wrret = write(pfd, pbuff, plen);
            if (wrret != plen) {   /* too restrictive, but OK here */
                perror("Proxy write error.  Exiting....\n");
                exit(1);
            }
            (void) write(pfd, pbuff, 0);  /* tell other end to close */
        }
    }
}

