#include <panel.h>
#include <errno.h>
#include <ctype.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>
#include <signal.h>
#include <ncurses.h>
#include <pthread.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <sys/utsname.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>
#include <gnu/libc-version.h>

#define PROGNAME "omphalos"	// FIXME
#define VERSION  "0.98-pre"	// FIXME

#define PAD_LINES 4
#define PAD_COLS (COLS - START_COL * 2)
#define START_LINE 2
#define START_COL 2

// Bind one of these state structures to each interface in the callback,
// and also associate an iface with them via ifacenum (for UI actions).
typedef struct iface_state {
	int ifacenum;			// iface number
	int scrline;			// line within the containing pad
	WINDOW *subpad;			// subpad
	const char *typestr;		// looked up using iface->arptype
	struct iface_state *next,*prev;
} iface_state;

enum {
	BORDER_COLOR = 1,		// main window
	HEADING_COLOR,
	DBORDER_COLOR,			// down interfaces
	DHEADING_COLOR,
	UBORDER_COLOR,			// up interfaces
	UHEADING_COLOR,
	PBORDER_COLOR,			// popups
	PHEADING_COLOR,
};

// FIXME granularize things, make packet handler iret-like
static pthread_mutex_t bfl = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static WINDOW *pad;
static pthread_t inputtid;
static struct utsname sysuts;
static unsigned count_interface;
static iface_state *current_iface;
static const char *glibc_version,*glibc_release;

// Status bar at the bottom of the screen. Must be reallocated upon screen
// resize and allocated based on initial screen at startup. Don't shrink
// it; widening the window again should show the full message.
static char *statusmsg;
static int statuschars;	// True size, not necessarily what's available

#define ANSITERM_COLS 80

// Pass current number of columns
static int
setup_statusbar(int cols){
	if(cols < 0){
		return -1;
	}else if(cols < ANSITERM_COLS){
		cols = ANSITERM_COLS;
	}
	if(statuschars <= cols){
		const size_t s = cols + 1;
		char *sm;

		if((sm = realloc(statusmsg,s)) == NULL){
			return -1;
		}
		statuschars = s;
		if(statusmsg == NULL){
			time_t t = time(NULL);
			struct tm tm;

			if(localtime_r(&t,&tm)){
				strftime(sm,s,"launched at %T",&tm);
			}else{
				sm[0] = '\0';
			}
		}
		statusmsg = sm;
	}
	return 0;
}

static inline int
interface_up_p(const interface *i){
	return (i->flags & IFF_UP);
}

static inline int
interface_carrier_p(const interface *i){
	return (i->flags & IFF_LOWER_UP);
}

static inline int
interface_promisc_p(const interface *i){
	return (i->flags & IFF_PROMISC);
}

static int
iface_optstr(WINDOW *w,const char *str,int hcolor,int bcolor){
	if(wcolor_set(w,bcolor,NULL) != OK){
		return -1;
	}
	if(waddch(w,'|') == ERR){
		return -1;
	}
	if(wcolor_set(w,hcolor,NULL) != OK){
		return -1;
	}
	if(waddstr(w,str) == ERR){
		return -1;
	}
	return 0;
}

static const char *
duplexstr(unsigned dplx){
	switch(dplx){
		case DUPLEX_FULL: return "full"; break;
		case DUPLEX_HALF: return "half"; break;
		default: break;
	}
	return NULL;
}

