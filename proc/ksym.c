/*
 * Copyright 1998-2003 by Albert Cahalan; all rights reserved.
 * This file may be used subject to the terms and conditions of the
 * GNU Library General Public License Version 2, or any later version
 * at your option, as published by the Free Software Foundation.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Library General Public License for more details.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include "procps.h"
#include "version.h"
#include "sysinfo.h" /* smp_num_cpus */
#include "wchan.h"  // to verify prototypes

#define KSYMS_FILENAME "/proc/ksyms"

#if 0
#undef KSYMS_FILENAME
#define KSYMS_FILENAME  "/would/be/nice/to/have/this/file"
#define SYSMAP_FILENAME "/home/albert/ps/45621/System.map-hacked"
#define linux_version_code 131598 /* ? */
#define smp_num_cpus 2
#endif

#if 0
#undef KSYMS_FILENAME
#define KSYMS_FILENAME  "/home/albert/ps/45621/ksyms-2.3.12"
#define SYSMAP_FILENAME "/home/albert/ps/45621/System.map-2.3.12"
#define linux_version_code 131852 /* 2.3.12 */
#define smp_num_cpus 2
#endif

#if 0
#undef KSYMS_FILENAME
#define KSYMS_FILENAME  "/home/albert/ps/45621/ksyms-2.3.18ac8-MODVERS"
#define SYSMAP_FILENAME "/home/albert/ps/45621/System.map-2.3.18ac8-MODVERS"
#define linux_version_code 131858 /* 2.3.18ac8 */
#define smp_num_cpus 2
#endif

#if 0
#undef KSYMS_FILENAME
#define KSYMS_FILENAME  "/home/albert/ps/45621/ksyms-2.3.18ac8-NOMODVERS"
#define SYSMAP_FILENAME "/home/albert/ps/45621/System.map-2.3.18ac8-NOMODVERS"
#define linux_version_code 131858 /* 2.3.18ac8 */
#define smp_num_cpus 2
#endif

/* These are the symbol types, with relative popularity:
 *     ? w  machine type junk for Alpha -- odd syntax
 *     ? S  not for i386
 *     4 W  not for i386
 *    60 R
 *   100 A
 *   125 r
 *   363 s  not for i386
 *   858 B
 *   905 g  generated by modutils?
 *   929 G  generated by modutils?
 *  1301 b
 *  2750 D
 *  4481 d
 * 11417 ?
 * 13666 t
 * 15442 T
 *
 * For i386, that is: "RArBbDd?tT"
 */

#define SYMBOL_TYPE_CHARS "Tt?dDbBrARGgsWS"

/*
 * '?' is a symbol type
 * '.' is part of a name (versioning?)
 * "\t[]" are for the module name in /proc/ksyms
 */
#define LEGAL_SYSMAP_CHARS "0123456789_ ?.\n\t[]" \
                     "abcdefghijklmnopqrstuvwxyz" \
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

/* System.map lines look like:
 * hex num, space, one of SYMBOL_TYPE_CHARS, space, LEGAL_SYSMAP_CHARS, \n
 *
 * Alpha systems can start with a few lines that have the address replaced
 * by space padding and a 'w' for the type. For those lines, the last space
 * is followed by something like: mikasa_primo_mv p2k_mv sable_gamma_mv
 * (just one of those, always with a "_mv", then the newline)
 *
 * The /proc/ksyms lines are like System.map lines w/o the symbol type char.
 * When odd features are used, the name part contains:
 * "(.*)_R(smp_|smp2gig_|2gig_)?[0-9a-fA-F]{8,}"
 * It is likely that more crap will be added...
 */

typedef struct symb {
  const char *name;
  unsigned KLONG addr;
} symb;

/* These mostly rely on POSIX to make them zero. */

static symb hashtable[256];

static char       *sysmap_data;
static unsigned    sysmap_room;
static symb       *sysmap_index;
static unsigned    sysmap_count;

static char       *ksyms_data;
static unsigned    ksyms_room     = 4096;
static symb       *ksyms_index;
static unsigned    ksyms_count;
static unsigned    idx_room;

/*********************************/

