
#ifndef __parser_h__
#define __parser_h__

// A class to parse out the links from a web page
class Parser {
public:
	Parser();
	~Parser();

	// Parse some passed in HTML for URLs (returns a linked list of Urls)
	Url* parse(char* html);

	// Get the regular expression we use to parse URLs
        static regex_t* _get_regex();

protected:
	// The global URL parsing regex
        static regex_t* m_regex;
};

#endif