// to be called only while ncurses lock is held
static int
iface_box(WINDOW *w,const interface *i,const iface_state *is){
	int bcolor,hcolor;
	size_t buslen;
	int attrs;

	// FIXME shouldn't have to know IFF_UP out here
	bcolor = interface_up_p(i) ? UBORDER_COLOR : DBORDER_COLOR;
	hcolor = interface_up_p(i) ? UHEADING_COLOR : DHEADING_COLOR;
	attrs = ((is == current_iface) ? A_REVERSE : 0) | A_BOLD;
	if(wattron(w,attrs | COLOR_PAIR(bcolor)) != OK){
		goto err;
	}
	if(box(w,0,0) != OK){
		goto err;
	}
	if(wattroff(w,A_REVERSE)){
		goto err;
	}
	if(mvwprintw(w,0,START_COL,"[") < 0){
		goto err;
	}
	if(wcolor_set(w,hcolor,NULL)){
		goto err;
	}
	if(waddstr(w,i->name) == ERR){
		goto err;
	}
	if(wprintw(w," (%s",is->typestr) != OK){
		goto err;
	}
	if(strlen(i->drv.driver)){
		if(waddch(w,' ') == ERR){
			goto err;
		}
		if(waddstr(w,i->drv.driver) == ERR){
			goto err;
		}
		if(strlen(i->drv.version)){
			if(wprintw(w," %s",i->drv.version) != OK){
				goto err;
			}
		}
		if(strlen(i->drv.fw_version)){
			if(wprintw(w," fw %s",i->drv.fw_version) != OK){
				goto err;
			}
		}
	}
	if(waddch(w,')') != OK){
		goto err;
	}
	if(wcolor_set(w,bcolor,NULL)){
		goto err;
	}
	if(wprintw(w,"]") < 0){
		goto err;
	}
	if(wattron(w,attrs)){
		goto err;
	}
	if(wattroff(w,A_REVERSE)){
		goto err;
	}
	if(mvwprintw(w,PAD_LINES - 1,START_COL * 2,"[") < 0){
		goto err;
	}
	if(wcolor_set(w,hcolor,NULL)){
		goto err;
	}
	if(wprintw(w,"mtu %d",i->mtu) != OK){
		goto err;
	}
	if(interface_up_p(i)){
		if(iface_optstr(w,"up",hcolor,bcolor)){
			goto err;
		}
		if(!interface_carrier_p(i)){
			if(iface_optstr(w,"no carrier",hcolor,bcolor)){
				goto err;
			}
		}else if(i->settings_valid){
			if(wprintw(w," (%uMb %s)",i->settings.speed,duplexstr(i->settings.duplex)) == ERR){
				goto err;
			}
		}
	}else{
		if(iface_optstr(w,"down",hcolor,bcolor)){
			goto err;
		}
		// FIXME find out whether carrier is meaningful for down
		// interfaces (i've not seen one)
	}
	if(interface_promisc_p(i)){
		if(iface_optstr(w,"promisc",hcolor,bcolor)){
			goto err;
		}
	}
	if(wcolor_set(w,bcolor,NULL)){
		goto err;
	}
	if(wprintw(w,"]") < 0){
		goto err;
	}
	if(wattroff(w,A_BOLD) != OK){
		goto err;
	}
	if( (buslen = strlen(i->drv.bus_info)) ){
		if(mvwprintw(w,PAD_LINES - 1,COLS - (buslen + 3 + START_COL),
					"%s",i->drv.bus_info) != OK){
			goto err;
		}
	}
	if(wcolor_set(w,0,NULL) != OK){
		goto err;
	}
	if(wattroff(w,attrs) != OK){
		goto err;
	}
	return 0;

err:
	abort();
	return -1;
}

// to be called only while ncurses lock is held
static int
draw_main_window(WINDOW *w,const char *name,const char *ver){
	int rows,cols;

	getmaxyx(w,rows,cols);
	if(setup_statusbar(cols)){
		goto err;
	}
	if(wcolor_set(w,BORDER_COLOR,NULL) != OK){
		goto err;
	}
	if(box(w,0,0) != OK){
		goto err;
	}
	if(mvwprintw(w,0,2,"[") < 0){
		goto err;
	}
	if(wattron(w,A_BOLD | COLOR_PAIR(HEADING_COLOR)) != OK){
		goto err;
	}
	if(wprintw(w,"%s %s on %s %s (libc %s-%s)",name,ver,sysuts.sysname,
				sysuts.release,glibc_version,glibc_release) < 0){
		goto err;
	}
	if(wattroff(w,A_BOLD | COLOR_PAIR(HEADING_COLOR)) != OK){
		goto err;
	}
	if(wcolor_set(w,BORDER_COLOR,NULL) != OK){
		goto err;
	}
	if(wprintw(w,"]") < 0){
		goto err;
	}
	if(wattron(w,A_BOLD | COLOR_PAIR(HEADING_COLOR)) != OK){
		goto err;
	}
	// addstr() doesn't interpret format strings, so this is safe. It will
	// fail, however, if the string can't fit on the window, which will for
	// instance happen if there's an embedded newline.
	mvwaddstr(w,rows - 1,START_COL,statusmsg);
	if(wattroff(w,A_BOLD | COLOR_PAIR(BORDER_COLOR)) != OK){
		goto err;
	}
	if(wcolor_set(w,0,NULL) != OK){
		goto err;
	}
	if(prefresh(w,0,0,0,0,rows,cols)){
		goto err;
	}
	return 0;

err:
	abort();
	return -1;
}

