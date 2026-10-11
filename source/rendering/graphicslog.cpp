#include "pch.h"
#include "graphicslog.h"

void WriteGraphicsLog(const char* format, ...)
{
	FILE* fp = nullptr;
	fopen_s(&fp, "init_log.txt", "a");
	if (!fp)
	{
		return;
	}
	va_list arguments;
	va_start(arguments, format);
	vfprintf(fp, format, arguments);
	va_end(arguments);
	fclose(fp);
}
