
#ifndef __WORKER_H__
#define __WORKER_H__

class Worker {
public:
	Worker();
	~Worker();

	// Start the thread
	int start(int pos);

	// Run
	void run();

	// Fill our list of URLs with ones we can use and go fetch (run all checks and stuff)
	void fill_list();

	// Get the domain ID for a string domain
	Domain* load_domain_info(char* strdomain);

	// Check if a URL already exists in our database
	bool url_exists(Url* url, Domain* info);

	// Check the robots rules in the database
	bool check_robots_rules(Url* url);

	// Check that redis is connected, reconnect if not
	void check_redis_connection();

protected:
	// Our connections to our databases
	MYSQL* m_conn;
	redisContext* m_context;

	// Our list of requests we are currently processing
	HttpRequest** m_requests;
	int m_active;

	// Our instance of epoll
	int m_epoll;

	// Our process ID
	int m_procid;
};

#endif
