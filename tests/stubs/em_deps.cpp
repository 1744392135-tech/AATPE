#include <string>
#include <cstdio>
int g_cpuCount = 8;
bool writeFile(const char* path, const std::string& content){FILE*f=fopen(path,"w"); if(!f)return false; fwrite(content.data(),1,content.size(),f); fclose(f); return true;}