static int
wvstatus_locked(WINDOW *w,const char *fmt,va_list va){
	if(fmt == NULL){
		statusmsg[0] = '\0';
	}else{
		vsnprintf(statusmsg,statuschars,fmt,va);
	}
	return draw_main_window(w,PROGNAME,VERSION);
}

// NULL fmt clears the status bar
static int
wvstatus(WINDOW *w,const char *fmt,va_list va){
	int ret;

	pthread_mutex_lock(&bfl);
	ret = wvstatus_locked(w,fmt,va);
	pthread_mutex_unlock(&bfl);
	return ret;
}

// NULL fmt clears the status bar
static int
wstatus_locked(WINDOW *w,const char *fmt,...){
	va_list va;
	int ret;

	va_start(va,fmt);
	ret = wvstatus_locked(w,fmt,va);
	va_end(va);
	return ret;
}

// NULL fmt clears the status bar
static int
wstatus(WINDOW *w,const char *fmt,...){
	va_list va;
	int ret;

	va_start(va,fmt);
	ret = wvstatus(w,fmt,va);
	va_end(va);
	return ret;
}

static const interface *
get_current_iface(void){
	if(current_iface){
		return iface_by_idx(current_iface->ifacenum);
	}
	return NULL;
}

static void
toggle_promisc_locked(const omphalos_iface *octx,WINDOW *w){
	const interface *i = get_current_iface();

	if(i){
		if(interface_promisc_p(i)){
			wstatus_locked(w,"Disabling promiscuity on %s",i->name);
			disable_promiscuity(octx,i);
		}else{
			wstatus_locked(w,"Enabling promiscuity on %s",i->name);
			enable_promiscuity(octx,i);
		}
	}
}

static void
up_interface_locked(WINDOW *w){
	const interface *i = get_current_iface();

	if(i){
		if(interface_up_p(i)){
			wstatus_locked(w,"%s: interface already up",i->name);
		}else{
			// FIXME send request to bring iface up
		}
	}
}

static void
use_next_iface_locked(WINDOW *w){
	if(current_iface && current_iface->next){
		const iface_state *is = current_iface;
		interface *i = iface_by_idx(is->ifacenum);

		current_iface = current_iface->next;
		iface_box(is->subpad,i,is);
		is = current_iface;
		i = iface_by_idx(is->ifacenum);
		iface_box(is->subpad,i,is);
		prefresh(w,0,0,0,0,LINES,COLS);
	}
}

static void
use_prev_iface_locked(WINDOW *w){
	if(current_iface && current_iface->prev){
		const iface_state *is = current_iface;
		interface *i = iface_by_idx(is->ifacenum);

		current_iface = current_iface->prev;
		iface_box(is->subpad,i,is);
		is = current_iface;
		i = iface_by_idx(is->ifacenum);
		iface_box(is->subpad,i,is);
		prefresh(w,0,0,0,0,LINES,COLS);
	}
}

// FIXME we ought precreate the help screen, and show/hide it rather than
// creating and destroying it every time.
struct panel_state {
	PANEL *p;
	WINDOW *w;
};

static void
hide_help_locked(WINDOW *w,struct panel_state *ps){
	hide_panel(ps->p);
	del_panel(ps->p);
	ps->p = NULL;
	delwin(ps->w);
	ps->w = NULL;
	update_panels();
	draw_main_window(w,PROGNAME,VERSION);
	doupdate();
}

