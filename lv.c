#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 4
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

static int debug_num_1 = 0;
static int debug_num_2 = 0;

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN,
};

enum editorHighlight {
  HL_NORMAL = 0,
  HL_NUMBER,
  HL_MATCH,
};

/*** data ***/

typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
  char *linecol;
  unsigned char *hl;
} erow;

struct cords {
  int x;
  int y;
};

struct editorConfig {
  int cx, cy;  // cords for indexing into chars
  int rx, ry;  // cords for indexing into render
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  int lncolwidth;
  erow *rows;
  int sh_len;
  struct cords *searchhistory;
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios orig_termios;
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[1;1H", 3);

  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcsetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        // Process Page_up and Page_down keys
        if (read(STDIN_FILENO, &seq[2], 1)!= 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        // Process arrows keys
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
    }
    return '\x1b';  // Return the `Escape` key char
  } else {
    return c;
  }
}

/* Gets the current cursor position and writes the result into row and col
 */
int getCursorPosition(int *row, int *col) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", row, col) != 2) return -1;

  return 0;
}

int getWindowSize(int* row, int* col) {
  struct winsize ws;

  if (1 || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // Failed to get window size by TIOCGWINSZ request,
    // use fallback to manually calculate the window size.
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(row, col);
  } else {
    *col = ws.ws_col;
    *row = ws.ws_row;
    return 0;
  }
}

/*** syntax highlighting ***/

void editorUpdateSyntax(erow *row) {
  row->hl = realloc(row->hl, row->rsize);
  memset(row->hl, HL_NORMAL, row->rsize);

  for (int i = 0; i < row->rsize; i++) {
    if (isdigit(row->render[i])) {
      row->hl[i] = HL_NUMBER;
    }
  }
}

int editorSyntaxToColor(int hl) {
  switch (hl) {
    case HL_NUMBER: return 31;
    case HL_MATCH: return 34;
    default: return 37;
  }
}

/*** row operation ***/

/* Updates the rx and ry coords
 * Counts tab spaces and sets rx accordingly
 */
void editorUpdateRenderCoords() {
  erow *row = &E.rows[E.cy];
  int rx = E.lncolwidth - 1;
  for (int j = 0; j < E.cx; j++) {
    if (row->chars[j] == '\t') {
      rx += (KILO_TAB_STOP - 1);
      rx -= (rx % KILO_TAB_STOP);
    }
    rx++;
  }
  E.rx = rx;
  E.ry = E.cy;
}

/* Updates the cx and cy coords according to rx and ry values
 */
void editorUpdateDataCoords() {
  erow *row = &E.rows[E.ry];
  int rx = E.lncolwidth - 1;
  int j;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      rx += KILO_TAB_STOP - 1;
      rx -= (rx % KILO_TAB_STOP);
    }
    rx++;

    if (rx > E.rx) {
      debug_num_1 = rx;
      debug_num_2 = j;
      E.cx = j;
      E.cy = E.ry;  // TODO - will break with line wrapping
      return;
    };
  }

  E.cx = j;
  E.cy = E.ry;  // TODO - will break with line wrapping
}

/* Updates the render row for the given erow
 */
void editorUpdateRow(erow *row) {
  int tabs = 0;
  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') tabs++;
  }

  free(row->render);
  row->render = malloc(row->size + (tabs*KILO_TAB_STOP) + 1);

  int idx = 0;
  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;

  editorUpdateSyntax(row);
}

/* Appends a row onto erow using the given string
 * (Does not update the render row)
 */
