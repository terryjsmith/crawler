
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <ctype.h>
#include <url.h>
#include <parser.h>

regex_t* Parser::m_regex = 0;

Parser::Parser() {
}

Parser::~Parser() {
}

regex_t* Parser::_get_regex() {
        if(!m_regex) {
                // Create a new compiled regex
                m_regex = new regex_t;

                // Compile our regular expression
                if(regcomp(m_regex, "href=['\"]?([^ '\"]+)['\"]?", REG_EXTENDED | REG_ICASE) != 0) {
                        printf("Cannot compile URL regex.\n");
                        return(NULL);
                }
        }

        return(m_regex);
}

Url* Parser::parse(char* html) {
	// Get our regular expression
        regex_t* regex = Parser::_get_regex();
        if(!regex) {
                printf("REGEX COMPILE ERROR\n");
                return(0);
        }

	// Grab a copy of the string we can manipulate
	char* page = (char*)malloc(strlen(html) + 1);
	strcpy(page, html);

	// Then create a pointer so we can move along the string
	char* pointer = page;

	Url* start = 0;
	while(true) {
        	// Parse the URL
        	regmatch_t re_matches[regex->re_nsub + 1];

        	// Execute our regex
        	if(regexec(regex, pointer, regex->re_nsub + 1, re_matches, 0) != 0) {
			break;
        	}

		char* url = (char*)malloc(re_matches[1].rm_eo - re_matches[1].rm_so + 1);
		memcpy(url, pointer + re_matches[1].rm_so, re_matches[1].rm_eo - re_matches[1].rm_so);
		url[re_matches[1].rm_eo - re_matches[1].rm_so] = '\0';

		printf("Found URL %s\n", url);	
		if(start == 0)
			start = new Url(url);
		else {
			Url* current = start;
			while(current->get_next() != 0)
				current = current->get_next();
			Url* u = new Url(url);
			current->set_next(u);
		}

		free(url);
		pointer += re_matches[1].rm_eo;
	}

	// Get rid of our copy of page
	free(page);
	return(start);
}