/* Kill this:  _R(smp_?|smp2gig_?|2gig_?)?[0-9a-f]{8,}$
 * We kill:    (_R[^A-Z]*[0-9a-f]{8,})+$
 *
 * The loop should almost never be taken, but it has to be there.
 * It gets rid of anything that _looks_ like a version code, even
 * if a real version code has already been found. This is because
 * the inability to perfectly recognize a version code may lead to
 * symbol mangling, which in turn leads to mismatches between the
 * /proc/ksyms and System.map data files.
 */
#if 0
static char *chop_version(char *arg){
  char *cp;
  cp = strchr(arg,'\t');
  if(cp) *cp = '\0';  /* kill trailing module name first */
  for(;;){
    char *p;
    int len = 0;
    cp = strrchr(arg, 'R');
    if(!cp || cp<=arg+1 || cp[-1]!='_') break;
    for(p=cp; *++p; ){
      switch(*p){
      default:
        goto out;
      case '0' ... '9':
      case 'a' ... 'f':
        len++;
        continue;
      case 'g' ... 'z':
      case '_':
        len=0;
        continue;
      }
    }
    if(len<8) break;
    cp[-1] = '\0';
  }
out:
  if(*arg=='G'){
    int len = strlen(arg);
    while( len>8 && !memcmp(arg,"GPLONLY_",8) ){
      arg += 8;
      len -= 8;
    }
  }
  return arg;
}
#endif
static char *chop_version(char *arg){
  char *cp;
  cp = strchr(arg,'\t');
  if(cp) *cp = '\0';  /* kill trailing module name first */
  for(;;){
    int len;
    cp = strrchr(arg, 'R');
    if(!cp || cp<=arg+1 || cp[-1]!='_') break;
    len=strlen(cp);
    if(len<9) break;
    if(strpbrk(cp+1,"ABCDEFGHIJKLMNOPQRSTUVWXYZ")) break;
    if(strspn(cp+len-8,"0123456789abcdef")!=8) break;
    cp[-1] = '\0';
  }
  if(*arg=='G'){
    int len = strlen(arg);
    while( len>8 && !memcmp(arg,"GPLONLY_",8) ){
      arg += 8;
      len -= 8;
    }
  }
  return arg;
}

/***********************************/

static const symb *search(unsigned KLONG address, symb *idx, unsigned count){
  unsigned left;
  unsigned mid;
  unsigned right;
  if(!idx) return NULL;   /* maybe not allocated */
  if(address < idx[0].addr) return NULL;
  if(address >= idx[count-1].addr) return idx+count-1;
  left  = 0;
  right = count-1;
  for(;;){
    mid = (left + right) / 2;
    if(address >= idx[mid].addr) left  = mid;
    if(address <= idx[mid].addr) right = mid;
    if(right-left <= 1) break;
  }
  if(address == idx[right].addr) return idx+right;
  return idx+left;
}

/*********************************/

/* allocate if needed, read, and return buffer size */
static void read_file(const char *restrict filename, char **bufp, unsigned *restrict roomp) {
  int fd = 0;
  ssize_t done;
  char *buf = *bufp;
  ssize_t total = 0;
  unsigned room = *roomp;

  if(!room) goto hell;     /* failed before */
  if(!buf) buf = malloc(room);
  if(!buf) goto hell;
open_again:
  fd = open(filename, O_RDONLY|O_NOCTTY|O_NONBLOCK);
  if(fd<0){
    switch(errno){
    case EINTR:  goto open_again;
    default:     _exit(101);
    case EACCES:   /* somebody screwing around? */
      /* FIXME: set a flag to disable symbol lookup? */
    case ENOENT:;  /* no module support */
    }
    goto hell;
  }
  for(;;){
    done = read(fd, buf+total, room-total-1);
    if(done==0) break;  /* nothing left */
    if(done==-1){
      if(errno==EINTR) continue;  /* try again */
      perror("");
      goto hell;
    }
    if(done==(ssize_t)room-total-1){
      char *tmp;
      total += done;
      /* more to go, but no room in buffer */
      room *= 2;
      tmp = realloc(buf, room);
      if(!tmp) goto hell;
      buf = tmp;
      continue;
    }
    if(done>0 && done<(ssize_t)room-total-1){
      total += done; 
      continue;   /* OK, we read some. Go do more. */
    }
    fprintf(stderr,"%ld can't happen\n", (long)done);
    /* FIXME: memory leak */
    _exit(42);
  }
  buf[total] = '\0';   // parse_ksyms() expects NUL-terminated file
  *bufp = buf;
  *roomp = room;
  close(fd);
  return;
hell:
  if(buf) free(buf);
  *bufp = NULL;
  *roomp = 0;   /* this function will never work again */
  total = 0;
  if(fd>0) close(fd);
  return;
}

