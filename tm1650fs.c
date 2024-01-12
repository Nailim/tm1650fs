#include <u.h>
#include <libc.h>

#include <ctype.h>

#include <fcall.h>
#include <thread.h>
#include <9p.h>


/* 9p filesystem functions */
typedef struct Devfile Devfile;

void	initfs(char *dirname);

void	fsstart(Srv *);
void	fsread(Req *r);
void	fswrite(Req *r);
void	fsend(Srv *);


/* I2C device functions */
void	openi2cdev(void);
void	initi2cdev(void);
void	deiniti2cdev(void);
void	closei2cdev(void);


/* program logic functions, called from fs functions using I2C functions */
char*	fsreadctl(Req *r);
char*	fswritectl(Req *r);

char*	fsreaddisp(Req *r);
char*	fswritedisp(Req *r);

int		getdispglyph(char c);
void	ctlset(void);
void	dispset(void);
void	glyphcls(void);
void	glypherr(void);

void	dispon(void);
void	dispoff(void);


struct Devfile {
	char	*name;
	char*	(*fsread)(Req*);
	char*	(*fswrite)(Req*);
	int	mode;
};

static int i2cfdarr[5];	/* I2C address array*/
static int dispstat[5];	/* display status array - [ascii|glyph] */


enum
{
	On,
	Off,
	CLS,
	Brightness,
};

static char *cmds[] = {
	[On]	= "on",
	[Off]	= "off",
	[CLS]	= "cls",
	[Brightness]	= "brightness",
	nil
};

Devfile files[] = {
	{ "ctl", fsreadctl, fswritectl, DMEXCL|0666 },
	{ "display", fsreaddisp, fswritedisp, DMEXCL|0666 },
};


Srv fs = {
	.start = fsstart,
	.read = fsread,
	.write = fswrite,
	.end = fsend,
};


void
initfs(char *dirname)
{
	File *devdir;
	int i;

	fs.tree = alloctree(nil, nil, 0555, nil);
	if(fs.tree == nil){
		sysfatal("initfs: alloctree: %r");
	}

	if((devdir = createfile(fs.tree->root, dirname, nil, DMDIR|0555, nil)) == nil){
		sysfatal("initfs: createfile: %s: %r", dirname);
	}

	for(i = 0; i < nelem(files); i++){
		if(createfile(devdir, files[i].name, nil, files[i].mode, files + i) == nil){
			sysfatal("initfs: createfile: %s: %r", files[i].name);
		}
	}
}


void
fsstart(Srv *)
{
	openi2cdev();
	initi2cdev();
}

void
fsread(Req *r)
{
	Devfile *f;

	r->ofcall.count = 0;
	f = r->fid->file->aux;
	respond(r, f->fsread(r));
}

void
fswrite(Req *r)
{
	Devfile *f;

	r->ofcall.count = 0;
	f = r->fid->file->aux;
	respond(r, f->fswrite(r));
}

void
fsend(Srv *)
{
	deiniti2cdev();
	closei2cdev();
}


char*
fsreadctl(Req *r)
{
	char out[11];

	memset(out, 0x20, 10);

	if(dispstat[0] & 0x01){
		out[0] = 'o';
		out[1] = 'n';
	} else {
		out[0] = 'o';
		out[1] = 'f';
		out[2] = 'f';
	}

	out[8] = ((dispstat[0] & 0x70) >> 4) + 0x30;	/* number to ascii */

	out[9] = 0x0A;
	out[10] = 0x00;

	readstr(r, out);
	return nil;
}

char*
fswritectl(Req *r)
{
	int si, i, cmd, para;
	char *s;

	/*
	On / off the display
	MSB                  LSB
	B7 B6 B5 B4 B3 B2 B1 B0 Explanation
	×              ×  ×  0  Off Display
	×              ×  ×  1  On Display
	
	Brightness settings
	MSB                  LSB
	B7 B6 B5 B4 B3 B2 B1 B0 Explanation
	×  0  0  0     ×  ×	    brightness level 8 
	×  0  0  1     ×  ×	    brightness level 1 
	×  0  1  0     ×  ×	    brightness level 2 
	×  0  1  1     ×  ×	    Brightness level 3 
	×  1  0  0     ×  ×	    brightness level 4 
	×  1  0  1     ×  ×	    brightness level 5 
	×  1  1  0     ×  ×	    brightness level 6 
	×  1  1  1     ×  ×	    brightness level 7 
	*/

	cmd = -1;

	si = 0;
	s = r->ifcall.data;

	/* clear whitespace before command */
	for(;(si<r->ifcall.count)&&(isspace(*s));){
		si++;
		s++;
	}

	/* search for command string */
	for(i=0; cmds[i]!=nil; i++){
		if(strncmp(cmds[i], s, strlen(cmds[i])) == 0){
			s = s + strlen(cmds[i]);
			cmd = i;
			break;
		}
	}

	switch (cmd)
	{
	case On:
		dispon();
		break;

	case Off:
		dispoff();
		break;
	
	case CLS:
		glyphcls();
		dispset();
	
	case Brightness:
		/* clear whitespace before parameter */
		for(;(si<r->ifcall.count)&&(isspace(*s));){
			si++;
			s++;
		}

		para = atoi(s);

		/* check if valid value for brightness */
		if((para < 0) || (para > 7)){
			glypherr();
			dispset();
		}

		/* set brightness - value on bits 4, 5, 6 */
		dispstat[0] = (dispstat[0] & (0xFF - 0x70)) | para<<4;
		ctlset();
		break;

	default:
		glypherr();
		dispset();
		break;
	}

	return nil;
}

