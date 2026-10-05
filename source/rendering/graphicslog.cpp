#include "pch.h"
#include "graphicslog.h"

void WriteGraphicsLog(const char* msg)
{
	FILE* fp = nullptr;
	fopen_s(&fp, "init_log.txt", "a");
	if (fp) { fprintf(fp, "%s", msg); fclose(fp); }
}
