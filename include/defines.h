
#ifndef __DEFINES_H__
#define __DEFINES_H__

#define NUM_THREADS			1
#define CONNECTIONS_PER_THREAD		10
#define SOCKET_BUFFER_SIZE		2048
#define MAX_PAGE_SIZE			2097152

#define BASE_PATH       		"/var/indexer/"

#define MIN_ACCESS_TIME 		10
#define ROBOTS_MIN_ACCESS_TIME		604800 			// One week

#define	MYSQL_HOST			"127.0.0.1"
#define MYSQL_USER			"crawler"
#define MYSQL_PASS			"SpasWehabEp4"
#define MYSQL_DB			"crawler"

#define REDIS_HOST			"127.0.0.1"
#define REDIS_PORT			6379

#endif
