
/* INCLUDES */

#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <regex.h>
#include <openssl/md5.h>
#include <my_global.h>
#include <mysql.h>
#include <sys/epoll.h>
#include <ares.h>

#include <defines.h>
#include <url.h>
#include <httprequest.h>

int main(int argc, char** argv) {
	// Connect to MySQL
        MYSQL* conn = mysql_init(NULL);
        if(mysql_real_connect(conn, "localhost", "crawler", "SpasWehabEp4", "crawler", 0, NULL, 0) == NULL) {
                printf("Unable to connect to MySQL.\n");
                return(0);
        }
        printf("Connected to MySQL.\n");

	Url* url = new Url("http://www.icedteapowered.com/");
	url->parse(NULL);

	HttpRequest* request = new HttpRequest();
	request->fetch_robots(url);

	int socket = request->initialize(url);

	// Set the output filename
        unsigned int dir_length = strlen(BASE_PATH) + strlen(url->get_host());
        char* dir = (char*)malloc(dir_length + 1);
        sprintf(dir, "%s%s", BASE_PATH, url->get_host());

        mkdir(dir, 0644);
        free(dir);

        unsigned int length = strlen(BASE_PATH) + strlen(url->get_host()) + 1 + (MD5_DIGEST_LENGTH * 2) + 5;
        char* filename = (char*)malloc(length + 1);
        sprintf(filename, "%s%s/%s.html", BASE_PATH, url->get_host(), url->get_path_hash());

        request->set_output_filename(filename);
        free(filename);

	// Set up our libevent notification base
        int m_epoll = epoll_create(1);
        if(m_epoll < 0) {
                printf("Unable to create epoll interface.\n");
                return(0);
        }

	// Add the socket to epoll
        struct epoll_event event;
        memset(&event, 0, sizeof(epoll_event));

        event.data.fd = socket;
        event.events = EPOLLIN | EPOLLET | EPOLLOUT;
        if((epoll_ctl(m_epoll, EPOLL_CTL_ADD, socket, &event)) < 0) {
                printf("Unable to setup epoll for %s: %s.\n", url->get_url(), strerror(errno));
                return(0);
        }

        while(true) {
                epoll_event* events = (epoll_event*)malloc(sizeof(epoll_event));
                memset(events, 0, sizeof(epoll_event));

                int msgs = epoll_wait(m_epoll, events, 1, -1);
                for(unsigned int i = 0; i < msgs; i++) {
                        if(!request->process((void*)&events[i])) {
                                // TODO: something went wrong, figure that out
                                printf("ERROR: %s\n", request->get_error());
                        }

			printf("State: %d\n", request->get_state());

                        // If we need to process robots.txt rules, do so
                        if(request->get_state() == HTTPREQUESTSTATE_ROBOTS) {
				// Get the URL we were working on
                                Url* url = request->get_url();

                                printf("Got robots for %s (code %d), processing.\n", url->get_url(), request->get_code());

                                if(request->get_code() == 200) {
                                        // Get and parse the rules
                                        char* content = request->get_content();
					printf("%s\n", content);
                                        char* line = strtok(content, "\r\n");
                                        bool applicable = false;
                                        while(line != NULL) {
                                                if(strlen(line) <= 2) {
                                                        line = strtok(NULL, "\r\n");
                                                        continue;
                                                }

                                                // Check to see if this is a user agent line, start by making it all lowercase
                                                for(unsigned int i = 0; i < strlen(line); i++) {
                                                        line[i] = tolower(line[i]);
                                                }

                                                // If it is a user-agent line, make sure it's aimed at us
                                                if(strstr(line, "user-agent") != NULL) {
                                                        if(strstr(line, "user-agent: *") != NULL) {
                                                                applicable = true;
                                                        }
                                                        else
                                                                applicable = false;
                                                }

                                                if(applicable) {
                                                        // Record the rule in the database
                                                        char* part = strstr(line, "disallow: ");
                                                        if(part) {
                                                                char* position = strchr(line, '\n');
                                                                unsigned int copy_length = (position == NULL) ? strlen(line) : (position - line);
                                                                copy_length -= 10;

                                                                // If copy length is 1, it's just a newline, "Disallow: " means you can index everything
                                                                if(copy_length <= 1) {
                                                                        line = strtok(NULL, "\r\n");
                                                                        continue;
                                                                }

                                                                char* disallowed = (char*)malloc(copy_length + 1);
                                                                strncpy(disallowed, line + 10, copy_length);
                                                                disallowed[copy_length] = '\0';

                                                                char* query = (char*)malloc(5000);
                                                                sprintf(query, "INSERT INTO robotstxt VALUES(%ld, '%s');", url->get_domain_id(), disallowed);
                                                                mysql_query(conn, query);

                                                                free(disallowed);
                                                                free(query);
                                                        }
                                                }

                                                line = strtok(NULL, "\r\n");
                                        }
                                }

                                // Re-send the request for the actual page
                                request->resend();
                        }

                        if(request->get_state() == HTTPREQUESTSTATE_COMPLETE) {
                                // Get the URL we were working on
                                Url* url = request->get_url();
                                int code = request->get_code();

                                // Mark as done and all that
                                char* query = (char*)malloc(1000 + strlen(url->get_url()));
                                time_t now = time(NULL);
                                sprintf(query, "INSERT INTO url VALUES(NULL, %ld, '%s', '%s', '', %d, %ld)", url->get_domain_id(), url->get_url(), url->get_path_hash(), code, now);
                                mysql_query(conn, query);
                                free(query);

                                if(code == 200) {
                                        char* filename = request->get_filename();

                                        // Add it to the parse_queue in redis
					printf("Adding %s to parse queue.\n", filename);
                                }

                                if((code == 302) || (code == 301)) {
                                        // Add the URL back into the queue
					printf("Redirect on URL %s.\n", url->get_url());
                                }
                        }
                }

                free(events);

                // Process any waiting DNS requests
                for(unsigned int i = 0; i < 1; i++) {
                        if(request->get_state() == HTTPREQUESTSTATE_DNS)
                                request->process(NULL);
                }

		usleep(1);
	}

	mysql_close(conn);

	return(0);
}
