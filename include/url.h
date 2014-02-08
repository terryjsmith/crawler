
#ifndef __URL_H__
#define __URL_H__

#define URL_SCHEME	0
#define URL_DOMAIN	1
#define URL_PATH	2
#define URL_QUERY	3

class Url {
public:
	Url(char* url);
	~Url();

	// Parse the URL, relative to the base URL
	bool parse(Url* base);

	// Clone this URL into a new one
	Url* clone();

	// Getters
	char* get_url() { return m_url; }
	char* get_scheme() { return m_parts[URL_SCHEME]; }
	char* get_host() { return m_parts[URL_DOMAIN]; }
	char* get_path() { return m_parts[URL_PATH]; }
	char* get_path_hash() { return m_hash; }
	char* get_query() { return m_parts[URL_QUERY]; }

	void set_domain_id(unsigned int long domain_id) { m_domain_id = domain_id; }
	unsigned int long get_domain_id() { return m_domain_id; }

	// Get the regular expression we use to parse URLs
        static regex_t* _get_regex();

	// Linked list stuff
	Url* get_next() { return m_next; }
	void set_next(Url* next) { m_next = next; }

protected:
	// The URL parts, in order of the defines above
	char* m_parts[4];

	// An internal copy of the full URL
	char* m_url;

	// An MD5 hash of the whole URL (domain + path)
	char* m_hash;

	// The domain ID of the URL if it's known
	unsigned int long m_domain_id;

	// Split this URL into it's parts
	bool _split();

	// The global URL parsing regex
	static regex_t* m_regex;

	// Linked list stuff
	Url* m_next;
};

#endif
