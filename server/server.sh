#!/bin/bash

SEMAPHORE_NAME='/fviewer_projects_semaphore'
SHARED_MEMORY_OBJECT_NAME='fviewer_projects_shared_memory'
PORT=-1
dir=`dirname $0`

#IF NO ARGUMENTS WERE PROVIDED
function USAGE ()
{
	echo ""
	echo "USAGE: "
	echo "    $0 start|stop|restart|compile [-?p]"
	echo ""
	echo "OPTIONS:"
	echo "    -p  specific port"
	echo "    -?  this usage information"
	echo ""
}

function start_fun ()
{
	if [ "$PORT" = "-1" ]; then
		$dir/proxy_server --semaphore $SEMAPHORE_NAME --shared_memory $SHARED_MEMORY_OBJECT_NAME > proxy_out 2> proxy_err &
	else
		$dir/proxy_server --semaphore $SEMAPHORE_NAME --shared_memory $SHARED_MEMORY_OBJECT_NAME -p $PORT > proxy_out 2> proxy_err &
	fi
	python $dir/parse_server.py --semaphore $SEMAPHORE_NAME --shared_memory $SHARED_MEMORY_OBJECT_NAME > parse_out 2> parse_err &
}

function stop_fun ()
{
	PID=`ps -eF | grep proxy_server | grep -v grep | awk '{print $2}'`
	kill -sigkill $PID
	PID=`ps -eF | grep parse_server.py | grep -v grep | awk '{print $2}'`
	kill -sigkill $PID
}
function restart ()
{
	start
	sleep 10
	stop
}

function compile_fun ()
{
	gcc -Wall -o $dir/proxy_server -lrt $dir/proxy_server.c
}

start=1
stop=2
restart=3
compile=4

action=-1

if [ "$2" = "-p" ]; then
	PORT=$3
elif [ "$2" = "-?" ]; then
	USAGE
	exit 0
else
	PORT=-1
fi

case $1 in
	compile ) compile_fun;;
	start   ) start_fun;;
	stop    ) stop_fun;;
	restart ) stop_fun
			  start_fun;;
	*       ) USAGE
			  exit 0;;
esac
