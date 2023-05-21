</$objtype/mkfile

ALL=tm1650fs

all:V:	$ALL

tm1650fs:		tm1650fs.$O
	$LD $LDFLAGS -o tm1650fs tm1650fs.$O

tm1650fs.$O:	tm1650fs.c
	$CC $CFLAGS tm1650fs.c

clean:V:
	rm -f *.$O tm1650fs

