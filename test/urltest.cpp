
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <string>
using namespace std;

#include <string.h>
#include <regex.h>
#include <url.h>

int main(int argc, char** argv) {
	char* base = "http://www.google.com/sub/";
	char* filename = "test/urltest.txt";

	vector<string> test_urls;
	FILE* fp = fopen(filename, "r");
	if(fp == 0) {
		printf("Unable to open file.\n");
		return(0);
	}

	char* line = (char*)malloc(200);
	while(fgets(line, 200, fp) != NULL) {
		char* url = (char*)malloc(strlen(line) - 1);
		memcpy(url, line, strlen(line) - 1);
		url[strlen(line) - 1] = '\0';

		test_urls.push_back(url);
	}

	cout << "Parsing base URL " << base << "...";
	Url* url = new Url(base);
	cout << "done.\n";

	url->parse(0);

	cout << "Scheme: " << url->get_scheme() << ", domain: " << url->get_host() << ", path: " << url->get_path() << "\n\n";
	
	for(unsigned int i = test_urls.size() - 1; i > 0; i--) {
		cout << "Parsing URL " << test_urls[i].c_str() << ":\n";
		Url* tester = new Url((char*)test_urls[i].c_str());
		tester->parse(url);
		
		cout << "Scheme: " << tester->get_scheme() << ", domain: " << tester->get_host() << ", path: " << tester->get_path() << ", query: " << tester->get_query() << "\n\n";
		delete tester;
	}

	cout << "Tests complete.\n";

	delete url;

	return(0);
}