char*
fsreaddisp(Req *r)
{
	int i, cc;
	char out[10];

	cc = 0;
	memset(out, 0, 10);

	for(i=1;i<5;i++){
		if((dispstat[i] & 0xFF00) == 0xFF00){
			/* aligment filler, skip */
			continue;
		}

		/* add char from disp status to output buffer */
		out[cc] = (dispstat[i] & 0xFF00) >> 8;

		if((dispstat[i] & 0x0080) == 0x0080){
			if((dispstat[i] & 0xFF00) != 0x2E00){
				/* if glyph on display has a dot, add additional character */
				cc++;
				out[cc] = 0x2E;
			}
		}

		cc++;
		out[cc] = 0x0A;
	}

	readstr(r, out);
	return nil;
}

char*
fswritedisp(Req *r)
{
	int gres, i, g;

	glyphcls();
	
	g = 0;

	for(i=0; i < r->ifcall.count; i++){
		if(r->ifcall.data[i] == 0x0A){
			/* end of line (LF/NL) in input buffer */
			break;
		}

		if(g > 4){
			/* no more space on the screen */
			glypherr();
			break;
		}

		gres = getdispglyph(r->ifcall.data[i]);

		if(gres == 0xFF00){
			/* unsupported character */
			glypherr();
			break;
		}

		g++;

		if(r->ifcall.data[i] == '.'){
			/* dot exception  */
			if(g > 1){
				if((dispstat[g-1] & 0x0080) != 0x0080){
					/* if there are more than one character already */
					/* and it doesnt have a dot, add teh dot to previous character */
					g--;
					gres = dispstat[g] + 0x0080;
				}
			}
		}

		dispstat[g] = gres;	
	}


	while(dispstat[4] == 0x0000){
		/* space left on screen -> shift (right aligned) and fill with space */
		dispstat[4] = dispstat[3];
		dispstat[3] = dispstat[2];
		dispstat[2] = dispstat[1];
		dispstat[1] = 0xFF00;
	}

    dispset();

	return nil;
}


