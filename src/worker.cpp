
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <regex.h>
#include <hiredis/hiredis.h>
#include <openssl/md5.h>
#include <my_global.h>
#include <mysql.h>
#include <sys/epoll.h>
#include <ares.h>
#include <time.h>

#include <defines.h>
#include <domain.h>
#include <url.h>
#include <parser.h>
#include <httprequest.h>
#include <worker.h>

Worker::Worker() {
	m_conn = 0;
	m_context = 0;
	m_epoll = 0;
	m_requests = 0;
	m_active = 0;
}

Worker::~Worker() {
	if(m_requests) {
		for(unsigned int i = 0; i < CONNECTIONS_PER_THREAD; i++) {
			if(m_requests[i]) continue;
			delete m_requests[i];
			m_requests[i] = 0;
		}

		free(m_requests);
	}

	if(m_context) {
		redisFree(m_context);
	}

	if(m_conn) {
		mysql_close(m_conn);
	}
}

int Worker::start(int pos) {
	m_procid = pos;

	int pid = fork();
	if(pid == 0) {
		// Start up, connect to databases
        	run();
	}

	return(pid);
}

void Worker::check_redis_connection() {
	redisFree(m_context);

	// Get a connection to redis to grab URLs
        m_context = redisConnect(REDIS_HOST, REDIS_PORT);
        if(m_context->err) {
                printf("Thread #%d unable to connect to redis.\n", m_procid);
                exit(0);
        }
}

Domain* Worker::load_domain_info(char* strdomain) {
	// Our return domain
	Domain* ret = new Domain;

	// Copy the domain in
	ret->domain = (char*)malloc(strlen(strdomain) + 1);
        strcpy(ret->domain, strdomain);

        // See if we have an existing domain
        char* query = (char*)malloc(100 + strlen(strdomain));
        unsigned int length = sprintf(query, "SELECT domain_id, domain, last_access, robots_last_access FROM domain WHERE domain_name = '%s'", strdomain);

        mysql_query(m_conn, query);
        MYSQL_RES* result = mysql_store_result(m_conn);

	free(query);

        if(mysql_num_rows(result)) {
                MYSQL_ROW row = mysql_fetch_row(result);

		// Save the data
                ret->domain_id = atol(row[0]);
		ret->last_access = atol(row[2]);
		ret->robots_last_access = atol(row[3]);
        }
	else {
		// Insert a new row
		query = (char*)malloc(1000);
                length = sprintf(query, "INSERT INTO domain VALUES(NULL, '%s', 0, 0)", strdomain);
                query[length] = '\0';

                mysql_query(m_conn, query);
                free(query);

                ret->domain_id = (unsigned long int)mysql_insert_id(m_conn);
		ret->last_access = 0;
		ret->robots_last_access = 0;
	}

	mysql_free_result(result);

	return(ret);
}

bool Worker::url_exists(Url* url, Domain* info) {
	// See if we have an existing domain
        char* query = (char*)malloc(100 + strlen(url->get_path_hash()));
        sprintf(query, "SELECT page_id FROM page WHERE page_hash = '%s'", url->get_path_hash());

        mysql_query(m_conn, query);
        MYSQL_RES* result = mysql_store_result(m_conn);
	free(query);

	bool exists = false;
	if(mysql_num_rows(result)) {
		exists = true;
	}

	mysql_free_result(result);
	return(exists);
}

bool Worker::check_robots_rules(Url* url) {
	char** rules = 0;
	unsigned int num_rules = 0;
	bool retval = true;

	// Okay, once we're here, we can load the rules and compare the URL
        char* query = (char*)malloc(1000);
        sprintf(query, "SELECT rule FROM robotstxt WHERE domain_id = %ld", url->get_domain_id());

        mysql_query(m_conn, query);
        MYSQL_RES* result = mysql_store_result(m_conn);

        if(mysql_num_rows(result)) {
		num_rules = mysql_num_rows(result);
                rules = (char**)malloc(num_rules * sizeof(char*));

                for(unsigned int i = 0; i < num_rules; i++) {
                        MYSQL_ROW row = mysql_fetch_row(result);
                        rules[i] = (char*)malloc(strlen(row[0]) + 1);
                        strcpy(rules[i], row[0]);
                }
        }

        free(query);
        mysql_free_result(result);

        // Make a lowercase copy of the URL to use for comparison
        char* lowercase = (char*)malloc(strlen(url->get_path()) + 1);
        strcpy(lowercase, url->get_path());

        for(unsigned int i = 0; i < strlen(url->get_path()); i++)
                lowercase[i] = tolower(lowercase[i]);

        for(unsigned int i = 0; i < num_rules; i++) {
                if(strncmp(lowercase, rules[i], strlen(rules[i])) == 0) {
                        retval = false;
                }
		free(rules[i]);
        }

	free(rules);
        free(lowercase);
	return(retval);
}

