/*
 * pxtest.c : This program simulates one kind of user-space driver.
 *
 * Copy tp the proxy device specified as the first parameter the file
 * specified as the second command line parameter.  Read from the proxy
 * device and copy any data received to standard out.
 *
 * The design of the program is as follows..  Two buffers are used in
 * passing the data from one side to the other.  If the proxy buffer is
 * empty then read from the proxy device.  If the proxy buffer has data,
 * then write it to standard out.  If the file buffer is empty, then read
 * from the file to fill it.  If the file buffer has data, then write it
 * to the proxy device.   We usually select() on one file descriptor for
 * each direction.  Which file descriptor depends on whether that buffer 
 * has data or not.
 *
 * Typical usage might be:
 *    gcc -o pxtest1 pxtest1.c
 *    ./pxtest1 /dev/proxy bigfile1 > echo2 &
 *    ./pxtest1 /dev/proxy bigfile2 > echo1
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
#define PXBUFZS 4000

int main (int argc, char *argv[])
{
    fd_set rfds;                /* bit masks for select statement */
    fd_set wfds;                /* bit masks for select statement */
    int    pfd = -1;            /* /dev/proxy file descriptor */
    int    preaddone = 0;       /* ==1 after the other end closes */
    int    ffd = -1;            /* file system FD.  ==-1 after close */
    int    mxfd = 1;
    int    slret,wrret;         /* select() write() return value */
    char   pbuff[PXBUFZS];
    int    pcount = 0;          /* number of characters in proxy buff */
    char   fbuff[PXBUFZS];
    int    fcount = 0;          /* number of characters in file buff */


    if (argc != 3) {
        printf("usage: %s <proxy_device> <file_to_send>\n", argv[0]);
        exit(1);
    }
    if ((pfd = open(argv[1], O_RDWR,0)) < 0 ) {
        printf("Unable to open proxy port %s\n", argv[1]);
        exit(1);
    }
    mxfd = (pfd > mxfd) ? pfd : mxfd ;

    if ((ffd = open(argv[2], O_RDONLY,0)) < 0 ) {
        printf("Unable to open: %s\n", argv[2]);
        exit(1);
    }
    mxfd = (ffd > mxfd) ? ffd : mxfd ;
    mxfd = mxfd + 1;

    while(1) {
        /* All done if nothing to send to stdout, got a close from the
           other end on our read, and done reading the file to send */
        if ((pcount == 0) && (preaddone == 1) && (ffd == -1))
            exit(0);


        FD_ZERO(&rfds);
        FD_ZERO(&wfds);

        if (pcount != 0)
            FD_SET(1, &wfds);
        else {
            if (preaddone == 0)
                FD_SET(pfd, &rfds);
        }

        if (fcount != 0) {
            FD_SET(pfd, &wfds);
        }
        else {
            if (ffd != -1)
                FD_SET(ffd, &rfds);
        }

        slret = select(mxfd, &rfds, &wfds, (fd_set *)NULL, (struct timeval *) NULL);

        /* Read from proxy to pbuff */
        if (FD_ISSET(pfd, &rfds)) {
            pcount = read(pfd, pbuff, PXBUFZS);
            if (pcount == 0) {
                preaddone = 1;
            }
            else if (pcount < 0) {
                perror("Proxy read error.  Exiting....\n");
                exit(1);
            }
        }

        /* Write from pbuff to std out */
        if (FD_ISSET(1, &wfds)) {
            wrret = write(1, pbuff, pcount);
            if (wrret <= 0) {
                perror("Standard Out write error.  Exiting....\n");
                exit(1);
            }
            else if (wrret == pcount)
                pcount = 0;
            else {
                (void) memmove(pbuff, pbuff+wrret, pcount-wrret);
                pcount = pcount - wrret;
            }
        }

        /* Read from file to fbuff */
        if ((ffd != -1) && FD_ISSET(ffd, &rfds)) {
            fcount = read(ffd, fbuff, PXBUFZS);
            if (fcount < 0) {
                perror("File read error.  Exiting....\n");
                exit(1);
            }
            else if (fcount == 0) {
                close(ffd);
                ffd = -1;
                /* Write 0 bytes to tell proxy driver to close */
                (void) write(pfd, fbuff, 0);
            }
        }

        /* Write from fbuff to proxy device */
        if (FD_ISSET(pfd, &wfds)) {
            wrret = write(pfd, fbuff, fcount);
            if (wrret <= 0) {
                perror("Proxy device write error.  Exiting....\n");
                exit(1);
            }
            else if (wrret == fcount)
                fcount = 0;
            else {
                (void) memmove(fbuff, fbuff+wrret, fcount-wrret);
                fcount = fcount - wrret;
            }
        }
    }
}