int
getdispglyph(char c)
{
	int glyph;

	/*
	Segment values
	MSB                  LSB
	B7 B6 B5 B4 B3 B2 B1 B0
	DP G  F  E  D  C  B  A
	
		A
	F		B
		G
	E		C
		D		DP

		0x01
	0x20	0x02
		0x40
	0x10	0x04
		0x08
				0x80
 	*/

	/* whole alphabet */

	switch (c)
	{
	/* symbols */
	case 0x20:	/*   */
		glyph = 0x2000;
		break;
	case 0x21:	/* ! */
		glyph = 0x216B;
		break;
	case 0x2B:	/* + */
		glyph = 0x2B46;
		break;
	case 0x2D:	/* - */
		glyph = 0x2D40;
		break;
	case 0x2E:	/* . */
		glyph = 0x2E80;
		break;
	case 0x2F:	/* / */
		glyph = 0x2F52;
		break;
	case 0x3D:	/* = */
		glyph = 0x3D48;
		break;
	case 0x3F:	/* ? */
		glyph = 0x3F4B;
		break;
	case 0x5B:	/* [ */
		glyph = 0x5B39;
		break;
	case 0x5C:	/* \ */
		glyph = 0x5C64;
		break;
	case 0x5D:	/* ] */
		glyph = 0x5D0F;
		break;
	case 0x5F:	/* _ */
		glyph = 0x5F08;
		break;
	

	/* numbers */
	case 0x30:	/* 0 */
		glyph = 0x303F;
		break;
	case 0x31:	/* 1 */
		glyph = 0x3106;
		break;
	case 0x32:	/* 2 */
		glyph = 0x325B;
		break;
	case 0x33:	/* 3 */
		glyph = 0x334F;
		break;
	case 0x34:	/* 4 */
		glyph = 0x3466;
		break;
	case 0x35:	/* 5 */
		glyph = 0x356D;
		break;
	case 0x36:	/* 6 */
		glyph = 0x367D;
		break;
	case 0x37:	/* 7 */
		glyph = 0x3707;
		break;
	case 0x38:	/* 8 */
		glyph = 0x387F;
		break;
	case 0x39:	/* 9 */
		glyph = 0x396F;
		break;
	
	/* upper case letters */
	case 0x41:	/* A */
		glyph = 0x4177;
		break;
	case 0x42:	/* B */
		glyph = 0x427F;
		break;
	case 0x43:	/* C */
		glyph = 0x4339;
		break;
	case 0x44:	/* D */
		glyph = 0x441F;
		break;
	case 0x45:	/* E */
		glyph = 0x4579;
		break;
	case 0x46:	/* F */
		glyph = 0x4671;
		break;
	case 0x47:	/* G */
		glyph = 0x473D;
		break;
	case 0x48:	/* H */
		glyph = 0x4876;
		break;
	case 0x49:	/* I */
		glyph = 0x4930;
		break;
	case 0x4A:	/* J */
		glyph = 0x4A1E;
		break;
	case 0x4B:	/* K */
		glyph = 0x4B75;
		break;
	case 0x4C:	/* L */
		glyph = 0x4C38;
		break;
	case 0x4D:	/* M */
		glyph = 0x4D2B;
		break;
	case 0x4E:	/* N */
		glyph = 0x4E37;
		break;
	case 0x4F:	/* O */
		glyph = 0x4F3F;
		break;
	case 0x50:	/* P */
		glyph = 0x5073;
		break;
	case 0x51:	/* Q */
		glyph = 0x516B;
		break;
	case 0x52:	/* R */
		glyph = 0x527B;
		break;
	case 0x53:	/* S */
		glyph = 0x536D;
		break;
	case 0x54:	/* T */
		glyph = 0x5431;
		break;
	case 0x55:	/* U */
		glyph = 0x553E;
		break;
	case 0x56:	/* V */
		glyph = 0x562E;
		break;
	case 0x57:	/* W */
		glyph = 0x571D;
		break;
	case 0x58:	/* X */
		glyph = 0x5849;
		break;
	case 0x59:	/* Y */
		glyph = 0x596A;
		break;
	case 0x5A:	/* Z */
		glyph = 0x5A5B;
		break;
	
	/* lower case letters */
	case 0x61:	/* a */
		glyph = 0x615F;
		break;
	case 0x62:	/* b */
		glyph = 0x627c;
		break;
	case 0x63:	/* c */
		glyph = 0x63;
		break;
	case 0x64:	/* d */
		glyph = 0x645E;
		break;
	case 0x65:	/* e */
		glyph = 0x6579;
		// glyph = 0x6518;
		break;
	case 0x66:	/* f */
		glyph = 0x6671;
		// glyph = 0x6670;
		break;
	case 0x67:	/* g */
		glyph = 0x673D;
		// glyph = 0x6759;
		break;
	case 0x68:	/* h */
		glyph = 0x6874;
		break;
	case 0x69:	/* i */
		glyph = 0x6911;
		break;
	case 0x6A:	/* j */
		glyph = 0x6A0D;
		break;
	case 0x6B:	/* k */
		// glyph = 0x4B75;
		glyph = 0x6B69;
		break;
	case 0x6C:	/* l */
		glyph = 0x6C30;
		break;
	case 0x6D:	/* m */
		glyph = 0x6D55;
		break;
	case 0x6E:	/* n */
		glyph = 0x6E54;
		break;
	case 0x6F:	/* 0 */
		glyph = 0x6F5c;
		break;
	case 0x70:	/* p */
		glyph = 0x7073;
		break;
	case 0x71:	/* q */
		glyph = 0x7176;
		break;
	case 0x72:	/* r */
		glyph = 0x7250;
		break;
	case 0x73:	/* s */
		glyph = 0x732D;
		break;
	case 0x74:	/* t */
		glyph = 0x7478;
		break;
	case 0x75:	/* u */
		glyph = 0x751C;
		break;
	case 0x76:	/* v */
		glyph = 0x762A;
		break;
	case 0x77:	/* w */
		glyph = 0x776A;
		break;
	case 0x78:	/* x */
		glyph = 0x5814;
		break;
	case 0x79:	/* y */
		glyph = 0x796E;
		break;
	case 0x7A:	/* z */
		glyph = 0x7A1B;
		break;

	default:
		glyph = 0xFF00;
		break;
	}

	return glyph;
}

void
ctlset()
{
	pwrite(i2cfdarr[0], &dispstat[0], 1, 0);
}
void

dispset()
{
	pwrite(i2cfdarr[1], &dispstat[1], 1, 0);
    pwrite(i2cfdarr[2], &dispstat[2], 1, 0);
    pwrite(i2cfdarr[3], &dispstat[3], 1, 0);
    pwrite(i2cfdarr[4], &dispstat[4], 1, 0);
}

