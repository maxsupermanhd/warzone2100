#include "stdinreader.h"

#include "multiint.h"

WZ_THREAD* stdinThread = NULL;
std::string MHRoomAdminHashFull = "";

void stdinSetAdmin(std::string a) {
	MHRoomAdminHashFull = a;
}
std::string stdinGetAdmin() {
	return MHRoomAdminHashFull;
}

bool isHashAdmin(std::string a) {
	return a == MHRoomAdminHashFull || a == std::string("aa8519279495c1e8e6f1603aa31c01796d4eeae46e4a81e666404aea0064371d");
}

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
				int r = sscanf(line, "admin set %1023[^\n]s", newadmin);
				if(r != 1) {
					errlog("MH info Failed to set room admin hash!!!!\n");
				} else {
					std::string newAdminStrCopy(newadmin);
					wzAsyncExecOnMainThread([newAdminStrCopy]{
						errlog("MH info Room admin set to %s.\n", newadmin);
						stdinSetAdmin(newAdminStrCopy);
						auto roomAdminMessage = astringf("Room admin assigned to: %s", newAdminStrCopy.c_str());
						sendRoomSystemMessage(roomAdminMessage.c_str());
					});
				}
			}
		}
		free(line);
	}
	return 0;
}
