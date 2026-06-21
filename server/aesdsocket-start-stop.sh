#!/bin/sh
#Startup script for `aesdsocket` in daemon mode with `-d` options

PIDFILE=/tmp/aesdsocket.pid

case "$1" in
	start)
		echo "Running aesdsocket in daemon mode"
		start-stop-daemon --start \
			--name aesdsocket \
			--pidfile $PIDFILE \
			--startas /usr/bin/aesdsocket -- -d
		;;
	stop)
		echo "Stopping aesdsocket gracefully"
		start-stop-daemon --stop \
			--name aesdsocket \
			--pidfile $PIDFILE \
			--retry 5 \
			--remove-pidfile
		;;
	*)
		echo "Usage: $0 {start|stop}"
		exit 1
		;;
esac