static int
display_help_locked(WINDOW *mainw,struct panel_state *ps){
	int rows,cols,startrow;

	memset(ps,0,sizeof(*ps));
	getmaxyx(mainw,rows,cols);
	startrow = START_LINE + 1 + ((PAD_LINES + 1) * 4);
	if(rows <= startrow){
		abort();goto done;
	}
	if(cols < START_COL * 2 + 1){
		abort();goto done;
	}
	if((ps->w = newwin(rows - (startrow + 1),cols - START_COL * 2,startrow,START_COL)) == NULL){
		abort();goto done;
	}
	if((ps->p = new_panel(ps->w)) == NULL){
		abort();goto done;
	}
	if(wattron(ps->w,A_BOLD) == ERR){
		abort();goto done;
	}
	if(wcolor_set(ps->w,PBORDER_COLOR,NULL) != OK){
		abort();goto done;
	}
	if(box(ps->w,0,0) != OK){
		abort();goto done;
	}
	if(wcolor_set(ps->w,PHEADING_COLOR,NULL) != OK){
		abort();goto done;
	}
	if(mvwprintw(ps->w,0,START_COL * 2,"press 'h' to dismiss help") == ERR){
		abort();goto done;
	}
	update_panels();
	if(doupdate() == ERR){
		abort();goto done;
	}
	return 0;

done:
	if(ps->p){
		hide_panel(ps->p);
		del_panel(ps->p);
	}
	if(ps->w){
		delwin(ps->w);
	}
	memset(ps,0,sizeof(*ps));
	return -1;
}

struct ncurses_input_marshal {
	WINDOW *w;
	PANEL *p;
	const omphalos_iface *octx;
};

// input handler while the help screen is active
static int
help_input(void){
	int ch;

	while((ch = getch()) != 'q' && ch != 'Q' && ch !='h'){
		switch(ch){
			case KEY_UP: case 'k':
				// FIXME scroll up
				break;
			case KEY_DOWN: case 'j':
				// FIXME scroll down
				break;
		}
	}
	return ch;
}