void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return; 

  E.rows = realloc(E.rows, sizeof(erow) * (E.numrows + 1));
  memmove(&E.rows[at+1], &E.rows[at], sizeof(erow) * (E.numrows - at));

  E.rows[at].size = len;
  E.rows[at].chars = malloc(len + 1);
  memcpy(E.rows[at].chars, s, len);
  E.rows[at].chars[len] = '\0';

  E.rows[at].linecol = malloc(E.lncolwidth + 1); // TODO - this should prob be max num of digits for int

  E.rows[at].rsize = 0;
  E.rows[at].render = NULL;
  E.rows[at].hl = NULL;
  editorUpdateRow(&E.rows[at]);

  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->chars);
  free(row->render);
  free(row->linecol);
  free(row->hl);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.rows[at]);
  memmove(&E.rows[at], &E.rows[at+1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

/*
 * Attempts to insert a new char into the given row
 */
void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorInsertNewline() {
  if (E.cx == 0)
    editorInsertRow(E.cy, "", 0);
  else {
    erow *row = &E.rows[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.rows[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.cy++;
  E.cx = 0;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

/*
 * Attempts to remove a char from a given row
 */
void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at > row->size) return;
  memmove(&row->chars[at], &row->chars[at+1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.rows[E.cy], E.cx, c);
  E.cx++;
}

void editorDeleteChar() {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;

  erow *row = &E.rows[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.rows[E.cy -1].size;
    editorRowAppendString(&E.rows[E.cy - 1],row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/*** file i/o ***/

/*
 * Returns a string buffer with all the lines in E.rows concatenated
 * for file writing. Writes buflen with the size of the string buffer
 */
char* editorRowsToString(int *buflen) {
  int totlen = 0;
  for (int j = 0; j < E.numrows; j++) {
    totlen += E.rows[j].size + 1;
  }
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (int j = 0; j < E.numrows; j++) {
    memcpy(p, E.rows[j].chars, E.rows[j].size);
    p += E.rows[j].size + 1;
    p[-1] = '\n';
  }

  return buf;
}

void editorOpen(char* filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    // Strip off carriage return or newline
    while (linelen > 0 && (line[linelen - 1] == '\n' || line [linelen - 1] == '\r'))
      linelen--;
    
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }

  int len;
  char *buf = editorRowsToString(&len);

  /* More advanced editors will write to a new, temporary file, and then rename
   * that file to the actual file the user wants to overwrite, and they’ll carefully
   * check for errors through the whole process. */
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }
  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editorFindMoveToMatch(int off) {
  if (E.sh_len == 0) return;
  int i = 0;
  while (E.searchhistory[i].y < E.ry
    || (E.searchhistory[i].y == E.ry && E.searchhistory[i].x <= E.rx)) { i++; }
  i = (i + off + E.sh_len) % E.sh_len;

  E.rx = E.searchhistory[i].x;
  E.ry = E.searchhistory[i].y;
  editorUpdateDataCoords();
  E.rowoff = E.cy - (E.screenrows / 2);
  if (E.rowoff < 0) E.rowoff = 0;
}

void editorFindCallback(char *query, int key) {
  if (key == '\r' || key == '\x1b') return;

  struct saved_hl {
    int saved_cy;
    unsigned char* saved_hl;
  };

  static struct saved_hl *saved_hl_lines;
  static int saved_hl_size;
  static int saved_hl_cap;

  if (saved_hl_lines) {
    for (int i = 0; i < saved_hl_size; i++) {
      memcpy(E.rows[saved_hl_lines[i].saved_cy].hl,
             saved_hl_lines[i].saved_hl,
             E.rows[saved_hl_lines[i].saved_cy].rsize);
      free(saved_hl_lines[i].saved_hl);
    }
    free(saved_hl_lines);
  }
  saved_hl_size = 0;
  saved_hl_cap = 8;
  saved_hl_lines = malloc(sizeof(struct saved_hl) * saved_hl_cap);

  E.sh_len = 0;
  free(E.searchhistory);
  int size = 10;
  int si = 0;
  E.searchhistory = malloc(sizeof(struct cords) * size);

  for (int i = 0; i < E.numrows; i++) {
    erow *row = &E.rows[i];
    char *p_match = strstr(row->render, query);
    if (p_match) {
      saved_hl_size++;
      if (saved_hl_size >= saved_hl_cap) {
        saved_hl_cap *= 2;
        saved_hl_lines = realloc(saved_hl_lines, sizeof(struct saved_hl) * saved_hl_cap);
      }
      saved_hl_lines[saved_hl_size-1].saved_hl = malloc(row->rsize);
      memcpy(saved_hl_lines[saved_hl_size-1].saved_hl, row->hl, row->rsize);
      saved_hl_lines[saved_hl_size-1].saved_cy = i;
    }
    while (p_match != NULL) {
      E.sh_len++;
      E.searchhistory[si].x = p_match - row->render + E.lncolwidth - 1;
      E.searchhistory[si].y = i;
      si++;
      if (si >= size) {
        size *= 2;
        E.searchhistory = realloc(E.searchhistory, sizeof(struct cords) * size);
      }
      
      memset(&row->hl[p_match - row->render], HL_MATCH, strlen(query));

      p_match = strstr(p_match + 1, query);
    }
  }

  if (si) {
    editorUpdateDataCoords();
    editorFindMoveToMatch(0);
    E.rowoff = E.cy - (E.screenrows / 2);
    if (E.rowoff < 0) E.rowoff = 0;
  }
}

void editorFind() {
  int saved_cx = E.cx, saved_cy = E.cy;
  int saved_coloff = E.coloff, saved_rowoff = E.rowoff;

  char *query = editorPrompt("Search: %s (ESC to cancel)", editorFindCallback);
  if (query) {
    free(query);
  } else {
    E.cx = saved_cx;
    E.cy = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
}

/*** append buffer ***/

/* A dynamically managed string buffer
 */
struct abuf {
  char *b;
  size_t len;
};

#define ABUF_INIT {NULL, 0}

/* Append to the string buffer
 */
void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

/* Free the string buffer
 */
void abFree(struct abuf* ab) {
  free(ab->b);
}

/*** output ***/

/* Scrolls the view by adjusting the offsets at E.rowoff and E.coloff
 */
void editorScroll() {
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.cx < E.coloff) {
    E.coloff = E.cx;
  }
  if (E.cx >= E.coloff + E.screencols) {
    E.coloff = E.cx - E.screencols + 1;
  }
}

/* Scroll the screen vertically based on a given amount of rows
 * (Positive numbers scroll down the file)
 */
void editorVerticalScroll(int off) {
  // Save cursor location relative to screen to restore later
  int dy = E.cy - E.rowoff;
  int dx = E.cx;

  if (off > 0) {
    E.cy = E.rowoff + E.screenrows - 1;
    if (E.cy + off >= E.numrows)
      E.cy = E.numrows - 1;
    else
      E.cy += off;
  } else if (off < 0) {
    E.cy = E.rowoff;
    if (E.cy + off < 0)
      E.cy = 0;
    else
      E.cy += off;
  }

  // Scroll the screen and restore previous cursor location
  editorScroll();
  E.cy = E.rowoff + dy;
  E.cx = dx;
}

/* Renders our application according to EditorConfig
 */
void editorDrawRows(struct abuf *ab) {
  for (int y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      // Add a welcome message if we don't open a file
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "Kilo editor -- version %s", KILO_VERSION);
        if (welcomelen > E.screencols) welcomelen = E.screencols;

        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.rows[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols - E.lncolwidth)
        len = E.screencols - E.lncolwidth;
      
      // Draw the line number on the side
      int relline = (E.cy - E.rowoff) - y < 0 ? y - (E.cy - E.rowoff) : (E.cy - E.rowoff) - y;
      if (relline == 0)
        snprintf(E.rows[filerow].linecol, E.lncolwidth, "%3d  ", filerow);
      else
        snprintf(E.rows[filerow].linecol, E.lncolwidth, "%4d ", relline);
      abAppend(ab, E.rows[filerow].linecol, E.lncolwidth);

      // Draw the row
      char *c = &E.rows[filerow].render[E.coloff];
      unsigned char *hl = &E.rows[filerow].hl[E.coloff];
      int current_color = -1;
      for (int j = 0; j < len; j++) {
        // Change color of any numbers
        if (hl[j] == HL_NORMAL) {
          if (current_color != -1) {
            abAppend(ab, "\x1b[39m", 5);
            current_color = -1;
          }
          abAppend(ab, &c[j], 1);
        } else {
          int color = editorSyntaxToColor(hl[j]);
          if (current_color != color) {
            current_color = color;
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
            abAppend(ab, buf, clen);
          }
          abAppend(ab, &c[j], 1);
        }
      }
      abAppend(ab, "\x1b[39m", 5);
    }

    abAppend(ab, "\x1b[K", 3);  // Clears the current line to the right of the cursor
    abAppend(ab, "\r\n", 2);
  }
}

/* A function that draws a status bar for viewing file information
 */
void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);  // Switch to inverted colors
  char status[80], rstatus[80];
  /*
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                     E.filename ? E.filename : "[No Name]", E.numrows,
                     E.dirty ? "(modified) : "");
                     */
  // DEBUG STATUS BAR
  int len = snprintf(status, sizeof(status), "cx: %d, cy: %d, rx: %d, ry: %d | sh_len: %d | debug1: %d, debug2: %d",
                     E.cx, E.cy, E.rx, E.ry, E.sh_len,
                     debug_num_1,
                     debug_num_2);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy +1, E.numrows);

  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);  // Switch back to normal formatting
  abAppend(ab, "\r\n", 2);
}

/* A function that draws a message bar for viewing messages from the editor
 */
void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

/* A function continuously called to redraw the screen
 */
void editorRefreshScreen() {
  editorUpdateRenderCoords();
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);  // Hide cursor temporarily
  abAppend(&ab, "\x1b[1;1H", 6);  // Move cursor to the beginning

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1,
                                            E.rx - E.coloff + 1);
  abAppend(&ab, buf, strlen(buf));  // Move cursor back to the stored x, y position
  abAppend(&ab, "\x1b[?25h", 6);  // Show cursor again

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/* Takes in a format string and variable number of args
 * and sets it as a message to the status bar
 */
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      if (callback) callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback) callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }

    if (callback) callback(buf, c);
  }
}

