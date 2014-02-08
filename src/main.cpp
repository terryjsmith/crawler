
/* INCLUDES */

#include <vector>
using namespace std;

#include <sys/stat.h>
#include <sys/wait.h>
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
#include <ares.h>

#include <defines.h>
#include <domain.h>
#include <url.h>
#include <httprequest.h>
#include <worker.h>

/* FUNCTIONS */

int main(int argc, char** argv) {
	// Multiple connections to MySQL requires this first
	mysql_library_init(0, 0, 0);

	// Initialize our async DNS lookup library
	ares_library_init(ARES_LIB_INIT_ALL);

	// Initialize a pool of workers
        Worker** workers = (Worker**)malloc(sizeof(Worker*) * NUM_THREADS);

	// Keep track of the PIDs
        int* pids = (int*)malloc(NUM_THREADS * sizeof(int));
        memset(pids, 0, NUM_THREADS * sizeof(int));

        // Initialize them
        for(unsigned int i = 0; i < NUM_THREADS; i++) {
                workers[i] = new Worker;
                pids[i] = workers[i]->start(i);

		char* filename = (char*)malloc(100);
                sprintf(filename, "/var/run/fetcher%d.pid", i);

		FILE* fp = fopen(filename, "w");
		fprintf(fp, "%d", pids[i]);
		fclose(fp);

		free(filename);
        }

        // Enter the main loop
        while(true) {
                // Check if the PID is still running
                for(int i = 0; i < NUM_THREADS; i++) {
			int status = 0;
			if(waitpid(pids[i], &status, WNOHANG)) {
                                // It died, clean up and re-spawn
                                delete workers[i];
                                workers[i] = 0;

                                workers[i] = new Worker;
                                pids[i] = workers[i]->start(i);

				char* filename = (char*)malloc(100);
		                sprintf(filename, "/var/run/fetcher%d.pid", i);

                		FILE* fp = fopen(filename, "w");
		                fprintf(fp, "%d", pids[i]);
                		fclose(fp);

		                free(filename);
			}
                }

                sleep(1);
        }

	// Shut it down
	ares_library_cleanup();

	return(0);
}
