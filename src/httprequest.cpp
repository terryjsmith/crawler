
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
#include <netdb.h>

#include <defines.h>
#include <url.h>

#include <httprequest.h>

HttpRequest::HttpRequest() {
	m_socket = 0;
	m_state = 0;
	m_url = 0;
	m_content = 0;
	m_header = 0;
	m_inheader = true;
	m_filename = 0;
	m_error = 0;
	m_channel = 0;
	m_size = 0;
	m_code = 0;
	m_chunked = false;
	m_chunksize = 0;
	m_chunkread = 0;
	m_effective = 0;
	m_robots = 0;
	m_contentlength = 0;
	m_fp = 0;
	m_connecttime = 0;
	m_keepalive = false;
	m_lasttime = time(NULL);
	m_lastcheck = time(NULL);
	memset(&m_sockaddr, 0, sizeof(sockaddr_in));
}

HttpRequest::~HttpRequest() {
	if(m_socket) {
		close(m_socket);
		m_socket = 0;
	}

	if(m_url) {
		delete m_url;
		m_url = 0;
	}

	if(m_content) {
		free(m_content);
		m_content = 0;
	}

	if(m_header) {
		free(m_header);
		m_header = 0;
	}

	if(m_filename) {
		free(m_filename);
		m_filename = 0;
	}

	if(m_error) {
		free(m_error);
		m_error = 0;
	}

	if(m_channel) {
		ares_destroy(m_channel);
	}

	if(m_effective) {
		free(m_effective);
		m_effective = 0;
	}

	if(m_robots) {
		delete m_robots;
		m_robots = 0;
	}

	if(m_fp)  {
		fclose(m_fp);
		m_fp = 0;
	}
}

void HttpRequest::set_output_filename(char* filename) {
	unsigned int length = strlen(filename);
	m_filename = (char*)malloc(length + 1);
	strcpy(m_filename, filename);
}

