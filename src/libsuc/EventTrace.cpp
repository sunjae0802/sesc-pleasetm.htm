#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "EventTrace.h"

FILE *EventTrace::fd = 0;

void EventTrace::openFile(const char *name)
{
    fd = fopen(name, "a");

    if(fd == 0) {
        fprintf(stderr, "NANASSERT::EVENTTRACE could not open trace file [%s]\n", name);
        exit(-3);
    }
}

void EventTrace::close()
{
    if(fd) {
        fclose(fd);
    }
}

void EventTrace::add(const char *format, ...)
{
    if(fd) {
        va_list ap;

        va_start(ap, format);
        vfprintf(fd, format, ap);
        va_end(ap);

        fprintf(fd, "\n");
    }
}

void EventTrace::flush()
{
    if(fd) {
        fflush(fd);
    }
}


