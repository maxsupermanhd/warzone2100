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
						errlog("MH info Room admin set to %s.\n", newAdminStrCopy.c_str());
						stdinSetAdmin(newAdminStrCopy);
						auto roomAdminMessage = astringf("Room admin assigned to: %s", newAdminStrCopy.c_str());
						sendRoomSystemMessage(roomAdminMessage.c_str());
					});
				}
			} else if(!strncmpl(line, "ban ip ")) {
				char tobanip[1024] = {0};
				int r = sscanf(line, "ban ip %1023[^\n]s", tobanip);
				if(r != 1) {
					errlog("MH info Failed to get ban ip!!!!\n");
				} else {
					std::string banIPStrCopy(tobanip);
					wzAsyncExecOnMainThread([banIPStrCopy] {
						for(int i=0; i<MAX_PLAYERS; i++) {
							auto player = NetPlay.players[i];
							if(!strcmp(player.IPtextAddress, banIPStrCopy.c_str())) {
								kickPlayer(i, "You have been banned from joining Autohoster.", ERROR_INVALID);
							}
							auto KickMessage = astringf("Player %s tried to join Autohoster but failed.", player.name);
							sendRoomSystemMessage(KickMessage.c_str());
						}
					});
				}
			} else if(!strncmpl(line, "chat bcast ")) {
				char chatmsg[1024] = {0};
				int r = sscanf(line, "chat bcast %1023[^\n]s", chatmsg);
				if(r != 1) {
					errlog("MH info Failed to get bcast message!!!!\n");
				} else {
					std::string chatmsgstr(chatmsg);
					wzAsyncExecOnMainThread([chatmsgstr] {
						sendRoomSystemMessage(chatmsgstr.c_str());
					});
				}
			} else if(!strncmpl(line, "shutdown now")) {
				exit(0);
			}
		}
		free(line);
	}
	return 0;
}