void editorMoveCursor(int key) {
  erow *row = E.cy >= E.numrows ? NULL : &E.rows[E.cy];

  switch (key) {
    case 'h':
    case ARROW_LEFT:
      if (E.cx != 0) E.cx--;
      break;

    case 'j':
    case ARROW_DOWN:
      if (E.cy < E.numrows) E.cy++;
      break;

    case 'k':
    case ARROW_UP:
      if (E.cy != 0) E.cy--;
      break;

    case 'l':
    case ARROW_RIGHT:
      if (row && E.cx < row->size) E.cx++;
      break;
  }

  // Correct cursor if it ends up outside the bounds of a line via movement
  row = (E.cy >= E.numrows) ? NULL : &E.rows[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

/* Handles the logic for processing user input/keypresses
 */
void editorProcessKeypress() {
  static int quit_times = KILO_QUIT_TIMES;
  int c = editorReadKey();

  switch (c) {
    case '\r':
      editorInsertNewline();
      break;
    
    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved chages. Press Ctrl-q %d more times to quit without saving.", quit_times--);
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[1;1H", 6);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case '0':
    case HOME_KEY:
      E.cx = 0;
      break;
    case '$':
    case END_KEY:
      if (E.cy < E.numrows)
        E.cx = E.rows[E.cy].size;
      break;

    case CTRL_KEY('f'):
      editorFind();
      break;

    case DEL_KEY:
      editorMoveCursor(ARROW_RIGHT);
    case BACKSPACE:
    case CTRL_KEY('h'):
      editorDeleteChar();
      break;

    case CTRL_KEY('d'):
    case PAGE_DOWN:
      editorVerticalScroll(E.screenrows / 2);
      break;
    case CTRL_KEY('u'):
    case PAGE_UP:
      editorVerticalScroll(E.screenrows / -2);
      break;

    case CTRL_KEY('e'):
      editorVerticalScroll(1);
      break;
    case CTRL_KEY('y'):
      editorVerticalScroll(-1);
      break;

    case 'h':
    case 'j':
    case 'k':
    case 'l':
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;

    case 'n':
      editorMoveCursor(ARROW_RIGHT);
      editorFindMoveToMatch(0);
      break;

    case 'N':
      editorFindMoveToMatch(-1);
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      break;

    default:
      editorInsertChar(c);
      break;
  }

  quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

/* Initializes all fields in the `E` construct
 */
void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.lncolwidth = 6;
  E.rows = NULL;
  E.sh_len = 0;
  E.searchhistory = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  // Setup editor
  enableRawMode();
  initEditor();

  // Load file
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  // Add a helpful message to the status bar on startup
  editorSetStatusMessage("HELP: Ctrl-s = save | Ctrl-q = quit | Ctrl-f = find");

  // Editor main loop
  while(1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
