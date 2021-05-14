#include "stdinreader.h"

#include "multiint.h"

WZ_THREAD* stdinThread = NULL;
std::string MHRoomAdminHashFull = "";

int stdinThreadFunc(void *) {
	fflush(stdin);
	fseek(stdin,0,SEEK_END);
	errlog("MH stdinReadReady\n");
	bool inexit = false;
	while(!inexit) {
		char *line = NULL;
		size_t len = 0;
		ssize_t lineSize = 0;
		lineSize = getline(&line, &len, stdin);
		if(lineSize == -1) {
			errlog("MH error Getline failed!\n");
			inexit = true;
		} else {
			if(!strncmpl(line, "exit")) {
				inexit = true;
			} else if(!strncmpl(line, "admin set ")) {
				char newadmin[1024] = {0};
				int r = sscanf(line, "admin set %1023s", newadmin);
				if(r != 1) {
					errlog("MH info Failed to set room admin hash!!!!\n");
				} else {
					errlog("MH info Room admin set to %s.\n", newadmin);
					std::string passnewadmin = newadmin;
					wzAsyncExecOnMainThread([passnewadmin] {
						MHRoomAdminHashFull = passnewadmin;
						sendRoomSystemMessage(("Room admin assigned to: "+passnewadmin).c_str());
					});
				}
			}
		}
		free(line);
	}
	return 0;
}
