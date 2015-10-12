## /dev/proxy:  A bidirectional pipe 

## WHAT:
- simple bidirectional pipe IPC
- like /sys or /proc but for *your* program instead of the kernel
- kernel module implementing a character device


## WHY:
- Simple: Use mknod to create a program interface as a device node
- Simple: API is just open()/read()/write()/close()
- Simple: Works with select() for event driven programming
- Simple: Write of zero bytes closes the other end
- Simple: Works with ALL programming languages, even Bash
- Simple: No dependencies and no libraries to install
- Simple: Builds on all Linux systems
- Secure: Access rights tied to filesystem permissions on device node


## INSTALLATION:
    sudo apt-get install linux-headers-`uname -r`
    git clone https://github.com/bob-linuxtoys/proxy.git
    cd proxy
    make
    sudo make install


## TEST:
    sudo modprobe proxy

    # Get proxy driver's major number to create device node
    PROXYMAJOR=`grep proxy /proc/devices | awk '{print $1}'`
    sudo mknod /dev/proxytest c $PROXYMAJOR 0
    sudo chmod 666 /dev/proxytest

    # Sender blocks waiting for the other end to open
    date > /dev/proxytest &
    # Open the other end to unblock sender
    cat /dev/proxytest

    # See pxtest1.c and pxtest2.c for a more complete test of proxy


## NOTES:
See Linux Journal of August, 2010 for an article on proxy and fanout

See also http://github.org/bob-linuxtoys/fanout


## COPYRIGHT:
Copyright 2010-2015, Bob Smith


## LICENSE:
At your discretion, you may consider this software covered
by either the GNU Public License, Version 2, or the BSD
3-Clause license.

    http://opensource.org/licenses/GPL-2.0
    http://opensource.org/licenses/BSD-3-Clause


