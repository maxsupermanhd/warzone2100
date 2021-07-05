#ifndef __STDINREADER_H_INCLUDED__
#define __STDINREADER_H_INCLUDED__

#include "lib/framework/wzapp.h"

#include <string>

extern WZ_THREAD* stdinThread;
extern std::string MHRoomAdminHashFull;

#define strncmpl(a, b) strncmp(a, b, strlen(b))
#define errlog(...) do{fprintf(stderr, ##__VA_ARGS__);fflush(stderr);}while(0);

void stdinSetAdmin(std::string a);
std::string stdinGetAdmin();

bool isHashAdmin(std::string a);

int stdinThreadFunc(void *);


void stdinLoadBlocklist();
bool CheckIPblocked(const char* ip);

#endif