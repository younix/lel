#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "arg.h"
char *argv0;

#define APP_NAME "lel"
#define HEADER_FORMAT "imagefile########"

/* Image status flags. */
enum { NONE = 0, LOADED = 1, SCALED = 2, DRAWN = 4 };
/* View mode. */
enum { ASPECT = 0, FULL_ASPECT, FULL_STRETCH };

static int viewmode = ASPECT;
static char *wintitle = APP_NAME;
static XImage *ximg = NULL;
static Drawable xpix = 0;
static Display *dpy = NULL;
static Window win;
static GC gc;
static int screen, xfd;
static int running = 1;
static int imgstate = NONE;
static int imgwidth, imgheight;
static uint8_t *imgbuf;
static int winx, winy, winwidth = 320, winheight = 240;
static int panxoffset = 0, panyoffset = 0;
static float zoomfact = 1.0, zoominc = 0.25;

void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if(fmt[0] && fmt[strlen(fmt)-1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	}
	exit(EXIT_FAILURE);
}

void
usage(void)
{
	die("%s", APP_NAME " " VERSION " - (c) 2014 " APP_NAME " engineers\n\n"
	      "usage: " APP_NAME "[OPTIONS...] [FILE]\n"
	      "    -a            Full window, keep aspect ratio\n"
	      "    -f            Full window, stretch (no aspect)\n"
	      "    -w <w>        Window width\n"
	      "    -h <h>        Window height\n"
	      "    -x <x>        Window x position\n"
	      "    -y <y>        Window y position\n"
	      "    -t <title>    Use title\n"
	      "    -v            Print version and exit\n");
}

int
if_open(FILE *f)
{
	uint8_t hdr[17];

	if (fread(hdr, 1, strlen(HEADER_FORMAT), f) != strlen(HEADER_FORMAT))
		return -1;

	if(memcmp(hdr, "imagefile", 9))
		return -1;

	imgwidth = ntohl((hdr[9] << 0) | (hdr[10] << 8) | (hdr[11] << 16) | (hdr[12] << 24));
	imgheight = ntohl((hdr[13] << 0) | (hdr[14] << 8) | (hdr[15] << 16) | (hdr[16] << 24));
	if(imgwidth <= 0 || imgheight <= 0)
		return -1;

	return 0;
}

int
if_read(FILE *f)
{
	int i, j, off, row_len;
	uint8_t *row;

	row_len = imgwidth * strlen("RGBA");
	if(!(row = malloc(row_len)))
		return 1;

	for(off = 0, i = 0; i < imgheight; ++i) {
		if(fread(row, 1, (size_t)row_len, f) != (size_t)row_len) {
			free(row);
			die("unexpected EOF or row-skew at %d\n", i);
		}
		for(j = 0; j < row_len; j += 4, off += 4) {
			imgbuf[off] = row[j];
			imgbuf[off + 1] = row[j + 1];
			imgbuf[off + 2] = row[j + 2];
			imgbuf[off + 3] = row[j + 3];
		}
	}
	free(row);

	imgstate |= LOADED;

	return 0;
}

/* NOTE: will be removed later, for debugging alpha mask */
#if 0
void
normalsize(char *newbuf)
{
	unsigned int x, y, soff = 0, doff = 0;

	for(y = 0; y < imgheight; y++) {
		for(x = 0; x < imgwidth; x++, soff += 4, doff += 4) {
			newbuf[doff+0] = imgbuf[soff+2];
			newbuf[doff+1] = imgbuf[soff+1];
			newbuf[doff+2] = imgbuf[soff+0];
			newbuf[doff+3] = imgbuf[soff+3];
		}
	}
}
#endif

/* scales imgbuf data to newbuf (ximg->data), nearest neighbour. */
void
scale(unsigned int width, unsigned int height, unsigned int bytesperline,
	char *newbuf)
{
	unsigned char *ibuf;
	unsigned int jdy, dx, bufx, x, y;

	jdy = bytesperline / 4 - width;
	dx = (imgwidth << 10) / width;
	for(y = 0; y < height; y++) {
		bufx = imgwidth / width;
		ibuf = &imgbuf[y * imgheight / height * imgwidth * 4];

		for(x = 0; x < width; x++) {
			*newbuf++ = (ibuf[(bufx >> 10)*4+2]);
			*newbuf++ = (ibuf[(bufx >> 10)*4+1]);
			*newbuf++ = (ibuf[(bufx >> 10)*4+0]);
			newbuf++;
			bufx += dx;
		}
		newbuf += jdy;
	}
}

