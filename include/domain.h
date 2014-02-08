
#ifndef __DOMAIN_H__
#define __DOMAIN_H__

class Domain {
public:
	Domain();
	~Domain();

public:
        int domain_id;
        char* domain;
        unsigned long int last_access;
        unsigned long int robots_last_access;
};



#endif