static void *
ncurses_input_thread(void *unsafe_marsh){
	struct ncurses_input_marshal *nim = unsafe_marsh;
	const omphalos_iface *octx = nim->octx;
	struct panel_state help;
	WINDOW *w = nim->w;
	int ch;

	memset(&help,0,sizeof(help));
	while((ch = getch()) != 'q' && ch != 'Q'){
	switch(ch){
		case KEY_UP: case 'k':
			pthread_mutex_lock(&bfl);
				use_prev_iface_locked(w);
			pthread_mutex_unlock(&bfl);
			break;
		case KEY_DOWN: case 'j':
			pthread_mutex_lock(&bfl);
				use_next_iface_locked(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'p':
			pthread_mutex_lock(&bfl);
				toggle_promisc_locked(octx,w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'u':
			pthread_mutex_lock(&bfl);
				up_interface_locked(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'h':{
			int r;
			pthread_mutex_lock(&bfl);
				r = display_help_locked(w,&help);
			pthread_mutex_unlock(&bfl);
			if(r == 0){
				ch = help_input();
				pthread_mutex_lock(&bfl);
					hide_help_locked(w,&help);
				pthread_mutex_unlock(&bfl);
				if(ch == 'q' || ch == 'Q'){
					goto breakout;
				}
			}
			break;
		}default:
			if(isprint(ch)){
				wstatus(w,"unknown command '%c' ('h' for help)",ch);
			}else{
				wstatus(w,"unknown scancode '%d' ('h' for help)",ch);
			}
			break;
	}
	}
breakout:
	wstatus(w,"%s","shutting down");
	// we can't use raise() here, as that sends the signal only
	// to ourselves, and we have it masked.
	kill(getpid(),SIGINT);
	pthread_exit(NULL);
}

// Cleanup which ought be performed even if we had a failure elsewhere, or
// indeed never started.
static int
mandatory_cleanup(WINDOW **w,PANEL **p){
	int ret = 0;

	pthread_mutex_lock(&bfl);
	if(*p){
		if(del_panel(*p) == ERR){
			ret = -4;
		}
		*p = NULL;
	}
	if(*w){
		if(delwin(*w) != OK){
			ret = -1;
		}
		*w = NULL;
	}
	if(stdscr){
		if(delwin(stdscr) != OK){
			ret = -2;
		}
		stdscr = NULL;
	}
	if(endwin() != OK){
		ret = -3;
	}
	pthread_mutex_unlock(&bfl);
	switch(ret){
	case -4: fprintf(stderr,"Couldn't destroy main panel\n"); break;
	case -3: fprintf(stderr,"Couldn't end main window\n"); break;
	case -2: fprintf(stderr,"Couldn't delete main window\n"); break;
	case -1: fprintf(stderr,"Couldn't delete main pad\n"); break;
	case 0: break;
	default: fprintf(stderr,"Couldn't cleanup ncurses\n"); break;
	}
	return ret;
}

static WINDOW *
ncurses_setup(const omphalos_iface *octx,PANEL **panel){
	struct ncurses_input_marshal *nim;
	const char *errstr = NULL;
	WINDOW *w = NULL;
	PANEL *p = NULL;

	if(initscr() == NULL){
		fprintf(stderr,"Couldn't initialize ncurses\n");
		return NULL;
	}
	if(cbreak() != OK){
		errstr = "Couldn't disable input buffering\n";
		goto err;
	}
	if(noecho() != OK){
		errstr = "Couldn't disable input echoing\n";
		goto err;
	}
	if(intrflush(stdscr,TRUE) != OK){
		errstr = "Couldn't set flush-on-interrupt\n";
		goto err;
	}
	if(nonl() != OK){
		errstr = "Couldn't disable nl translation\n";
		goto err;
	}
	if(start_color() != OK){
		errstr = "Couldn't initialize ncurses color\n";
		goto err;
	}
	if(use_default_colors()){
		errstr = "Couldn't initialize ncurses colordefs\n";
		goto err;
	}
	if((w = newpad(LINES,COLS)) == NULL){
		errstr = "Couldn't initialize main pad\n";
		goto err;
	}
	if((p = new_panel(stdscr)) == NULL){
		errstr = "Couldn't initialize main panel\n";
		goto err;
	}
	keypad(stdscr,TRUE);
	if(init_pair(BORDER_COLOR,COLOR_GREEN,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(HEADING_COLOR,COLOR_YELLOW,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(DBORDER_COLOR,COLOR_WHITE,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(DHEADING_COLOR,COLOR_WHITE,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(UBORDER_COLOR,COLOR_YELLOW,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(UHEADING_COLOR,COLOR_GREEN,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(PBORDER_COLOR,COLOR_CYAN,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(PHEADING_COLOR,COLOR_RED,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(curs_set(0) == ERR){
		errstr = "Couldn't disable cursor\n";
		goto err;
	}
	if(setup_statusbar(COLS)){
		errstr = "Couldn't setup status bar\n";
		goto err;
	}
	if(draw_main_window(w,PROGNAME,VERSION)){
		errstr = "Couldn't use ncurses\n";
		goto err;
	}
	if((nim = malloc(sizeof(*nim))) == NULL){
		goto err;
	}
	nim->octx = octx;
	nim->w = w;
	nim->p = p;
	if(pthread_create(&inputtid,NULL,ncurses_input_thread,nim)){
		errstr = "Couldn't create UI thread\n";
		free(nim);
		goto err;
	}
	// FIXME install SIGWINCH() handler...
	*panel = p;
	return w;

err:
	mandatory_cleanup(&w,&p);
	fprintf(stderr,"%s",errstr);
	return NULL;
}

static int
print_iface_state(const interface *i __attribute__ ((unused)),const iface_state *is){
	if(mvwprintw(is->subpad,1,1,"pkts: %ju",i->frames) != OK){
		return -1;
	}
	if(prefresh(is->subpad,0,0,is->scrline,START_COL,is->scrline + PAD_LINES,START_COL + PAD_COLS) != OK){
		return -1;
	}
	return 0;
}

static inline void
packet_cb_locked(const interface *i,iface_state *is){
	if(is){
		if(print_iface_state(i,is)){
			abort();
		}
	}
}

static void
packet_callback(const interface *i __attribute__ ((unused)),void *unsafe){
	pthread_mutex_lock(&bfl);
	packet_cb_locked(i,unsafe);
	pthread_mutex_unlock(&bfl);
}

static inline void *
interface_cb_locked(const interface *i,int inum,iface_state *ret){
	if(ret == NULL){
		const char *tstr;

		if( (tstr = lookup_arptype(i->arptype)) ){
			if( (ret = malloc(sizeof(iface_state))) ){
				ret->typestr = tstr;
				ret->scrline = START_LINE + count_interface * (PAD_LINES + 1);
				ret->ifacenum = inum;
				if((ret->prev = current_iface) == NULL){
					current_iface = ret;
					ret->next = NULL;
				}else{
					while(ret->prev->next){
						ret->prev = ret->prev->next;
					}
					ret->next = ret->prev->next;
					ret->prev->next = ret;
				}
				if( (ret->subpad = subpad(pad,PAD_LINES,PAD_COLS,ret->scrline,START_COL)) ){
					++count_interface;
				}else{
					if(current_iface == ret){
						current_iface = NULL;
					}else{
						ret->prev->next = NULL;
					}
					free(ret);
					ret = NULL;
				}
			}
		}
	}
	if(ret){
		iface_box(ret->subpad,i,ret);
		if(i->flags & IFF_UP){
			packet_cb_locked(i,ret);
		}
		prefresh(pad,0,0,0,0,LINES,COLS);
	}
	return ret;
}

static void *
interface_callback(const interface *i,int inum,void *unsafe){
	void *r;

	pthread_mutex_lock(&bfl);
	r = interface_cb_locked(i,inum,unsafe);
	pthread_mutex_unlock(&bfl);
	return r;
}

static inline void
interface_removed_locked(iface_state *is){
	if(is){
		delwin(is->subpad);
		if(is->next){
			is->next->prev = is->prev;
		}
		if(is->prev){
			is->prev->next = is->next;
		}
		if(is == current_iface){
			current_iface = is->prev;
		}
		free(is);
		prefresh(pad,0,0,0,0,LINES,COLS);
	}
}

static void
interface_removed_callback(const interface *i __attribute__ ((unused)),void *unsafe){
	pthread_mutex_lock(&bfl);
	interface_removed_locked(unsafe);
	pthread_mutex_unlock(&bfl);
}

static void
diag_callback(const char *fmt,...){
	va_list va;

	va_start(va,fmt);
	wvstatus(pad,fmt,va);
	va_end(va);
}

int main(int argc,char * const *argv){
	omphalos_ctx pctx;
	PANEL *panel;

	if(setlocale(LC_ALL,"") == NULL){
		fprintf(stderr,"Couldn't initialize locale (%s?)\n",strerror(errno));
		return EXIT_FAILURE;
	}
	if(uname(&sysuts)){
		fprintf(stderr,"Coudln't get OS info (%s?)\n",strerror(errno));
		return EXIT_FAILURE;
	}
	glibc_version = gnu_get_libc_version();
	glibc_release = gnu_get_libc_release();
	if(omphalos_setup(argc,argv,&pctx)){
		return EXIT_FAILURE;
	}
	pctx.iface.packet_read = packet_callback;
	pctx.iface.iface_event = interface_callback;
	pctx.iface.iface_removed = interface_removed_callback;
	pctx.iface.diagnostic = diag_callback;
	if((pad = ncurses_setup(&pctx.iface,&panel)) == NULL){
		return EXIT_FAILURE;
	}
	if(omphalos_init(&pctx)){
		int err = errno;

		mandatory_cleanup(&pad,&panel);
		fprintf(stderr,"Error in omphalos_init() (%s?)\n",strerror(err));
		return EXIT_FAILURE;
	}
	omphalos_cleanup(&pctx);
	if(mandatory_cleanup(&pad,&panel)){
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