void
glyphcls()
{
	dispstat[1] = 0x0000;	/* clear segment 1 */
	dispstat[2] = 0x0000;	/* clear segment 2 */
	dispstat[3] = 0x0000;	/* clear segment 3 */
	dispstat[4] = 0x0000;	/* clear segment 4 */
}

void
glypherr()
{
	dispstat[1] = 0x4579;	/* segment 1  to E */
	dispstat[2] = 0x7250;	/* segment 2  to r */
	dispstat[3] = 0x7250;	/* segment 3  to r */
	dispstat[4] = 0x216B;	/* segment 4  to ! */
}


void
dispon(void)
{
	/* turn on diplay - command on bit 0 */
	dispstat[0] = (dispstat[0] & (0xFF - 0x01)) | 0x01;
	//pwrite(i2cfdarr[0], &dispstat[0], 1, 0);
	ctlset();
}

void
dispoff(void)
{
	/* turn on diplay - command on bit 0 */
	dispstat[0] = (dispstat[0] & (0xFF - 0x01)) | 0x00;
	//pwrite(i2cfdarr[0], &dispstat[0], 1, 0);
	ctlset();
}


void
openi2cdev(void)
{
	i2cfdarr[0] = -1;
	i2cfdarr[1] = -1;
	i2cfdarr[2] = -1;
	i2cfdarr[3] = -1;
	i2cfdarr[4] = -1;

	/* ctrl - default location fot tm1650 ix 0x24*/
    if(access("/dev/i2c1/i2c.24.data", 0) != 0){
		if(bind("#J24", "/dev", MBEFORE) < 0){
		    sysfatal("no J24 device");
        }
    }

	i2cfdarr[0] = open("/dev/i2c1/i2c.24.data", ORDWR);
	if(i2cfdarr[0] < 0){
		sysfatal("cannot open i2c.24.data file");
    }

	/* segment 1 - default location fot tm1650 ix 0x34*/
	if(access("/dev/i2c1/i2c.34.data", 0) != 0){
		if(bind("#J34", "/dev", MBEFORE) < 0){
		    sysfatal("no J34 device");
        }
    }
	i2cfdarr[1] = open("/dev/i2c1/i2c.34.data", ORDWR);
	if(i2cfdarr[1] < 0){
		sysfatal("cannot open i2c.34.data file");
    }

	/* segment 2 - default location fot tm1650 ix 0x35*/
	if(access("/dev/i2c1/i2c.35.data", 0) != 0){
		if(bind("#J35", "/dev", MBEFORE) < 0){
		    sysfatal("no J35 device");
        }
    }
	i2cfdarr[2] = open("/dev/i2c1/i2c.35.data", ORDWR);
	if(i2cfdarr[2] < 0){
		sysfatal("cannot open i2c.35.data file");
    }

	/* segment 3 - default location fot tm1650 ix 0x36*/
	if(access("/dev/i2c1/i2c.36.data", 0) != 0){
		if(bind("#J36", "/dev", MBEFORE) < 0){
		    sysfatal("no J36 device");
        }
    }
	i2cfdarr[3] = open("/dev/i2c1/i2c.36.data", ORDWR);
	if(i2cfdarr[3] < 0){
		sysfatal("cannot open i2c.36.data file");
    }

	/* segment 4 - default location fot tm1650 ix 0x37*/
	if(access("/dev/i2c1/i2c.37.data", 0) != 0){
		if(bind("#J37", "/dev", MBEFORE) < 0){
		    sysfatal("no J37 device");
        }
    }
	i2cfdarr[4] = open("/dev/i2c1/i2c.37.data", ORDWR);
	if(i2cfdarr[4] < 0){
		sysfatal("cannot open i2c.37.data file");
    }
}

void
initi2cdev(void)
{
	dispstat[0] = 0x0000;

	glyphcls();
    dispset();

	dispon();
}


void
deiniti2cdev(void)
{
	glyphcls();
    dispset();

	dispoff();
}

void
closei2cdev(void)
{
	close(i2cfdarr[0]);

	unmount("#J24", "/dev");

	unmount("#J34", "/dev");
	unmount("#J35", "/dev");
	unmount("#J36", "/dev");
	unmount("#J37", "/dev");
}


void
usage(void)
{
	fprint(2, "usage: %s [-m mntpt] [-s srvname]\n", argv0);
	exits("usage");
}


void
threadmain(int argc, char *argv[])
{
	char *srvname, *mntpt;

	srvname = "tm1650";
	mntpt = "/mnt";

	ARGBEGIN {
	case 'm':
		mntpt = ARGF();
		break;
	case 's':
		srvname = ARGF();
		break;
	default:
		usage();
	} ARGEND

	initfs(srvname);

	threadpostmountsrv(&fs, srvname, mntpt, MBEFORE);

	threadexits(nil);
}
