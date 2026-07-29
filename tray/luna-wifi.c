/* luna-wifi — ConnMan Wi-Fi control in an XEmbed system tray (no GTK). */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ICON_SIZE 24
#define MENU_W 360
#define MENU_H 370
#define MAX_SERVICES 12
#define ROW_H 24
#define XEMBED_EMBEDDED_NOTIFY 0
#define SYSTEM_TRAY_REQUEST_DOCK 0
typedef struct { char name[96], path[160]; int connected; } Service;
typedef struct { Display *dpy; Window icon, menu; int screen, shown, wifi_on; Atom utf8, name, manager, selection, opcode, xembed; Service services[MAX_SERVICES]; int count; } Wifi;
static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }

static void command(const char *a, const char *b) {
    if (fork() != 0) return;
    if (b) execlp("connmanctl", "connmanctl", a, b, (char *)NULL);
    else execlp("connmanctl", "connmanctl", a, (char *)NULL);
    _exit(127);
}
static void dock(Wifi *w) {
    Window owner = XGetSelectionOwner(w->dpy, w->selection); if (!owner) return;
    XEvent e; memset(&e, 0, sizeof(e)); e.xclient.type=ClientMessage; e.xclient.window=owner;
    e.xclient.message_type=w->opcode; e.xclient.format=32; e.xclient.data.l[0]=CurrentTime;
    e.xclient.data.l[1]=SYSTEM_TRAY_REQUEST_DOCK; e.xclient.data.l[2]=w->icon;
    XSendEvent(w->dpy, owner, False, NoEventMask, &e);
}
static void refresh(Wifi *w) {
    char line[512]; w->count=0; w->wifi_on=0;
    FILE *f=popen("connmanctl technologies 2>/dev/null", "r");
    if (f) { while(fgets(line,sizeof(line),f)) if(strstr(line,"/net/connman/technology/wifi")) w->wifi_on=1; pclose(f); }
    f=popen("connmanctl services 2>/dev/null", "r"); if(!f) return;
    while(w->count<MAX_SERVICES && fgets(line,sizeof(line),f)) {
        char *path=strstr(line," wifi_"); if(!path) continue; *path++=0;
        Service *s=&w->services[w->count]; memset(s,0,sizeof(*s)); s->connected=strchr(line,'*')!=NULL;
        char *n=line; while(*n==' '||*n=='*'||*n=='A'||*n=='R'||*n=='O') n++;
        snprintf(s->name,sizeof(s->name),"%.*s",(int)(sizeof(s->name)-1),n); size_t l=strlen(s->name); while(l&&(s->name[l-1]==' '||s->name[l-1]=='\n'))s->name[--l]=0;
        char *end=path; while(*end&&*end!=' '&&*end!='\n') end++;
        snprintf(s->path,sizeof(s->path),"%.*s",(int)(end-path),path);
        if(*s->name&&*s->path) w->count++;
    } pclose(f);
}
static void tooltip(Wifi *w) {
    char t[160]="Wi-Fi disabled"; for(int i=0;i<w->count;i++) if(w->services[i].connected) snprintf(t,sizeof(t),"Wi-Fi: %s",w->services[i].name);
    XChangeProperty(w->dpy,w->icon,w->name,w->utf8,8,PropModeReplace,(unsigned char*)t,(int)strlen(t));
}
static void draw_icon(Wifi *w) {
    GC g=XCreateGC(w->dpy,w->icon,0,NULL); XSetForeground(w->dpy,g,WhitePixel(w->dpy,w->screen)); XFillRectangle(w->dpy,w->icon,g,0,0,ICON_SIZE,ICON_SIZE);
    XSetForeground(w->dpy,g,w->wifi_on?0x1976d2:0x6b7280); XFillArc(w->dpy,w->icon,g,3,3,18,18,35*64,110*64); XFillArc(w->dpy,w->icon,g,6,7,12,12,35*64,110*64); XFillArc(w->dpy,w->icon,g,10,12,4,4,0,360*64); XFreeGC(w->dpy,g);
}
static void label(Wifi*w,GC g,int x,int y,const char*s,unsigned long c){XSetForeground(w->dpy,g,c);XDrawString(w->dpy,w->menu,g,x,y,s,(int)strlen(s));}
static void draw_menu(Wifi *w) {
    GC g=XCreateGC(w->dpy,w->menu,0,NULL); XSetForeground(w->dpy,g,0x172033); XFillRectangle(w->dpy,w->menu,g,0,0,MENU_W,MENU_H);
    label(w,g,18,28,"Wi-Fi Networks",0xf8fafc); label(w,g,18,49,w->wifi_on?"Select a network to connect or disconnect":"Wi-Fi is disabled",0x9fb3c8);
    for(int i=0;i<w->count;i++){int y=65+i*ROW_H;Service*s=&w->services[i];XSetForeground(w->dpy,g,s->connected?0x1d4ed8:0x22304a);XFillRectangle(w->dpy,w->menu,g,10,y,MENU_W-20,ROW_H-2);char row[150];snprintf(row,sizeof(row),"%s%s",s->connected?"Connected  ":"          ",s->name);label(w,g,20,y+16,row,0xf8fafc);}
    if(!w->count) label(w,g,18,88,"No networks found — choose Scan",0x9fb3c8);
    int y=MENU_H-42; XSetForeground(w->dpy,g,0x27364f); XFillRectangle(w->dpy,w->menu,g,10,y,MENU_W-20,30);
    label(w,g,20,y+20,"Scan",0xf8fafc); label(w,g,112,y+20,w->wifi_on?"Turn Wi-Fi Off":"Turn Wi-Fi On",0xf8fafc); label(w,g,280,y+20,"Quit",0xf8fafc); XFreeGC(w->dpy,g);
}
static void show(Wifi*w){refresh(w);tooltip(w);draw_icon(w);XMoveWindow(w->dpy,w->menu,DisplayWidth(w->dpy,w->screen)-MENU_W-16,30);XMapRaised(w->dpy,w->menu);w->shown=1;draw_menu(w);}
int main(void) {
    Wifi w;memset(&w,0,sizeof(w));signal(SIGINT,stop);signal(SIGTERM,stop);w.dpy=XOpenDisplay(NULL);if(!w.dpy){fprintf(stderr,"luna-wifi: cannot open DISPLAY\n");return 1;}w.screen=DefaultScreen(w.dpy);Window root=RootWindow(w.dpy,w.screen);
    w.utf8=XInternAtom(w.dpy,"UTF8_STRING",False);w.name=XInternAtom(w.dpy,"_NET_WM_NAME",False);w.manager=XInternAtom(w.dpy,"MANAGER",False);w.selection=XInternAtom(w.dpy,"_NET_SYSTEM_TRAY_S0",False);w.opcode=XInternAtom(w.dpy,"_NET_SYSTEM_TRAY_OPCODE",False);w.xembed=XInternAtom(w.dpy,"_XEMBED",False);
    w.icon=XCreateSimpleWindow(w.dpy,root,0,0,ICON_SIZE,ICON_SIZE,0,BlackPixel(w.dpy,w.screen),WhitePixel(w.dpy,w.screen));XSelectInput(w.dpy,w.icon,ExposureMask|ButtonPressMask|StructureNotifyMask);XSelectInput(w.dpy,root,StructureNotifyMask);long info[2]={0,1};Atom xi=XInternAtom(w.dpy,"_XEMBED_INFO",False);XChangeProperty(w.dpy,w.icon,xi,xi,32,PropModeReplace,(unsigned char*)info,2);XStoreName(w.dpy,w.icon,"Luna Wi-Fi");XMapRaised(w.dpy,w.icon);
    XSetWindowAttributes a;a.override_redirect=True;a.background_pixel=0x172033;w.menu=XCreateWindow(w.dpy,root,0,30,MENU_W,MENU_H,0,CopyFromParent,InputOutput,CopyFromParent,CWOverrideRedirect|CWBackPixel,&a);XSelectInput(w.dpy,w.menu,ExposureMask|ButtonPressMask);refresh(&w);tooltip(&w);draw_icon(&w);dock(&w);
    while(running){XEvent e;XNextEvent(w.dpy,&e);if(e.type==Expose){if(e.xexpose.window==w.icon)draw_icon(&w);else if(e.xexpose.window==w.menu)draw_menu(&w);}else if(e.type==ClientMessage&&e.xclient.message_type==w.manager)dock(&w);else if(e.type==ClientMessage&&e.xclient.message_type==w.xembed&&e.xclient.data.l[1]==XEMBED_EMBEDDED_NOTIFY)draw_icon(&w);else if(e.type==ButtonPress&&e.xbutton.window==w.icon){if(w.shown){XUnmapWindow(w.dpy,w.menu);w.shown=0;}else show(&w);}else if(e.type==ButtonPress&&e.xbutton.window==w.menu){int y=e.xbutton.y,by=MENU_H-42;if(y>=65&&y<65+w.count*ROW_H){Service*s=&w.services[(y-65)/ROW_H];command(s->connected?"disconnect":"connect",s->path);XUnmapWindow(w.dpy,w.menu);w.shown=0;}else if(y>=by){if(e.xbutton.x<95)command("scan","wifi");else if(e.xbutton.x<260)command(w.wifi_on?"disable":"enable","wifi");else running=0;}}XFlush(w.dpy);}
    XDestroyWindow(w.dpy,w.menu);XDestroyWindow(w.dpy,w.icon);XCloseDisplay(w.dpy);return 0;
}