void Worker::fill_list() {
	// Set up to start doing transfers
        unsigned int max_tries = 100;

        redisReply* reply = (redisReply*)redisCommand(m_context, "LLEN url_queue");
	if(!reply) {
		printf("Y U NO REPLY REDIS?\n");
		exit(0);
	}

	if(reply->type == REDIS_REPLY_ERROR) {
		printf("REDIS ERROR: %s\n", reply->str);
		exit(0);
	}

        max_tries = min(reply->integer, 10);
        freeReplyObject(reply);

	unsigned int counter = 0;
        for(unsigned int i = 0; i < CONNECTIONS_PER_THREAD; i++) {
		if(m_requests[i]) continue;
		if(counter >= max_tries) break;
		counter++;

                // Fetch a URL from redis
                redisReply* reply = (redisReply*)redisCommand(m_context, "LPOP url_queue");
		if(reply->type == REDIS_REPLY_ERROR) {
                	printf("REDIS ERROR: %s\n", reply->str);
        	        exit(0);
	        }

		// Make sure the URL doesn't have quotes around it
		char* urlstr = reply->str;
		if(urlstr[0] == '"') {
			unsigned int length = strlen(reply->str) - 2;
			urlstr = (char*)malloc(length + 1);
			strncpy(urlstr, reply->str + 1, length);
			urlstr[length] = '\0';
		}
		else {
			unsigned int length = strlen(reply->str);
			urlstr = (char*)malloc(length + 1);
			strcpy(urlstr, reply->str);
		}
		freeReplyObject(reply);

                // Split the URL info it's parts; no base URL
                Url* url = new Url(urlstr);
                if(!url->parse(NULL)) {
			printf("unparsable URL %s\n", url->get_url());
			free(urlstr);
			delete url;
                        i--;
                        continue;
		}

		free(urlstr);

                // Verify the scheme is one we want (just http for now)
                if(strcmp(url->get_scheme(), "http") != 0) {
			printf("invalid scheme\n");
                        delete url;
                        i--;
                        continue;
                }

                // Load the site info from the database
                Domain* info = load_domain_info(url->get_host());
                url->set_domain_id(info->domain_id);

                // Check whether this domain is on a timeout
                time_t now = time(NULL);
                if((now - info->last_access) < MIN_ACCESS_TIME) {
                        reply = (redisReply*)redisCommand(m_context, "RPUSH url_queue \"%s\"", url->get_url());
			if(reply->type == REDIS_REPLY_ERROR) {
                		printf("REDIS ERROR: %s\n", reply->str);
	        	        exit(0);
		        }
                        freeReplyObject(reply);

                        delete info;
                        delete url;
                        i--;
                        continue;
                }

                // Next check if we've already parsed this URL
                if(url_exists(url, info)) {
                        delete info;
                        delete url;
                        i--;
                        continue;
                }

		printf("Fetching %s...\n", url->get_url());

		// Create our HTTP request
		m_requests[i] = new HttpRequest();

		// Check if our robots.txt file is valid for this domain
		if(abs(now - info->robots_last_access) > ROBOTS_MIN_ACCESS_TIME) {
			m_requests[i]->fetch_robots(url);
		}
		else {
			// Check our existing robots.txt rules (if any)
			if(!(check_robots_rules(url))) {
				printf("robots rule\n");
				delete m_requests[i];
				m_requests[i] = 0;

				delete info;
				delete url;
				i--;
				continue;
			}
		}

		// Don't need this anymore
		delete info;

                unsigned int length = strlen(BASE_PATH) + strlen(url->get_host()) + 1 + (MD5_DIGEST_LENGTH * 2) + 5;
                char* filename = (char*)malloc(length + 1);
                sprintf(filename, "%s%s_%s.html", BASE_PATH, url->get_host(), url->get_path_hash());
	
		m_requests[i]->set_output_filename(filename);
		free(filename);	

                // Set the last access time to now
                char* query = (char*)malloc(1000);
		sprintf(query, "UPDATE domain SET last_access = %ld WHERE domain_id = %ld", now, url->get_domain_id());
		mysql_query(m_conn, query);
		free(query);

                // Otherwise, we're good
                int socket = m_requests[i]->initialize(url);
                if(!socket) {
                        printf("Thread #%d unable to initialize socket for %s.\n", m_procid, url->get_url());
                        exit(0);
                }

                // Add the socket to epoll
                struct epoll_event event;
                memset(&event, 0, sizeof(epoll_event));

                event.data.fd = socket;
                event.events = EPOLLIN | EPOLLET | EPOLLOUT;
                if((epoll_ctl(m_epoll, EPOLL_CTL_ADD, socket, &event)) < 0) {
                        printf("Thread #%d unable to setup epoll for %s: %s.\n", m_procid, url->get_url(), strerror(errno));
                        exit(0);
                }

		m_active++;
        }
}