int HttpRequest::initialize(Url* url) {
	// Save a copy of the URL
	m_url = url;

	// Initialize the socket for TCP
	if((m_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		m_error = (char*)malloc(1000);
		sprintf(m_error, "Unable to create socket.");
                return(0);
        }

	// Tell the socket not to doddle
	linger no = { 0, 0 };
	setsockopt(m_socket, SOL_SOCKET, SO_LINGER, &no, sizeof(linger));

	// Start our DNS request
	ares_init(&m_channel);
	ares_gethostbyname(m_channel, m_url->get_host(), AF_INET, _dns_lookup, this);
	m_state = HTTPREQUESTSTATE_DNS;
	m_lasttime = time(NULL);

        return(m_socket);
}

void HttpRequest::fetch_robots(Url* url) {
	// Compute the robots.txt URL
        char* path = "/robots.txt";
        m_robots = new Url(path);
        m_robots->parse(url);
}

void HttpRequest::_dns_lookup(void *arg, int status, int timeouts, hostent* host) {
	HttpRequest* req = (HttpRequest*)arg;
	if((status == ARES_SUCCESS) && (host)) {
		req->connect(host);
	}
	else {
		req->error("Unable to do DNS lookup");
	}
}

void HttpRequest::connect(struct hostent* host) {
	if(host) {
		if(host->h_addr && host->h_length) {
			m_sockaddr.sin_family = AF_INET;
			bcopy((char*)host->h_addr, (char*)&m_sockaddr.sin_addr.s_addr, host->h_length);
			m_sockaddr.sin_port = htons(80);
		}
	}

	// Make our socket non-blocking
        int flags;
        if((flags = fcntl(m_socket, F_GETFL, 0)) < 0) {
                m_error = (char*)malloc(1000);
                printf("Unable to read socket for non-blocking I/O on %s.", m_url->get_url());
                return;
        }

        flags |= O_NONBLOCK;
        if((fcntl(m_socket, F_SETFL, flags)) < 0) {
                printf("Unable to set up socket for non-blocking I/O on %s.\n", m_url->get_url());
                return;
        }

	m_lasttime = time(NULL);
	m_lastcheck = time(NULL);

	::connect(m_socket,(struct sockaddr *)&m_sockaddr, sizeof(sockaddr));
        m_state = HTTPREQUESTSTATE_CONNECTING;
}

void HttpRequest::error(char* error) {
	m_error = (char*)malloc(strlen(error) + 2 + strlen(m_url->get_url()) + 1);
	sprintf(m_error, "%s: %s", error, m_url->get_url());
	m_state = HTTPREQUESTSTATE_ERROR;
}

int HttpRequest::resend() {
	// Get rid of old content
	free(m_content);
	m_content = 0;

	free(m_header);
	m_header = 0;

	m_size = 0;
	m_chunked = false;
	m_chunksize = 0;
	m_chunkread = 0;
	m_contentlength = 0;
	m_inheader = true;

	close(m_socket);

	// Tell the socket not to doddle
        linger no = { 0, 0 };
        setsockopt(m_socket, SOL_SOCKET, SO_LINGER, &no, sizeof(linger));

	// Initialize the socket for TCP
        if((m_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
                m_error = (char*)malloc(1000);
                sprintf(m_error, "Unable to create socket.");
                return(0);
        }

	// If we weren't able to keep the connection alive, re-open it
	connect(NULL);
	return(m_socket);
}

bool HttpRequest::process(void* arg) {
	// we use if statements here so that we can execute multiple code blocks in a row where necessary
	if(m_state == HTTPREQUESTSTATE_DNS) {
		// Otherwise, poll to see if the data we need is available yet
		int nfds, count;
		fd_set read, write;
		struct timeval tv, *tvp;

		FD_ZERO(&read);
 		FD_ZERO(&write);
		nfds = ares_fds(m_channel, &read, &write);

		if(nfds == 0) return(true);

		tvp = ares_timeout(m_channel, NULL, &tv);
		count = select(nfds, &read, &write, NULL, tvp);

		// This will automatically fire off our callback if it finds something so nothing to do after this
		ares_process(m_channel, &read, &write);
		return(true);
	}

	if(m_state == HTTPREQUESTSTATE_CONNECTING) {
		// Find out if we've connected to errored out
		if(arg) {
			epoll_event* event = (epoll_event*)arg;
			if(event->events & EPOLLERR) {
				m_error = (char*)malloc(1000 + strlen(m_url->get_url()));
				sprintf(m_error, "Unable to connect to %s: %s", m_url->get_url(), strerror(errno));
				m_state = HTTPREQUESTSTATE_ERROR;
				return(false);
			}
			else {
				time_t now = time(NULL);
				m_connecttime = max(now - m_lasttime, 1);
				m_lasttime = now;
				m_state = HTTPREQUESTSTATE_SEND;
			}
		}
		else {
			time_t now = time(NULL);
			if(abs(now - m_lasttime) > HTTPTIMEOUT_CONNECT) {
				this->error("Timeout on connect.");
				return(false);
			}
		}
	}

	if(m_state == HTTPREQUESTSTATE_SEND) {
		Url* use = (m_robots == 0) ? m_url : m_robots;

		// Connect should have been successful, send data
		char* request = (char*)malloc(strlen(use->get_path()) + strlen(use->get_query()) + strlen(use->get_host()) + 250);
		char* always = "Accept-Encoding:\r\nAccept: text/html,application/xhtml+xml,application/xml\r\nUser-Agent: Pointer.io Crawler (+http://www.pointer.io/crawler/)\r\nConnection: Keep-alive";
		unsigned int length = 0;

		if(strlen(use->get_query())) {
			length = sprintf(request, "GET %s?%s HTTP/1.1\r\nHost: %s\r\n%s\r\n\r\n", use->get_path(), use->get_query(), use->get_host(), always);
		}
		else {
			length = sprintf(request, "GET %s HTTP/1.1\r\nHost: %s\r\n%s\r\n\r\n", use->get_path(), use->get_host(), always);
		}

		send(m_socket, request, length, 0);
		free(request);
		m_lasttime = time(NULL);
		m_state = HTTPREQUESTSTATE_RECV;
		return(true);
	}

	if(m_state == HTTPREQUESTSTATE_RECV) {
		if(arg) {
			while(true) {
				bool end_of_file = false;
        	       		char* buffer = (char*)malloc(SOCKET_BUFFER_SIZE + 1);
               			memset(buffer, 0, SOCKET_BUFFER_SIZE);

	                	int count = read(m_socket, buffer, SOCKET_BUFFER_SIZE);
				m_lasttime = time(NULL);
	               		if(count < 0) {
					if(errno == EAGAIN) {
			                        free(buffer);
        	       			        break;
					}
					else {
						this->error("EPOLL READ ERROR\n");
						return(false);
					}

					break;
	                	}

	               		if(count == 0) {
        	       		        free(buffer);
					if(m_size)
						m_state = HTTPREQUESTSTATE_WRITE;
	                        	break;
	               		}

		                if(count > 0) {
					buffer[count] = '\0';
					bool found_end = false;
					int handled = 0;

					// If we're not handling robots.txt, output
                                        if(!m_robots) {
                                                if(!m_fp && m_filename) {
                                                        m_fp = fopen(m_filename, "w");
                                                        if(!m_fp) {
                                                                m_error = (char*)malloc(100 + strlen(m_filename));
								sprintf(m_error, "Unable to open file %s.", m_filename);

                                                                m_state = HTTPREQUESTSTATE_ERROR;
                                                                return(false);
                                                        }
                                                }
					}

					if(m_inheader) {
						char* pos = strstr(buffer, "\r\n\r\n");
						found_end = (pos == NULL) ? false : true;
						m_inheader = (pos == NULL) ? true : false;

						int existing_length = (m_header == 0) ? 0 : strlen(m_header);
						int new_length = (pos == NULL) ? count : pos - buffer + 4;
						handled += new_length;

						int length = (m_header == 0) ? new_length : existing_length + new_length;

						char* header = (char*)malloc(length + 1);
						if(m_header)
							strncpy(header, m_header, existing_length);
						strncpy(header + existing_length, buffer, new_length);
						header[length] = '\0';

						// Write the header out to a file
                                                if(!m_robots && m_filename)
                                                        fwrite(buffer, new_length, 1, m_fp);

						if(m_header)
							free(m_header);

						m_header = header;
					}

					// If we've found the end of the header, parse it
					if(found_end) {
						// Get the code out
						if(strlen(m_header) > 13) {
							char* code = (char*)malloc(4);
				                        strncpy(code, m_header + 9, 3);
				                        code[3] = '\0';
				                        m_code = atoi(code);
				                        free(code);
						}

						int line_length = strcspn(m_header, "\n");
						int header_length = strlen(m_header);
						int offset = 0;
						while(line_length < (header_length - offset)) {
							if(line_length <= 1) { 
								offset += line_length + 1;
								break;
							}

							// Get the line out
							char* line = (char*)malloc(line_length + 1);
				                        strncpy(line, m_header + offset, line_length);
				                        line[line_length] = '\0';

							// Set the line to all lower case for our comparisons
							for(int i = 0; i < line_length; i++)
								line[i] = tolower(line[i]);

							if((strstr(line, "location: ")) == line) {
				                                unsigned int length = strlen(line) - 11;
				                                m_effective = (char*)malloc(length + 1);
				                                strncpy(m_effective, line + 10, length);
				                                m_effective[length] = '\0';
				                        }

				                        // If we have a keep-alive header, mark that too
				                        if((strstr(line, "connection: keep-alive")) == line) {
				                                m_keepalive = true;
				                        }

							// If we have a content length header, process
							if((strstr(line, "content-length: ")) == line) {
								m_contentlength = atol(line + 16);
							}

							if((strstr(line, "transfer-encoding: chunked")) == line) {
								m_chunked = true;
							}

							offset += line_length + 1;
							free(line);

							if(offset >= header_length) break;
							line_length = strcspn(m_header + offset, "\n");
						}
					}

					if(m_chunked) {
						while(handled < count) {
							if(!m_chunksize) {
								// Read in the next chunksize
								int length = strcspn(buffer + handled, "\n");
								m_chunkread = 0;

								// If this is a blank line or just a newline character in it, move to the next line
								if(length <= 1) {
									handled += length + 1;
									continue;
								}

								char* hex = (char*)malloc(length + 1);
								strncpy(hex, buffer + handled, length);
								hex[length] = '\0';
								m_chunksize = strtol(hex, NULL, 16);
								free(hex);

								if(m_chunksize == 0) {
									end_of_file = true;
									break;
								}

								handled += length + 1;
								continue;
							}

							if(m_chunkread < m_chunksize) {
								// Read up to a maximum of m_chunksize - m_chunkread bytes
								int max_read = m_chunksize - m_chunkread;
								int new_length = (max_read > (count - handled)) ? count - handled : max_read;

								// Write what we have out to file
								if(!m_robots && m_filename)
									fwrite(buffer + handled, new_length, 1, m_fp);

								// Copy the existing content in and the new content
								char* new_content = (char*)malloc(m_size + new_length + 1);
								if(m_content)
									strncpy(new_content, m_content, m_size);
								strncpy(new_content + m_size, buffer + handled, new_length);
								new_content[m_size + new_length] = '\0';

								if(m_content)
									free(m_content);
								m_content = new_content;

								handled += new_length;
								m_chunkread += new_length;
								m_size += new_length;

								// If we've already read the whole chunk size, set m_chunksize to zero to make it reset
								if(m_chunkread >= m_chunksize) {
									m_chunksize = 0;
									handled++;
								}
							}
						}
					}
					else if(m_contentlength) {
						if(handled < count) {
							// Copy the rest into the content
							int new_length = count - handled;
							char* new_content = (char*)malloc(m_size + new_length + 1);
							if(m_content)
								strncpy(new_content, m_content, m_size);

							// Write it out to file as well
							if(!m_robots && m_filename)
								fwrite(buffer + handled, new_length, 1, m_fp);

							strncpy(new_content + m_size, buffer + handled, new_length);
							new_content[m_size + new_length] = '\0';
							if(m_content)
								free(m_content);
							m_content = new_content;

							m_size += new_length;

							if(m_size >= m_contentlength) end_of_file = true;
						}
					}
					else {
						// We just have to wait for the connection to close (BOOOOO), read in what we have
						if(handled < count) {
							// Copy the rest into the content
                                                        int new_length = count - handled;
                                                        char* new_content = (char*)malloc(m_size + new_length + 1);
                                                        if(m_content)
                                                                strncpy(new_content, m_content, m_size);

                                                        // Write it out to file as well
                                                        if(!m_robots && m_filename)
                                                                fwrite(buffer + handled, new_length, 1, m_fp);

                                                        strncpy(new_content + m_size, buffer + handled, new_length);
                                                        if(m_content)
                                                                free(m_content);
                                                        m_content = new_content;

                                                        m_size += new_length;
						}
					}
	        	        }

				// If we're done, finish up
				if(end_of_file) {
					free(buffer);
					m_state = HTTPREQUESTSTATE_WRITE;
					break;
				}

				if(m_size > MAX_PAGE_SIZE) {
					free(buffer);
					this->error("Exceed max page size.");
					return(false);
				}

	                	free(buffer);
			}
	        }
		else {
			// Wait for a max multiple of the connect time
			int wait_time = m_connecttime * 5;
			m_lastcheck = time(NULL);
			if((m_lastcheck - m_lasttime) > wait_time) {
				if(!m_chunked && !m_contentlength) {
					// Just mark it as complete
					m_state = HTTPREQUESTSTATE_WRITE;
				}
				else {
					this->error("RECV TIMEOUT");
					return(false);
				}
			}
		}
	}

	if(m_state == HTTPREQUESTSTATE_WRITE) {
		// First process the HTTP response headers
		m_lasttime = time(NULL);

		Url* url = (m_robots == NULL) ? m_url : m_robots;

		if(!m_content) {
			printf("No content returned for %s\n", url->get_url());
			m_state = HTTPREQUESTSTATE_COMPLETE;
			return(true);
		}

		// If this is a robots.txt request, mark it as "to be processed"
		if(m_robots != 0) {
			m_state = HTTPREQUESTSTATE_ROBOTS;

			delete m_robots;
			m_robots = 0;
			return(true);
		}

		if(m_code != 200) {
                        // Nothing too do here, mark as complete and move on
                        m_state = HTTPREQUESTSTATE_COMPLETE;
			printf("Invalid code %d for %s\n", m_code, url->get_url());
                        return(true);
                }

		// If we get here, it means we need to (optionally) dump it out to a file if there's on set, otherwise, complete
                if(!m_filename) {
			printf("No file name specified for %s, returning.\n", url->get_url());
                        m_state = HTTPREQUESTSTATE_COMPLETE;
                        return(true);
                }

		printf("Wrote %s to %s (%d)\n", url->get_url(), m_filename, m_size);

		m_state = HTTPREQUESTSTATE_COMPLETE;
		return(true);
	}

	if(m_state == HTTPREQUESTSTATE_ERROR) {
                return(false);
        }

	return(true);
}