void
ximage(unsigned int newwidth, unsigned int newheight)
{
	int depth;

	/* destroy previous image */
	if(ximg) {
		XDestroyImage(ximg);
		ximg = NULL;
	}
	depth = DefaultDepth(dpy, screen);
	if(depth >= 24) {
		if(xpix)
			XFreePixmap(dpy, xpix);
		xpix = XCreatePixmap(dpy, win, winwidth, winheight, depth);
		ximg = XCreateImage(dpy, CopyFromParent, depth,	ZPixmap, 0,
		                    NULL, newwidth, newheight, 32, 0);
		ximg->data = malloc(ximg->bytes_per_line * ximg->height);
		scale(ximg->width, ximg->height, ximg->bytes_per_line, ximg->data);
		XInitImage(ximg);
	} else {
		die("This program does not yet support display depths < 24.\n");
	}
}

void
scaleview(void)
{
	switch(viewmode) {
	case FULL_STRETCH:
		ximage(winwidth, winheight);
		break;
	case FULL_ASPECT:
		if(winwidth * imgheight > winheight * imgwidth)
			ximage(imgwidth * winheight / imgheight, winheight);
		else
			ximage(winwidth, imgheight * winwidth / imgwidth);
		break;
	case ASPECT:
	default:
		ximage(imgwidth * zoomfact, imgheight * zoomfact);
		break;
	}
	imgstate |= SCALED;
}

void
draw(void)
{
	int xoffset = 0, yoffset = 0;

	if(viewmode != FULL_STRETCH) {
		/* center vertical, horizontal */
		xoffset = (winwidth - ximg->width) / 2;
		yoffset = (winheight - ximg->height) / 2;
		/* pan offset */
		xoffset -= panxoffset;
		yoffset -= panyoffset;
	}
	XSetForeground(dpy, gc, BlackPixel(dpy, 0));
	XFillRectangle(dpy, xpix, gc, 0, 0, winwidth, winheight);
	XPutImage(dpy, xpix, gc, ximg, 0, 0, xoffset, yoffset, ximg->width, ximg->height);
	XCopyArea(dpy, xpix, win, gc, 0, 0, winwidth, winheight, 0, 0);

	XFlush(dpy);
	imgstate |= DRAWN;
}

void
update(void)
{
	if(!(imgstate & LOADED))
		return;
	if(!(imgstate & SCALED))
		scaleview();
	if(!(imgstate & DRAWN))
		draw();
}

void
setview(int mode)
{
	if(viewmode == mode)
		return;
	viewmode = mode;
	imgstate &= ~(DRAWN | SCALED);
	update();
}

void
pan(int x, int y)
{
	panxoffset -= x;
	panyoffset -= y;
	imgstate &= ~(DRAWN | SCALED);
	update();
}

void
inczoom(float f)
{
	if((zoomfact + f) <= 0)
		return;
	zoomfact += f;
	imgstate &= ~(DRAWN | SCALED);
	update();
}

void
zoom(float f)
{
	if(f == zoomfact)
		return;
	zoomfact = f;
	imgstate &= ~(DRAWN | SCALED);
	update();
}

void
buttonpress(XEvent *ev)
{
	switch(ev->xbutton.button) {
	case Button4:
		inczoom(zoominc);
		break;
	case Button5:
		inczoom(-zoominc);
		break;
	}
}

void
keypress(XEvent *ev)
{
	KeySym key;

	key = XLookupKeysym(&ev->xkey, 0);
	switch(key) {
	case XK_Escape:
	case XK_q:
		running = 0;
		break;
	case XK_Left:
	case XK_h:
		pan(winwidth / 20, 0);
		break;
	case XK_Down:
	case XK_j:
		pan(0, -(winheight / 20));
		break;
	case XK_Up:
	case XK_k:
		pan(0, winheight / 20);
		break;
	case XK_Right:
	case XK_l:
		pan(-(winwidth / 20), 0);
		break;
	case XK_a:
		setview(FULL_ASPECT);
		break;
	case XK_o:
		setview(ASPECT);
		break;
	case XK_f:
		setview(FULL_STRETCH);
		break;
	case XK_KP_Add:
	case XK_equal:
	case XK_plus:
		inczoom(zoominc);
		break;
	case XK_KP_Subtract:
	case XK_underscore:
	case XK_minus:
		inczoom(-zoominc);
		break;
	case XK_3:
		zoom(4.0);
		break;
	case XK_2:
		zoom(2.0);
		break;
	case XK_1:
		zoom(1.0);
		break;
	case XK_0:
		zoom(1.0);
		setview(ASPECT); /* fallthrough */
	case XK_r:
		panxoffset = 0;
		panyoffset = 0;
		imgstate &= ~(DRAWN | SCALED);
		update();
		break;
	}
}

