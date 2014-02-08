
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <openssl/md5.h>
#include <ctype.h>
#include <url.h>

regex_t* Url::m_regex = 0;

Url::Url(char* url) {
	// Initialize
	m_parts[0] = m_parts[1] = m_parts[2] = m_parts[3] = 0;
	m_hash = 0;
	m_domain_id = 0;
	m_next = 0;

	// Save a copy of our URL
        m_url = (char*)malloc(strlen(url) + 1);
	strcpy(m_url, url);
}

Url::~Url() {
	if(m_url) {
		free(m_url);
		m_url = 0;
	}

	if(m_hash) {
		free(m_hash);
		m_hash = 0;
	}

	for(int i = 0; i < 4; i++) {
		if(m_parts[i]) {
			free(m_parts[i]);
			m_parts[i] = 0;
		}
	}
}

regex_t* Url::_get_regex() {
	if(!m_regex) {
		// Create a new compiled regex
         	m_regex = new regex_t;

         	// Compile our regular expression
         	if(regcomp(m_regex, "([a-z]+)://([a-z0-9\\.-]+)(/*[^\\?]*)\\?*([^#]*)#*.*", REG_EXTENDED | REG_ICASE) != 0) {
         		printf("Cannot compile URL regex.\n");
                	return(NULL);
        	}
	}

	return(m_regex);
}

bool Url::parse(Url* base) {
	// Check if this URL is already absolute
	if(strncmp(m_url, "http", 4) == 0) {
		return(_split());
	}

	// Check for mailto and javascript links
	if((strncmp(m_url, "mailto:", 7) == 0) || (strncmp(m_url, "javascript:", 11) == 0)) {
		return(false);
	}

	if(!base) {
		return(false);
	}

	// Check for a URL relative to the site root
        if(strncmp(m_url, "/", 1) == 0) {
                // Initialize a temporary URL to put it all together
                unsigned int copy_length = strlen(base->get_scheme()) + 3 + strlen(base->get_host());
                unsigned int length = copy_length + strlen(m_url);
                char* complete = (char*)malloc(length + 1);

                memcpy(complete, base->get_url(), copy_length);
                memcpy(complete + copy_length, m_url, strlen(m_url));
                complete[length] = '\0';

                // Free the current URL and replace it with the complete URL for splitting
                free(m_url);
                m_url = complete;

                return(_split());
        }
	
	// Strip out any "current directory" starting part of the URL (./)
	if(strncmp(m_url, "./", 2) == 0) {
		unsigned int length = strlen(m_url) - 2;
		char* temp = (char*)malloc(length + 1);
		strcpy(temp, m_url + 2);

		// Free the URL and copy
		free(m_url);
		m_url = temp;
	}

	// Prepend the path part of the base URL onto this URL
	unsigned int copy_length = strlen(base->get_path()) - 1;
	unsigned int url_length = strlen(m_url);

	char* temp = (char*)malloc(url_length + copy_length);

	// we might not have a copy length, since we aren't taking the first forward slash on the base's path (ie. domain.com/)
	if(copy_length)
		strncpy(temp, base->get_path() + 1, copy_length);
	strcpy(temp + copy_length, m_url);

	free(m_url);
	m_url = temp;

	// Break the URL into path m_parts, dividing along each forward slash (/); start by finding the numbers of m_parts
	char* pointer = m_url;

	char* position = 0;
	unsigned int count = 1;
	while((position = strchr(pointer, '/')) != 0) {
		count++;
		pointer = position + 1;
	}

	// Now that we know how many m_parts, separate
	pointer = m_url;
	char** path_parts = (char**)malloc(count * sizeof(char*));

	// Loop over each part and copy it in
	for(unsigned int i = 0; i < count; i++) {
		char* found = strchr(pointer, '/');
		int length = found ? found - pointer : strlen(pointer);

		char* part = (char*)malloc(length + 1);
		memcpy(part, pointer, length);
		part[length] = '\0';

		path_parts[i] = part;

		// Increment pointer
		pointer = pointer + (length + 1);
	}

	// Iterate over the m_parts, getting rid of any directories that need to be recursed (../, ./)
	char* final_path =  (char*)malloc(strlen(m_url) + 2);
	final_path[strlen(m_url) + 1] = '\0';

	// Start from the end of the string
	int offset = strlen(m_url) + 1;

	// This code is a little bit backwards; to be efficient, we now recurse backwards over the path m_parts, prepending them to the final path we already have
	for(int i = count - 1; i >= 0; i--) {
		// If this part is recursed, just move on, skipping the next part as well
		if(strcmp(path_parts[i], "..") == 0) {
			i--;
			continue;
		}

		if(strcmp(path_parts[i], ".") == 0) {
			continue;
		}

		// Otherwise, add it on to the final path, backwards
		unsigned int part_length = strlen(path_parts[i]);
		offset -= part_length;
		strncpy(final_path + offset, path_parts[i], part_length);

		offset--;
		final_path[offset] = '/';
	}

	// Clean up the path m_parts
	for(unsigned int i = 0; i < count; i++) {
		free(path_parts[i]);
	}
	free(path_parts);

	// Free the URL as it is now and re-create it
	unsigned int final_path_length = strlen(m_url) - offset;
	unsigned int base_copy_length = strlen(base->get_scheme()) + 3 + strlen(base->get_host());

	free(m_url);

	m_url = (char*)malloc(base_copy_length + final_path_length + 1);
	strncpy(m_url, base->get_url(), base_copy_length);
	strcpy(m_url + base_copy_length, final_path + offset);

	// Free the final path temp
	free(final_path);

	return(_split());
}

