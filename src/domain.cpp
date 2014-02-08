
#include <stdlib.h>
#include <domain.h>

Domain::Domain() {
	domain_id = 0;
	domain = 0;
	last_access = robots_last_access = 0;
}

Domain::~Domain() {
	if(domain) {
		free(domain);
		domain = 0;
	}
}