void
handleevent(XEvent *ev)
{
	XWindowAttributes attr;

	switch(ev->type) {
	case MapNotify:
		if (!winwidth || !winheight) {
			XGetWindowAttributes(ev->xmap.display, ev->xmap.window, &attr);
			winwidth = attr.width;
			winheight = attr.height;
		}
		break;
	case ConfigureNotify:
		if(winwidth != ev->xconfigure.width || winheight != ev->xconfigure.height) {
			winwidth = ev->xconfigure.width;
			winheight = ev->xconfigure.height;
			imgstate &= ~(SCALED);
		}
		break;
	case Expose:
		imgstate &= ~(DRAWN);
		update();
		break;
	case KeyPress:
		keypress(ev);
		break;
	case ButtonPress:
		buttonpress(ev);
		break;
	}
}

void
setup(void)
{
	XClassHint class = { APP_NAME, APP_NAME };

	if(!(dpy = XOpenDisplay(NULL)))
		die("Can't open X display.\n");
	xfd = ConnectionNumber(dpy);
	screen = DefaultScreen(dpy);

	win = XCreateWindow(dpy, DefaultRootWindow(dpy), winx, winy, winwidth, winheight, 0,
	                    DefaultDepth(dpy, screen), InputOutput,
	                    CopyFromParent, 0, NULL);
	gc = XCreateGC(dpy, win, 0, NULL);

	XStoreName(dpy, win, wintitle);
	XSelectInput(dpy, win, StructureNotifyMask | ExposureMask | KeyPressMask |
	                       ButtonPressMask);
	XMapRaised(dpy, win);
	XSetWMProperties(dpy, win, NULL, NULL, NULL, 0, NULL, NULL, &class);
	XFlush(dpy);
}

void
run(void)
{
	XEvent ev;

	while(running && !XNextEvent(dpy, &ev)) {
		handleevent(&ev);
	}
}

int
main(int argc, char *argv[]) {
	char *filename = "";
	FILE *fp = NULL;
	int tflag = 0;
	int wflag = 0;
	int hflag = 0;

	ARGBEGIN {
	case 'a':
		viewmode = FULL_ASPECT;
		break;
	case 'f':
		viewmode = FULL_STRETCH;
		break;
	case 'h':
		hflag = 1;
		if(!(winheight = atoi(EARGF(usage()))))
			usage();
		break;
	case 't':
		wintitle = EARGF(usage());
		tflag = 1;
		break;
	case 'w':
		wflag = 1;
		if(!(winwidth = atoi(EARGF(usage()))))
			usage();
		break;
	case 'x':
		winx = atoi(EARGF(usage()));
		break;
	case 'y':
		winy = atoi(EARGF(usage()));
		break;
	default:
		usage();
		break;
	} ARGEND;

	if(argc >= 1) {
		filename = argv[0];
		if(!(fp = fopen(filename, "rb"))) {
			die("can't read %s:", filename);
			return EXIT_FAILURE;
		}
	} else {
		filename = "<stdin>";
		fp = stdin;
	}
	if(!tflag)
		wintitle = filename;

	if(if_open(fp))
		die("can't open image (invalid format?)\n");
	if(!(imgbuf = malloc((imgwidth) * (imgheight) * 4)))
		die("can't malloc\n");
	if_read(fp);

	if(!wflag)
		winwidth = imgwidth;
	if(!hflag)
		winheight = imgheight;

	setup();
	run();

	if(fp && fp != stdin)
		fclose(fp);

	free(imgbuf);

	if(ximg)
		XDestroyImage(ximg);
	if(xpix)
		XFreePixmap(dpy, xpix);
	if(dpy)
		XCloseDisplay(dpy);

	return EXIT_SUCCESS;
}