bool Url::_split() {
        // Get our regular expression
        regex_t* regex = Url::_get_regex();
	if(!regex) {
		printf("REGEX COMPILE ERROR\n");
		return(false);
	}

        // Parse the URL
        regmatch_t re_matches[regex->re_nsub + 1];

        // Execute our regex
	int error = 0;
        if((error = regexec(regex, m_url, regex->re_nsub + 1, re_matches, 0)) != 0) {
		char* errstr = (char*)malloc(1000);
		unsigned int length = regerror(error, regex, errstr, 1000);
		errstr[length] = '\0';

		printf("%s: %s (%d)\n", m_url, errstr, error);
		free(errstr);

                return(false);
	}

	// Loop through the expected number of matches
        for(unsigned int i = 1; i <= m_regex->re_nsub; i++) {
		unsigned int index = i - 1;
		m_parts[index] = (char*)malloc(re_matches[i].rm_eo - re_matches[i].rm_so + 1);
		memcpy(m_parts[index], m_url + re_matches[i].rm_so, re_matches[i].rm_eo - re_matches[i].rm_so);
		m_parts[index][re_matches[i].rm_eo - re_matches[i].rm_so] = '\0';
	}

	// Convert the scheme and domain to lowercase
	for(unsigned int i = 0; i < strlen(m_parts[URL_SCHEME]); i++)
		m_parts[URL_SCHEME][i] = tolower(m_parts[URL_SCHEME][i]);

	for(unsigned int i = 0; i < strlen(m_parts[URL_DOMAIN]); i++)
		m_parts[URL_DOMAIN][i] = tolower(m_parts[URL_DOMAIN][i]);

	// Get the MD5 m_hash of the path and query
	unsigned int length = strlen(m_parts[URL_DOMAIN]) + strlen(m_parts[URL_PATH]);
	if(strlen(m_parts[URL_QUERY])) {
		length += 1 + strlen(m_parts[URL_QUERY]);
	}

	char* encrypt = (char*)malloc(length + 1);
	if(strlen(m_parts[URL_QUERY]))
		sprintf(encrypt, "%s%s?%s", m_parts[URL_DOMAIN], m_parts[URL_PATH], m_parts[URL_QUERY]);
	else
		sprintf(encrypt, "%s%s", m_parts[URL_DOMAIN], m_parts[URL_PATH]);

        unsigned char temp[MD5_DIGEST_LENGTH];
        memset(temp, 0, MD5_DIGEST_LENGTH);
        MD5((const unsigned char*)encrypt, length, temp);

	free(encrypt);

        // Convert it to hex
        m_hash = (char*)malloc((MD5_DIGEST_LENGTH * 2) + 1);
        for(unsigned int k = 0; k < MD5_DIGEST_LENGTH; k++) {
                sprintf(m_hash + (k * 2), "%02x", temp[k]);
        }
        m_hash[MD5_DIGEST_LENGTH * 2] = '\0';

        return(true);
}

