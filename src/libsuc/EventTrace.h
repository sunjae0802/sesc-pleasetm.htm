#ifndef EVENTTRACE_H
#define EVENTTRACE_H

class EventTrace {
private:
    static FILE *fd;

public:
    static void openFile(const char *name);
    static void add(const char *format, ...);
    static void close();
    static void flush();
};

#endif // EVENTTRACE_H
