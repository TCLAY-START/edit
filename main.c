#include<stdio.h>
#include<errno.h>
#include<unistd.h>
#include<stdlib.h>
#include<termios.h>
#include<ctype.h>
#include<sys/ioctl.h>
#include<string.h>
#include<time.h>
#include<stdarg.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>

#define FIERS 0

#define KILO_TAB_STOP 8

#define CTRL_KEY(k) ( (k) & 0x1f )

#define KILO_QUIT_TIMES 3

enum editorKey{
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY
};

typedef struct erow{
    int size;
    int rsize;
    char *chars;
    char *render;
}erow;

struct editorConfig{
    struct termios orig_termios;
    int screenrows;
    int screencols;
    int cx;
    int cy;
    int rx;
    int numrows;
    int colsoff;
    int rowoff;
    int dirty;
    char *filename;
    char statusmsg[88];
    time_t statusmsg_time;
    erow *rows;
};

struct editorConfig E;

struct abuf{
    char *str;
    int len;
}buf = {NULL, 0};

void editorUpdateRow(erow *);
void editorInsertRow(int, char *, size_t);
char *editorPrompt(char *, void (*callback)(char *, char));
void editorFind();

void editorSetStatusMessage(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

void abAppend(struct abuf *ab, const char *s, int len){
    char *new = (char *)realloc(ab->str, ab->len + len);
    if(new == NULL)
        return;
    memcpy(new + ab->len, s, len);
    ab->str = new;
    ab->len +=len;
}

void abFree(struct abuf *ab){
    free(ab->str);
    ab->str = NULL;
    ab->len = 0;
}


void die(const char *s){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode(){
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("disableRawMOde function tcsetattr()");
}

void enableRawMode(){
    struct termios raw;

    if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("enableRawMode function tcgetattr()");

    atexit(disableRawMode);

    raw = E.orig_termios;

    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("enableRawMode function tcsetattr()");
}

void editorRowInsertChar(erow *rows, int at, int c){
    if(at < 0 || at > rows->size)
        at = rows->size;
    rows->chars = realloc(rows->chars, rows->size + 2);
    memmove(rows->chars + (at + 1), rows->chars + at, rows->size - at + 1);
    rows->size++;
    rows->chars[at] = c;
    editorUpdateRow(rows);
}

void editorInsertChar(int c){
    if(E.cy >= E.numrows)
        return;
    
    editorRowInsertChar(E.rows + E.cy, E.cx, c);
    E.cx++;
    E.dirty++;
}

char *editorRowsToString(int *buflen){
    int len = 0;
    int i;
    for(i = 0; i < E.numrows; i++){
        len += E.rows[i].size + 1;
    }

    *buflen = len;

    char *buf = (char*)malloc(len);
    char *tmp = buf;
    for(i = 0; i < E.numrows; i++){
        memcpy(tmp, E.rows[i].chars, E.rows[i].size);
        tmp += E.rows[i].size;
        *tmp = '\n';
        tmp++;
    } 

    return buf;
}

void editorSave(){
    if(E.filename == NULL){
        E.filename = editorPrompt("Save as: %s", NULL);
        if(E.filename == NULL)
            return;
    }
    
    int len = 0;
    int fd = open(E.filename, O_RDWR | O_CREAT, 0664);
    char *buf = editorRowsToString(&len);

    if(fd != -1){
        if(ftruncate(fd, len) != -1){
            if(write(fd, buf, len) == len){
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk",len);
                return;
            }
        }
    }

    close(fd);
    free(buf);
    editorSetStatusMessage("Can't save I/O error: %s",strerror(errno));
}

void editorFreeRow(erow *rows){
    free(rows->chars);
    free(rows->render);
}

void editorDelRow(int at){
    editorFreeRow(E.rows + at);
    memmove(E.rows + at, E.rows + at + 1,sizeof(erow)*(E.numrows - at));
    E.numrows--;
}

void editorRowAppendString(erow *rows, char *s, size_t len){
    rows->chars = realloc(rows->chars, rows->size + len + 1);
    memmove(rows->chars + rows->size, s, len);
    rows->size += len;
    rows->chars[rows->size] = '\0';
    editorUpdateRow(rows);
}

void editorRowDelChar(erow *rows, int at){
    if(at < 0 || at >= rows->size)
        return;
    memmove(rows->chars + at, rows->chars + at + 1, rows->size - at);
    rows->size--;
    editorUpdateRow(rows);
}

void editorDelChar(){
    if(E.cy == E.numrows)
        return;
    if(E.cx == 0 && E.cy == 0)
        return;
        
    if(E.cx > 0){
        editorRowDelChar(E.rows + E.cy, E.cx - 1);
        E.cx--;
    }else{
        erow *rows = E.rows + E.cy;
        E.cx = E.rows[E.cy - 1].size;
        editorRowAppendString(E.rows + E.cy - 1, rows->chars, rows->size);
        editorDelRow(E.cy);
        E.cy--;
    }
    E.dirty++;
}

void editorInsertNewline(){
    if(E.cx == 0){
        editorInsertRow(E.cy, "", 0);
    }else{
        editorInsertRow(E.cy, E.rows[E.cy].chars, E.cx);
        erow *rows = E.rows + E.cy + 1;
        int len = rows->size - E.cx;
        memmove(rows->chars, rows->chars + E.cx, len);
        rows->size = len;
        editorUpdateRow(rows);
        E.cx = 0;
    }
    E.cy++;
    E.dirty++;
}

void editorMoveCursor(int key){
    erow *row = (E.cy >= E.numrows) ? NULL : E.rows + E.cy;
    switch(key){
        case ARROW_LEFT:
            if(E.cx != 0)
                E.cx--;
            else if(E.cy != 0){
                E.cy--;
                E.cx = E.rows[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            //if(E.cx < E.screencols)
            if(row && E.cx < row->size)
                E.cx++;
            else if(row){
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if(E.cy != 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if(E.cy < E.numrows)
                E.cy++;
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : E.rows + E.cy;
    int len = row ? row->size : 0;
    if(E.cx > len)
        E.cx = len;
}

int editorReadKey(){
    int nread;
    char c;

    while( (nread = read(STDIN_FILENO, &c, 1)) != 1){
        if(nread == -1 && errno == EAGAIN)
            die("editorReadKey function read()");
    }

    if(c == '\x1b'){
        char ch[2];

        if(read(STDIN_FILENO, ch, 1) != 1)
            return '\x1b';
        if(read(STDIN_FILENO, ch + 1, 1) != 1)
            return '\x1b';

        if(ch[0] == '['){
            switch (ch[1]){
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
            }
        }

        return '\x1b';
    }

    return c;
}

void editorProcessKeypress(){
    int c = editorReadKey();
    static int quit_times = KILO_QUIT_TIMES;
#if FIERS
    if(iscntrl(c)){
        printf("fiers is %d\r\n", c);
    }else{
        printf("fiers is %d, ('%c')\r\n"c,c);
    }
#else

    switch(c){
        case '\r':
            editorInsertNewline();
            break;

        case CTRL_KEY('q'):
            if(E.dirty && quit_times > 0){
                editorSetStatusMessage("WARNTNG!!! File has unsaved changes. Press Ctrl-Q %d more times to quit.",quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            editorDelChar();
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        default:
            editorInsertChar(c);
    }

    quit_times = KILO_QUIT_TIMES;
#endif
}

void editorDrawRows(struct abuf *ab){
    for(int y = 0; y < E.screenrows; y++){

        int rowoff = y + E.rowoff;
        if(rowoff >= E.numrows){
            if(E.numrows == 0 && y == E.screenrows / 3){
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "kilo editor --version %s", "0.0.1");
                if(welcomelen > E.screencols)
                    welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if(padding){
                    abAppend(ab, "~", 1);
                    padding--;
                }

                while(padding){
                    abAppend(ab, " ", 1);
                    padding--;
                }
                abAppend(ab, welcome, welcomelen);
            }else{
                abAppend(ab, "~", 1);
            }
            
        }else{
            int len = E.rows[rowoff].rsize - E.colsoff;
            if(len < 0)
                len = 0;
            if(len > E.screencols)
                len = E.screencols;
            abAppend(ab, E.rows[rowoff].render + E.colsoff, len);
        }

        abAppend(ab, "\x1b[K", 3);

        abAppend(ab, "\r\n", 2);
    }
}

int editorRowCxToRx(erow *row, int cx){
    int rx = 0;
    for(int i = 0; i < cx; i++){
        if(row->chars[i] == 9){
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        rx++;
    }

    return rx;
}

void editorScroll(){
    E.rx = 0; 

    if(E.cy < E.numrows){
        E.rx = editorRowCxToRx(E.rows + E.cy, E.cx);
    }
    if(E.cy < E.rowoff){
        E.rowoff = E.cy;
    }

    if(E.cy >= E.rowoff + E.screenrows){
        E.rowoff = E.cy - E.screenrows + 1;
    }

    if(E.rx < E.colsoff){
        E.colsoff = E.rx;
    }

    if(E.rx > E.colsoff + E.screencols){
        E.colsoff = E.rx - E.screencols + 1;
    }
}

void editorDrawStatusBar(struct abuf *ab){
    abAppend(ab, "\x1b[7m",4);
    char buf[88] = {0};
    int len = snprintf(buf, sizeof(buf),"%.20s - %d lins", E.filename?E.filename : "[NO Name]", E.numrows);
    char rbuf[88] = {0};
    int rlen = snprintf(rbuf, sizeof(rbuf), "%d/%d", E.cy + 1, E.numrows);
    if(len > E.screencols)
        len = E.screencols;
    abAppend(ab,buf, len);
    for(int i = len; i < E.screencols; i++){
        if(E.screencols - i == rlen){
            abAppend(ab, rbuf, rlen);
            break;
        }else{
            abAppend(ab, " ", 1);
        }
    }

    abAppend(ab, "\x1b[m",3);
    abAppend(ab, "\r\n", 2);
}

void editorRrawMessageBar(struct abuf *ab){
    abAppend(ab, "\x1b[K",3);
    int msglen = strlen(E.statusmsg);
    if(msglen > E.screencols)
        msglen = E.screencols;
    if(msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen(){

    editorScroll();

    abAppend(&buf, "\x1b[?25l", 6);
    abAppend(&buf, "\x1b[H", 3);

    editorDrawRows(&buf);
    editorDrawStatusBar(&buf);
    editorRrawMessageBar(&buf);

    char bf[32];
    snprintf(bf, sizeof(bf), "\x1b[%d;%dH", E.cy - E.rowoff + 1, E.cx - E.colsoff + 1);
    abAppend(&buf, bf, strlen(bf));

    abAppend(&buf, "\x1b[?25h", 6);
    write(STDOUT_FILENO, buf.str, buf.len);

    abFree(&buf);
}

int getCursorPostiton(int *rows, int *cols){
    char buf[32];
    int i = 0;

    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while(i < sizeof(buf) - 1){
        if(read(STDIN_FILENO, buf + i, 1) != 1)
            break;
        if(buf[i] == 'R')
            break;

        i++;
    }

    buf[i] = '\0';

    if(buf[0] != '\x1b' || buf[1] != '[')
        return -1;

    if(sscanf(buf + 2,"%d;%d",rows, cols) == 2)
        return 0;
    return -1;
}

int getWindowSize(int *rows, int *cols){
    struct winsize ws;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1){
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPostiton(rows, cols);
    }else{
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    }

    return 0;
}

void editorUpdateRow(erow *row){
    int tabs = 0;
    int i;
    for(i = 0; i < row->size; i++){
        if(row->chars[i] == 9)
            tabs++;
    }

    char *ran = (char *)malloc(row->size + tabs *(KILO_TAB_STOP - 1) + 1);
    if(!ran){
        die("editorUpdateRow function malloc()");
    }

    int idx = 0;
    for(i = 0; i < row->size; i++){
        if(row->chars[i] == 9){
            ran[idx++] = ' ';
            while(idx % KILO_TAB_STOP != 0){
                ran[idx++] = ' ';
            }
        }else{
            ran[idx++] = row->chars[i];
        }
    }

    ran[idx] = '\0';
    row->render = ran;
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len){
    if(at < 0 || at > E.numrows)
        return;
    erow *erows = realloc(E.rows, sizeof(erow) * (E.numrows + 1));
    if(!erows){
        die("editorAppendRow function realloc()");
    }
    memmove(erows + at + 1, erows + at, sizeof(erow) * (E.numrows - at ));
    erows[at].chars = (char *)malloc(len + 1);
    memcpy(erows[at].chars, s, len);
    erows[at].chars[len] = '\0';
    erows[at].size = len;
    E.rows = erows;

    E.rows[at].rsize = 0;
    E.rows[at].render = NULL;

    editorUpdateRow(E.rows + at);

    E.numrows++;
}

char *editorPrompt(char *prompt, void (*callback)(char*, char)){
    int strlen = 128;
    char *buf = malloc(strlen);

    *buf = '\0';
    int len = 0;

    while(1){
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if(c == '\x1b'){
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        }else if(c == CTRL_KEY('h') || c == DEL_KEY || c == BACKSPACE){
            if(len > 0) buf[--len] = '\0';
        }else if(c == '\r'){
            if(len > 0){
                editorSetStatusMessage(prompt, "");
                return buf;
            }
        }else if( !iscntrl(c) && c < 128){
            if(len == strlen - 1){
                strlen *= 2;
                buf = realloc(buf, strlen);
            }
            buf[len++] = c;
            buf[len] = '\0';
        }

        if(callback) callback(buf, c);
    }
}

void editorFindCallback(char *query, char c){

    for(int i = 0; i < E.numrows; i++){
        erow *rows = E.rows + i;
        char *match = strstr(rows->render, query);

        if(match){
            E.cy = i;
            E.cx = match - rows->render;
            E.rowoff = E.numrows;
            break;
        }
    }

}

void editorFind(){
    int save_cx = E.cx;
    int save_cy = E.cy;
    int save_colsoff = E.colsoff;
    int save_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (ESC to cancel)", editorFindCallback);

    if(query){
        free(query);
        editorSetStatusMessage("");
    }else{
        E.cx = save_cx;
        E.cy = save_cy;
        E.colsoff = save_colsoff;
        E.rowoff = save_rowoff;
    }
}

void editorOpen(char *filename){
    char *line = NULL;
    size_t lengcap = 0;
    FILE *file = fopen(filename, "r");
    E.filename = strdup(filename);
    ssize_t lenght = getline(&line, &lengcap , file);
    while(lenght != -1){
        while(lenght > 0 && (line[lenght-1] == '\n' || line[lenght-1] == '\r'))
            lenght--;
            
        editorInsertRow(E.numrows, line, lenght);
        free(line);
        line = NULL;
        lenght = getline(&line, &lengcap , file);
    }

    fclose(file);
}

void initEditor(){
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.numrows = 0;
    E.rowoff = 0;
    E.colsoff = 0;
    E.rows = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.dirty = 0;

    if(getWindowSize(&E.screenrows, &E.screencols) == -1){
        die("initEditor function getWindowSize()");
    }
    E.screenrows -= 2;
}


int main(int argc, char **argv){
    enableRawMode();
    initEditor();
    if(argc >= 2){
        editorOpen(argv[1]);
    }

    editorSetStatusMessage(
        "HELP: Ctrl-s = save, Ctrl-Q = quit, Ctrl-F = Search");

    while(1){
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