/*********************************/

static int parse_ksyms(void) {
  char *endp;
  if(!ksyms_room || !ksyms_data) goto quiet_goodbye;
  endp = ksyms_data;
  ksyms_count = 0;
  if(idx_room) goto bypass;  /* some space already allocated */
  idx_room = 512;
  for(;;){
    void *vp;
    idx_room *= 2;
    vp = realloc(ksyms_index, sizeof(symb)*idx_room);
    if(!vp) goto bad_alloc;
    ksyms_index = vp;
bypass:
    for(;;){
      char *saved;
      if(!*endp) return 1;
      saved = endp;
      ksyms_index[ksyms_count].addr = STRTOUKL(endp, &endp, 16);
      if(endp==saved || *endp != ' ') goto bad_parse;
      endp++;
      saved = endp;
      endp = strchr(endp,'\n');
      if(!endp) goto bad_parse;   /* no newline */
      *endp = '\0';
      ksyms_index[ksyms_count].name = chop_version(saved);
      ++endp;
      if(++ksyms_count >= idx_room) break;  /* need more space */
    }
  }

  if(0){
bad_alloc:
    fprintf(stderr, "Warning: not enough memory available\n");
  }
  if(0){
bad_parse:
    fprintf(stderr, "Warning: "KSYMS_FILENAME" not normal\n");
  }
quiet_goodbye:
  idx_room = 0;
  if(ksyms_data) free(ksyms_data) , ksyms_data = NULL;
  ksyms_room = 0;
  if(ksyms_index) free(ksyms_index) , ksyms_index = NULL;
  ksyms_count = 0;
  return 0;
}

/*********************************/

#define VCNT 16