void Worker::run() {
	// Get a connection to redis to grab URLs
        m_context = redisConnect(REDIS_HOST, REDIS_PORT);
        if(m_context->err) {
                printf("Thread #%d unable to connect to redis.\n", m_procid);
                return;
        }

        // Connect to MySQL
        m_conn = mysql_init(NULL);
        if(!mysql_real_connect(m_conn, MYSQL_HOST, MYSQL_USER, MYSQL_PASS, MYSQL_DB, 0, NULL, 0)) {
                printf("Thread #%d unable to connect to MySQL: %s\n", m_procid, mysql_error(m_conn));
                return;
        }

	mysql_set_character_set(m_conn, "utf8");

	// Set up our libevent notification base
	m_epoll = epoll_create(CONNECTIONS_PER_THREAD);
	if(m_epoll < 0) {
		printf("Thread #%d unable to create epoll interface.\n", m_procid);
                return;
	}

	// Initialize our stack of HttpRequests
	m_requests = (HttpRequest**)malloc(CONNECTIONS_PER_THREAD * sizeof(HttpRequest*));
	memset(m_requests, 0, CONNECTIONS_PER_THREAD * sizeof(HttpRequest*));

	fill_list();

        while(true) {
		epoll_event* events = (epoll_event*)malloc(CONNECTIONS_PER_THREAD * sizeof(epoll_event));
		memset(events, 0, CONNECTIONS_PER_THREAD * sizeof(epoll_event));

		int msgs = epoll_wait(m_epoll, events, CONNECTIONS_PER_THREAD, 0);
		for(int i = 0; i < msgs; i++) {
			// Find the applicable HttpRequest object
                        int pos = 0;
                        for(unsigned int j = 0; j < CONNECTIONS_PER_THREAD; j++) {
                                if(!m_requests[j]) continue;

                                if(m_requests[j]->get_socket() == events[i].data.fd) {
                                        pos = j;
                                        break;
                                }
                        }

                        if(!m_requests[pos]) {
				printf("ERROR: unable to find request.\n");
				epoll_ctl(m_epoll, EPOLL_CTL_DEL, m_requests[pos]->get_socket(), NULL);
				m_active--;
				continue;
                        }

			if(events[i].events & EPOLLERR) {
				printf("EPOLL ERROR FLAG on %s at stage %d: %s\n", m_requests[pos]->get_url()->get_url(), m_requests[pos]->get_state(), strerror(errno));

				epoll_ctl(m_epoll, EPOLL_CTL_DEL, m_requests[pos]->get_socket(), NULL);
                                delete m_requests[pos];
                                m_requests[pos] = 0;

				m_active--;
                                continue;
			}

			if(!m_requests[pos]->process((void*)&events[i])) {
				// Something went wrong, figure that out
				printf("ERROR: Processing error in stage %d: %s.\n", m_requests[pos]->get_state(), m_requests[pos]->get_error());

				epoll_ctl(m_epoll, EPOLL_CTL_DEL, m_requests[pos]->get_socket(), NULL);
				delete m_requests[pos];
				m_requests[pos] = 0;

				m_active--;
				continue;
			}
		}

		free(events);

		// Process any waiting requests
		for(int i = 0; i < CONNECTIONS_PER_THREAD; i++) {
			if(!m_requests[i]) continue;
	
			int state = m_requests[i]->get_state();
			if(state == HTTPREQUESTSTATE_DNS)
				m_requests[i]->process(NULL);

			state = m_requests[i]->get_state();
			if(state == HTTPREQUESTSTATE_WRITE)
                                m_requests[i]->process(NULL);

			// If we need to process robots.txt rules, do so
			state = m_requests[i]->get_state();
                        if(m_requests[i]->get_state() == HTTPREQUESTSTATE_ROBOTS) {
                                // Get the URL we were working on
                                Url* url = m_requests[i]->get_url();

                                printf("Got robots for %s (code %d), processing.\n", url->get_url(), m_requests[i]->get_code());

                                // Set the last access time to now
                                time_t now = time(NULL);
                                char* query = (char*)malloc(1000);
                                sprintf(query, "UPDATE domain SET robots_last_access = %ld WHERE domain_id = %ld", now, url->get_domain_id());
                                mysql_query(m_conn, query);
                                free(query);

                                if(m_requests[i]->get_code() == 200) {
                                        // Get and parse the rules
                                        char* content = m_requests[i]->get_content();
                                        bool applicable = false;

                                        int content_length = strlen(content);
                                        int line_length = strcspn(content, "\r\n");
                                        int offset = 0;
                                        while(line_length < (content_length - offset - 1)) {
                                                if(line_length <= 2) {
                                                        offset += line_length + 1;
                                                        line_length = strcspn(content + offset, "\r\n");
                                                        continue;
                                                }

                                                char* line = (char*)malloc(line_length + 1);
                                                strncpy(line, content + offset, line_length);
                                                line[line_length] = '\0';

                                                // Check to see if this is a user agent line, start by making it all lowercase
                                                for(unsigned int j = 0; j < strlen(line); j++) {
                                                        line[j] = tolower(line[j]);
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

                                                                char* disallowed = (char*)malloc(copy_length + 1);
                                                                strncpy(disallowed, line + 10, copy_length);
                                                                disallowed[copy_length] = '\0';

                                                                char* query = (char*)malloc(5000);
                                                                sprintf(query, "INSERT INTO robotstxt VALUES(%ld, '%s');", url->get_domain_id(), disallowed);
                                                                mysql_query(m_conn, query);

                                                                free(disallowed);
                                                                free(query);
                                                        }
                                                }

                                                free(line);
                                                line = 0;

                                                offset += line_length + 1;
                                                offset = min(strlen(content) - 1, offset);
                                                if(offset >= (strlen(content) - 1)) break;

                                                line_length = strcspn(content + offset, "\r\n");
                                        }
                                }

                                // Now that we have the robots.txt back, check it again
                                if(!check_robots_rules(url)) {
                                        printf("After fetching robots.txt, %s is disallow.\n", url->get_url());

                                        epoll_ctl(m_epoll, EPOLL_CTL_DEL, m_requests[i]->get_socket(), NULL);

                                        // If it's against the rules, get rid of it and move on
                                        delete m_requests[i];
                                        m_requests[i] = 0;

                                        m_active--;
                                        continue;
                                }

                                // Re-send the request for the actual page
                                epoll_ctl(m_epoll, EPOLL_CTL_DEL, m_requests[i]->get_socket(), NULL);
                                int socket = m_requests[i]->resend();

                                // Add the new socket to epoll
                                struct epoll_event event;
                                memset(&event, 0, sizeof(epoll_event));

                                event.data.fd = socket;
                                event.events = EPOLLIN | EPOLLET | EPOLLOUT;
                                if((epoll_ctl(m_epoll, EPOLL_CTL_ADD, socket, &event)) < 0) {
                                        printf("Unable to setup epoll for %s: %s.\n", url->get_url(), strerror(errno));
					delete m_requests[i];
                                        m_requests[i] = 0;

                                        m_active--;
                                        continue;
                                }
			}

			// Check for any timeouts
			if((state == HTTPREQUESTSTATE_RECV) || (state == HTTPREQUESTSTATE_CONNECTING)) {
				if(!m_requests[i]->process(NULL)) {
					// Make sure the request is still in this status
					state = m_requests[i]->get_state();
					if((state == HTTPREQUESTSTATE_RECV) || (state == HTTPREQUESTSTATE_CONNECTING)) {
						printf("ERROR: %s timeout after %d seconds in stage %d\n", m_requests[i]->get_error(), m_requests[i]->get_last_check() - m_requests[i]->get_last_time(), m_requests[i]->get_state());

						delete m_requests[i];
						m_requests[i] = 0;
						m_active--;

						continue;
					}
				}
			}

			state = m_requests[i]->get_state();
			if(state == HTTPREQUESTSTATE_COMPLETE) {
                                // Get the URL we were working on
                                Url* url = m_requests[i]->get_url();
                                int code = m_requests[i]->get_code();

                                // Mark as done and all that
                                char* query = (char*)malloc(1000 + strlen(url->get_url()));
                                time_t now = time(NULL);
                                sprintf(query, "INSERT INTO page VALUES(NULL, %ld, '%s', '%s', '', %d, %ld)", url->get_domain_id(), url->get_url(), url->get_path_hash(), code, now);
                                mysql_query(m_conn, query);
                                free(query);

                                if(code == 200) {
					// Parse the returned HTML, instantiate a new parser
					Parser* parser = new Parser;
					Url* list = parser->parse(m_requests[i]->get_content());

					// Keep count of how many we find
					int url_count = 0;

					// Loop through any potential list
					Url* current = list;
					while(current) {
						// Parse the URL
						current->parse(m_requests[i]->get_url());

						// Add the URL back into the queue
                                                redisReply* reply = (redisReply*)redisCommand(m_context, "RPUSH url_queue \"%s\"", current->get_url());
                                                if(reply->type == REDIS_REPLY_ERROR) {
                                                       printf("REDIS ERROR: %s\n", reply->str);
                                                       exit(0);
                                                }
                                                freeReplyObject(reply);

						url_count++;
					}

					printf("URL %s contains %d links.\n", url->get_url(), url_count);
                                }

                                if((code == 302) || (code == 301)) {
					if(m_requests[i]->get_effective_url()) {
	                                        // Add the URL back into the queue
        	                                redisReply* reply = (redisReply*)redisCommand(m_context, "RPUSH url_queue \"%s\"", m_requests[i]->get_effective_url());
						if(reply->type == REDIS_REPLY_ERROR) {
         					       printf("REDIS ERROR: %s\n", reply->str);
					               exit(0);
					        }
                	                        freeReplyObject(reply);
					}
                                }

				epoll_ctl(m_epoll, EPOLL_CTL_DEL, m_requests[i]->get_socket(), NULL);
                                delete m_requests[i];
                                m_requests[i] = 0;
				m_active--;

				continue;
                        }

			// Check for any error states and remove them
			state = m_requests[i]->get_state();
                        if(state == HTTPREQUESTSTATE_ERROR) {
				printf("ERROR: Processing error in stage %d: %s.\n", m_requests[i]->get_state(), m_requests[i]->get_error());

                                epoll_ctl(m_epoll, EPOLL_CTL_DEL, m_requests[i]->get_socket(), NULL);
                                delete m_requests[i];
                                m_requests[i] = 0;

                                m_active--;
                                continue;
			}

			// Finally, no matter what if it's been 60 seconds without any activity, shut it down
			time_t now = time(NULL);
			if(abs(now - m_requests[i]->get_last_time()) > HTTPTIMEOUT_ANY) {
				printf("TIMED OUT: %s, stage %d\n", m_requests[i]->get_url()->get_url(), m_requests[i]->get_state());

				epoll_ctl(m_epoll, EPOLL_CTL_DEL, m_requests[i]->get_socket(), NULL);
                                delete m_requests[i];
                                m_requests[i] = 0;
                                m_active--;
			}
		}

		// Re-fill the list
		if(m_active < CONNECTIONS_PER_THREAD)
			fill_list();
        }

        // Clean up
        mysql_close(m_conn);
        redisFree(m_context);

        return;
}