static int sysmap_mmap(const char *restrict const filename, void (*message)(const char *restrict, ...)) {
  struct stat sbuf;
  char *endp;
  int fd;
  char Version[32];
  fd = open(filename, O_RDONLY|O_NOCTTY|O_NONBLOCK);
  if(fd<0) return 0;
  if(fstat(fd, &sbuf) < 0) goto bad_open;
  if(!S_ISREG(sbuf.st_mode)) goto bad_open;
  if(sbuf.st_size < 5000) goto bad_open;  /* if way too small */
  /* Would be shared read-only, but we want '\0' after each name. */
  endp = mmap(0, sbuf.st_size + 1, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
  sysmap_data = endp;
  while(*endp==' '){  /* damn Alpha machine types */
    if(strncmp(endp,"                 w ", 19)) goto bad_parse;
    endp += 19;
    endp = strchr(endp,'\n');
    if(!endp) goto bad_parse;   /* no newline */
    if(strncmp(endp-3, "_mv\n", 4)) goto bad_parse;
    endp++;
  }
  if(sysmap_data == (caddr_t) -1) goto bad_open;
  close(fd);
  fd = -1;
  sprintf(Version, "Version_%d", linux_version_code);
  sysmap_room = 512;
  for(;;){
    void *vp;
    sysmap_room *= 2;
    vp = realloc(sysmap_index, sizeof(symb)*sysmap_room);
    if(!vp) goto bad_alloc;
    sysmap_index = vp;
    for(;;){
      char *vstart;
      if(endp - sysmap_data >= sbuf.st_size){   /* if we reached the end */
        int i = VCNT;            /* check VCNT times to verify this file */
        if(*Version) goto bad_version;
        if(!ksyms_index) return 1; /* if can not verify, assume success */
        while(i--){
#if 1
          const symb *findme;
          const symb *map_symb;
          /* Choose VCNT entries from /proc/ksyms to test */
          findme = ksyms_index + (ksyms_count*i/VCNT);
          /* Search for them in the System.map */
          map_symb = search(findme->addr, sysmap_index, sysmap_count);
          if(map_symb){
            if(map_symb->addr != findme->addr) continue;
            /* backup to first matching address */
            while (map_symb != sysmap_index){
              if (map_symb->addr != (map_symb-1)->addr) break;
              map_symb--;
            }
            /* search for name in symbols with same address */
            while (map_symb != (sysmap_index+sysmap_count)){
              if (map_symb->addr != findme->addr) break;
              if (!strcmp(map_symb->name,findme->name)) goto good_match;
              map_symb++;
            }
            map_symb--; /* backup to last symbol with matching address */
            message("{%s} {%s}\n",map_symb->name,findme->name);
            goto bad_match;
          }
good_match:;
#endif
        }
        return 1; /* success */
      }
      sysmap_index[sysmap_count].addr = STRTOUKL(endp, &endp, 16);
      if(*endp != ' ') goto bad_parse;
      endp++;
      if(!strchr(SYMBOL_TYPE_CHARS, *endp)) goto bad_parse;
      endp++;
      if(*endp != ' ') goto bad_parse;
      endp++;
      vstart = endp;
      endp = strchr(endp,'\n');
      if(!endp) goto bad_parse;   /* no newline */
      *endp = '\0';
      ++endp;
      vstart = chop_version(vstart);
      sysmap_index[sysmap_count].name = vstart;
      if(*vstart=='V' && *Version && !strcmp(Version,vstart)) *Version='\0';
      if(++sysmap_count >= sysmap_room) break;  /* need more space */
    }
  }

  if(0){
bad_match:
    message("Warning: %s does not match kernel data.\n", filename);
  }
  if(0){
bad_version:
//
// Dear Slackware Packager,
//
//      As I am unable to find any sane way to contact you,
// I have resorted to an insane way. Forgive me.
//      Do you have a bug to report? Why don't you do so?
// The procps maintainer has kindly provided an email address
// for you to send bug reports to. You have never done so.
// Please use procps-feedback@lists.sf.net or, currently,
// albert@users.sf.net directly. Genuinely useful and correct
// patches will gladly be accepted, reducing your own work.
// Broken patches will be patiently discussed. You are also
// strongly encouraged to subscribe to procps-news@lists.sf.net
// for news of changes that may affect you.
//
//      Yours Truly,
//      Procps Maintainer
//
// P.S.
//
// A number of patches from the SuSE, Red Hat, and Mandrake
// packagers have been accepted. One of those packagers was
// saved from a disasterous patch (breaking thread support)
// because he discussed the problem. It's the Open Source way.
//
    message("Warning: %s has an incorrect kernel version.\n", filename);
  }
  if(0){
bad_alloc:
    message("Warning: not enough memory available\n");
  }
  if(0){
bad_parse:
    message("Warning: %s not parseable as a System.map\n", filename);
  }
  if(0){
bad_open:
    message("Warning: %s could not be opened as a System.map\n", filename);
  }

  sysmap_room=0;
  sysmap_count=0;
  if(sysmap_index) free(sysmap_index);
  sysmap_index = NULL;
  if(fd>=0) close(fd);
  if(sysmap_data) munmap(sysmap_data, sbuf.st_size + 1);
  sysmap_data = NULL;
  return 0;
}

/*********************************/

static void read_and_parse(void){
  static time_t stamp;    /* after data gets old, load /proc/ksyms again */
  if(time(NULL) != stamp){
    read_file(KSYMS_FILENAME, &ksyms_data, &ksyms_room);
    parse_ksyms();
    memset((void*)hashtable,0,sizeof(hashtable)); /* invalidate cache */
    stamp = time(NULL);
  }
}

/*********************************/

static void default_message(const char *restrict format, ...) {
    va_list arg;

    va_start (arg, format);
    vfprintf (stderr, format, arg);
    va_end (arg);
}

/*********************************/

static int use_wchan_file;

int open_psdb_message(const char *restrict override, void (*message)(const char *, ...)) {
  static const char *sysmap_paths[] = {
    "/boot/System.map-%s",
    "/boot/System.map",
    "/lib/modules/%s/System.map",
    "/usr/src/linux/System.map",
    "/System.map",
    NULL
  };
  struct stat sbuf;
  struct utsname uts;
  char path[128];
  const char **fmt = sysmap_paths;
  const char *sm;

#ifdef SYSMAP_FILENAME    /* debug feature */
  override = SYSMAP_FILENAME;
#endif

  // first allow for a user-selected System.map file
  if(
    (sm=override)
    ||
    (sm=getenv("PS_SYSMAP"))
    ||
    (sm=getenv("PS_SYSTEM_MAP"))
  ){
    read_and_parse();
    if(sysmap_mmap(sm, message)) return 0;
    /* failure is better than ignoring the user & using bad data */
    return -1;           /* ought to return "Namelist not found." */
  }

  // next try the Linux 2.5.xx method
  if(!stat("/proc/self/wchan", &sbuf)){
    use_wchan_file = 1; // hack
    return 0;
  }

  // finally, search for the System.map file
  uname(&uts);
  path[sizeof path - 1] = '\0';
  do{
    int did_ksyms = 0;
    snprintf(path, sizeof path - 1, *fmt, uts.release);
    if(!stat(path, &sbuf)){
      if (did_ksyms++) read_and_parse();
      if (sysmap_mmap(path, message)) return 0;
    }
  }while(*++fmt);
  /* TODO: Without System.map, no need to keep ksyms loaded. */
  return -1;
}

/***************************************/

int open_psdb(const char *restrict override) {
    return open_psdb_message(override, default_message);
}

/***************************************/

static const char * read_wchan_file(unsigned pid){
  static char buf[64];
  const char *ret = buf;
  ssize_t num;
  int fd;

  snprintf(buf, sizeof buf, "/proc/%d/wchan", pid);
  fd = open(buf, O_RDONLY);
  if(fd==-1) return "?";
  num = read(fd, buf, sizeof buf - 1);
  close(fd);
  if(num<1) return "?"; // allow for "0"
  buf[num] = '\0';

  if(buf[0]=='0' && buf[1]=='\0') return "-";

  // would skip over numbers if they existed -- but no

  // lame ppc64 has a '.' in front of every name
  if(*ret=='.') ret++;
  switch(*ret){
    case 's': if(!strncmp(ret, "sys_", 4)) ret += 4;   break;
    case 'd': if(!strncmp(ret, "do_",  3)) ret += 3;   break;
    case '_': while(*ret=='_') ret++;                  break;
  }
  return ret;
}

/***************************************/

static const symb fail = { .name = "?" };
static const char dash[] = "-";
static const char star[] = "*";

#define MAX_OFFSET (0x1000*sizeof(long))  /* past this is generally junk */

/* return pointer to temporary static buffer with function name */
const char * lookup_wchan(unsigned KLONG address, unsigned pid) {
  const symb *mod_symb;
  const symb *map_symb;
  const symb *good_symb;
  const char *ret;
  unsigned hash;

  // can't cache it due to a race condition :-(
  if(use_wchan_file) return read_wchan_file(pid);

  if(!address)  return dash;
  if(!~address) return star;

  read_and_parse();
  hash = (address >> 4) & 0xff;  /* got 56/63 hits & 7/63 misses */
  if(hashtable[hash].addr == address) return hashtable[hash].name;
  mod_symb = search(address, ksyms_index,  ksyms_count);
  if(!mod_symb) mod_symb = &fail;
  map_symb = search(address, sysmap_index, sysmap_count);
  if(!map_symb) map_symb = &fail;

  /* which result is closest? */
  good_symb = (mod_symb->addr > map_symb->addr)
            ? mod_symb
            : map_symb
  ;
  if(address > good_symb->addr + MAX_OFFSET) good_symb = &fail;

  /* good_symb->name has the data, but needs to be trimmed */
  ret = good_symb->name;
  // lame ppc64 has a '.' in front of every name
  if(*ret=='.') ret++;
  switch(*ret){
    case 's': if(!strncmp(ret, "sys_", 4)) ret += 4;   break;
    case 'd': if(!strncmp(ret, "do_",  3)) ret += 3;   break;
    case '_': while(*ret=='_') ret++;                  break;
  }
  /* if(!*ret) ret = fail.name; */  /* not likely (name was "sys_", etc.) */

  /* cache name after abbreviation */
  hashtable[hash].addr = address;
  hashtable[hash].name = ret;

  return ret;
}
